/* Code for FREENOVE ESP32-S3 WROOM 1 chip equipped with a INMP441 microphone */
#include <Arduino.h>

#include <AsyncTelegram2.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include "camera.h"
#include "config.h"
#include "utils.h"
#include "driver/temp_sensor.h"
#include <WiFiClientSecure.h>
#include <nvs_flash.h>
#include <cmath>
#include <mutex>
#include <vector>
#include "microphone.h"
#include "SD_MMC.h"

#define SD_MMC_CMD 38 //Please do not modify it.
#define SD_MMC_CLK 39 //Please do not modify it. 
#define SD_MMC_D0  40 //Please do not modify it.

enum ALARM_TYPE {
    AUDIO,
    PHOTO
};

class AudioData {
public:
    int amp = 0;
    int freq = 0;
    AudioData() {};
    AudioData(AudioData &ad) {  
        this->amp = ad.amp;
        this->freq = ad.freq;
    }  
};

WiFiClientSecure client; //global ssl connection object
AsyncTelegram2 tgbot(client);
int lastmsgID;
bool enable_detection;
bool night_mode;
bool photoflash;
int photoLastID;
bool audio_detection = false;
uint8_t prev_img[30720] = {0,}; // (640*480)/10
unsigned int changes = 0; // How many pixel changed in two consecutive photos
TaskHandle_t task_mic; //background task for microphone
AudioData audio_detected; // greater than 0 for successful detection
bool debug = false;
int pix_diff_threshold = 50;
int brightness_threshold = 25; // Minimum average pixel brightness to be considered in day mode
unsigned long boot_time;
short alarms = 0; //alarm counter
unsigned long last_alarm_sent = 0;
bool stop_too_dark = false; //stop detection because it's too dark
unsigned long failed_connection_attempts; // Failed connections attempts to wifi or telegram server
#define DEFAULT_AMP_THRESHOLD 40 // Signal with an average amplitude greater than the threshold triggers an alarm
#define DEFAULT_FREQ_THRESHOLD 8 // Signal with an average frequency minor than the threshold (high freq) triggers an alarm
int audio_amp = DEFAULT_AMP_THRESHOLD;
int audio_freq = DEFAULT_FREQ_THRESHOLD;
short min_hour, max_hour = -1; // For time-based detection, set time interval

/* Function signatures */
void saveSettings();
void reboot();
void sendAlarm(ALARM_TYPE at);
void sendPhoto();
bool detectMotion();
void handleCommands();
bool isNight();
void sendMessage(String message);
AudioData audioDetection();
void sendAudio(int seconds);
/* --- */


// Arduino code by default runs on CORE 1
// Create a parallel function to run on CORE 0
void task_worker( void * parameter) {
    // Not used anymore because intensive background task created issues with wifi connection
}
// --- SETUP
void initTempSensor(){
    temp_sensor_config_t temp_sensor = TSENS_CONFIG_DEFAULT();
    temp_sensor.dac_offset = TSENS_DAC_L2;  // TSENS_DAC_L2 is default; L4(-40°C ~ 20°C), L2(-10°C ~ 80°C), L1(20°C ~ 100°C), L0(50°C ~ 125°C)
    temp_sensor_set_config(temp_sensor);
    temp_sensor_start();
}

void setup() {
    delay(500);
    Serial.begin(115200);
    if (debug) {
        neopixelWrite(RGB_BUILTIN, 0, RGB_BRIGHTNESS, 0);  // Green
        delay(1000);
        neopixelWrite(RGB_BUILTIN, 0, 0, 0);  // Off / black
    }

    boot_time = millis();

    #ifdef DEBUG
    dumpChipInfo();
    #endif
    initTempSensor();

    #ifdef USE_MICROSD
    // Initialize SD card if present
    mem.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
    if (!mem.begin("/sdcard", true, true, SDMMC_FREQ_DEFAULT, 5)) {
      Serial.println("Card Mount Failed");
      return;
    }
    uint8_t cardType = mem.cardType();
    if(cardType == CARD_NONE){
        Serial.println("No SD_MMC card attached");
        return;
    }

    Serial.print("SD_MMC Card Type: ");
    if(cardType == CARD_MMC){
        Serial.println("MMC");
    } else if(cardType == CARD_SD){
        Serial.println("SDSC");
    } else if(cardType == CARD_SDHC){
        Serial.println("SDHC");
    } else {
        Serial.println("UNKNOWN");
    }

    uint64_t cardSize = mem.cardSize() / (1024 * 1024);
    Serial.printf("SD_MMC Card Size: %lluMB\n", cardSize);
    #else
    // Initialize Littlefs
    if (!mem.begin(true)) {
        log("An Error has occurred while mounting LittleFS - restarting");
        delay(200);
        reboot(); // restart and try again
    } else {
        log("SPI flash Total bytes: " + String(mem.totalBytes()));
        log("SPI flash Used bytes: " + String(mem.usedBytes()));
        //listDir(LittleFS, "/", 1);
    }
    #endif
    Settings settings;
    if (!settings.open()) {
        log("ERROR: Cannot open settings!");
        reboot();
    } else {
        // load all saved values
        enable_detection = settings.read("detection", false);
        photoflash = settings.read("photoflash", false);
        photoLastID = settings.read("photoLastID", 0);
        night_mode = settings.read("nightMode", false);
        lastmsgID = settings.read("lastmsgID", -1);
        debug = settings.read("debug", false);
        audio_detection = settings.read("audio_detection", false);
        failed_connection_attempts = settings.read("failed_connection_attempts", 0);
        audio_amp = settings.read("audio_amp", DEFAULT_AMP_THRESHOLD);
        audio_freq = settings.read("audio_freq", DEFAULT_FREQ_THRESHOLD);
        min_hour = settings.read("min_hour", -1); //hours since midnight 0 - 23
        max_hour = settings.read("max_hour", -1);
        brightness_threshold = settings.read("brightness_threshold", 25);
        settings.close();
    }

    log("Init camera..");
    // Initialize camera config
    camera_config_t config;
    initCameraConfig(config);
    // camera init
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x Please check if the camera is connected well.", err);
        reboot();
    }

    // Get a reference to the sensor
    sensor_t* s = esp_camera_sensor_get();

    // Dump camera module, warn for unsupported modules.
    switch (s->id.PID) {
        case OV9650_PID: log("WARNING: OV9650 camera module is not properly supported, will fallback to OV2640 operation"); break;
        case OV7725_PID: log("WARNING: OV7725 camera module is not properly supported, will fallback to OV2640 operation"); break;
        case OV2640_PID: log("OV2640 camera module detected"); break;
        case OV3660_PID: log("OV3660 camera module detected"); break;
        case OV5640_PID: log("OV5640 camera module detected"); break;
        default: log("WARNING: Camera module is unknown and not properly supported, will fallback to OV2640 operation");
    }

    log("Starting wifi..");
    // Start WiFi connection
    log("Connecting to " + String(ssid));
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    int attempts = 12;
    neopixelWrite(RGB_BUILTIN, 10, 0, 0);
    while ((WiFi.status() != WL_CONNECTED) && (attempts > 0)) {
        log("Status: "+get_wifi_status(WiFi.status()));
        delay(2000);
        Serial.print(".");
        attempts--;
    }
    if (attempts == 0) {
        failed_connection_attempts++;
        reboot();
    }
    neopixelWrite(RGB_BUILTIN, 0, 0, 0);  // Off / black
    log("");
    log("WiFi connected");
    log(WiFi.localIP().toString());

    // Sync time with NTP
    configTzTime(MYTZ, "time.google.com", "time.windows.com", "pool.ntp.org");
    client.setInsecure();

    client.setTimeout(12000);

    // Set the Telegram bot properties
    tgbot.setUpdateTime(1000);
    tgbot.setTelegramToken(token);

    // Check if all things are ok
    log("\nTest Telegram connection... ");
    bool tg_res = tgbot.begin();
    delay(5000);
    if (tg_res) {
        log("OK");
    } else {
        failed_connection_attempts++;
        log("NOK");
        delay(3000);
        reboot();
    }

    enable_detection ? log("Detection Enabled") : log("Detection Disabled");

    #ifdef USE_MICROPHONE
    // start up the I2S peripheral
    if (!i2s_init()) {
        log("Failed to init i2s!");
        audio_detection = false;
    }
    #endif

    log("Setup done");
}

void loop() {
    //Serial.println("loop function");
    unsigned long start, end;
    // check wifi and telegram connection status
    int attempts = 3;
    while ((WiFi.status() != WL_CONNECTED) && (attempts > 0)) {
        log("Status: "+get_wifi_status(WiFi.status()));
        delay(2000);
        Serial.print(".");
        attempts--;
    }
    if (attempts == 0) {
        neopixelWrite(RGB_BUILTIN, 10, 0, 0);
        delay(100);
        reboot();
    }
    if (!tgbot.checkConnection()) {
        log("Disconnected from telegram");
        reboot();
    }
    neopixelWrite(RGB_BUILTIN, 0, 0, 0);

    // send failed connection attempt count
    if (failed_connection_attempts > 0) {
        failed_connection_attempts = 0;
        sendMessage("Established connection to telegram after "+String(failed_connection_attempts)+" failed attempts");
    }

    // check chip temperature
    float result = 0;
    temp_sensor_read_celsius(&result);
    if (result > 78) {
        String tm = "Temperature too high! "+String(result)+" °C";
        log(tm);
        sendMessage(tm);
        // Sort of protection to cool down
        esp_deep_sleep_start();
    }

    // check environment light (day or night)
    start = millis();
    // reset sensor config (default: day mode)
    setSensorMode(true);
    // check environment brightness
    if (isNight()) {
        log("Set detection into night mode");
        //put sensor into night mode
        setSensorMode(false);
        // reset prev image pixels
        memset(prev_img, 0, sizeof(prev_img));
        //check again if it's too dark
        if (isNight()) {
            if (enable_detection) {
                //too dark, we cannot perform any detection
                enable_detection = false;
                stop_too_dark = true;
                log("Detection disabled, it's too dark");
            }
        } else { //the photo is not too dark, we can perform detection
            if (stop_too_dark) { // if we stop before bc it was too dark, then re-enable detection
                stop_too_dark = false;
                enable_detection = true;
                log("Detection enabled, it's not dark");
            }
            pix_diff_threshold = 30;
            if ((night_mode) && (!enable_detection)) {
                log("Night mode: enabling detection");
                enable_detection = true;
            }
        }
    } else {
        if (stop_too_dark) { // if we stop before bc it was too dark, then re-enable detection
            stop_too_dark = false;
            enable_detection = true;
            log("Detection enabled, it's not dark");
        }
        log("Set detection into day mode");
        // reset prev image pixels
        memset(prev_img, 0, sizeof(prev_img));
        pix_diff_threshold = 40;
        if (night_mode && enable_detection) {
            log("Night mode: disabling detection");
            enable_detection = false;
        }
    }
    end = millis();
    Serial.printf("Environment brightness task: %lu ms\n", (end-start));

    // handle new commands
    start = millis();
    handleCommands();
    end = millis();
    Serial.printf("Handled command in %lu ms\n", (end-start));

    //update time
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        Serial.println("Failed to obtain time");
    } else {
        Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
    }
 
    if ((enable_detection) && // detection is enabled?
        // Check if we have some time interval configured
        (((min_hour < 0) && (max_hour < 0)) || ((timeinfo.tm_hour >= min_hour) && (timeinfo.tm_hour <= max_hour)))
        ) {
        start = millis();
        log("detecting motion...");
        detectMotion(); //initialize prev img
        bool confident_detection = true;
        for(int i = 0; i < 1; i++) {
            #ifndef USE_MICROPHONE
            delay(1000);
            #else
            // here we need some delay between the two photos. We can listen the microphone performing sound detection
            // in order to have a more optimized overall detection
            if (audio_detection) {
                // Listen to detect sounds
                audio_detected = audioDetection();
                if ((audio_detected.freq > 0) || (audio_detected.amp > 0)) {
                    sendAlarm(ALARM_TYPE::AUDIO);
                }
            } else {
                delay(1000);
            }
            #endif
            confident_detection = confident_detection && detectMotion();
        }
        if (confident_detection) {
            sendAlarm(ALARM_TYPE::PHOTO);
        }
        end = millis();
        Serial.printf("Detection performed in %lu ms\n", (end-start));
    } else if (audio_detection) {
        // Listen to detect sounds
        audio_detected = audioDetection();
        if ((audio_detected.freq > 0) || (audio_detected.amp > 0)) {
            sendAlarm(ALARM_TYPE::AUDIO);
        } 
    } else {
        delay(1000);
    }
    Serial.println("end loop");
}

// AUDIO ------

void sendAudio(int seconds) {
    bool prev = audio_detection;
    audio_detection = false; //temporary disable audio detection
    String filename = "/audio.wav";
    if (mem.exists(filename)) {
        mem.remove(filename);
    }
    delay(1000);
    if (freespaceAvailable() && !audio_detection) { // check if we have enough space
        // Microphone it's also used for audio detection in the background task running on CORE 0
        File f = mem.open(filename, FILE_WRITE);
        recordAudio(f, seconds);
        f.close();
    } else {
        sendMessage("Not enough space to store audio or audio detection enabled");
        audio_detection = prev;
        return;
    }
    audio_detection = prev;
    
    Serial.println("Sending to telegram");
    File fwr = mem.open(filename, FILE_READ);
    if (!fwr) {
        Serial.println("Error while opening fwr");
    } else {
        Serial.println("Sending "+filename+" with size of "+String(fwr.size()));
    }
    String unique_name = "Audio_" + String(micros()) + ".wav";
    tgbot.sendDocument(userid, fwr, fwr.size(), AsyncTelegram2::BINARY, unique_name.c_str(), "audio");
    delay(3000);
    fwr.close();
}

// ------ AUDIO DETECTION ------------
AudioData audioDetection() {
    // Listen the mic looking for loud sounds greater than thresholds
    AudioData res;
    res.amp = 0;
    res.freq = 0;
    size_t numBytesRead;
    size_t buf_len = 32000; // number of audio samples to capture from mic
    uint8_t *buffer = (uint8_t*)malloc(buf_len);
    if (!buffer) {
        log("Error: cannot allocate memory");
        return res;
    }
    // Read data from DMA buffers into our copy buffer
    i2s_read(I2S_NUM_0, (void*)buffer, buf_len, &numBytesRead, portMAX_DELAY);
    const int16_t *samples = (const int16_t *)buffer;
    int num_samples = buf_len / sizeof(int16_t);
    int16_t maxsample = INT16_MIN, minsample = INT16_MAX, abssample;
    int avg_amp = 0;
    int16_t avg_freq, freq_count, freq = 0;
    for (int i = 0; i < num_samples; i++) {
        minsample = min(minsample, samples[i]);
        maxsample = max(maxsample, samples[i]);
        //abssample = abs(samples[i]);
        // Consider only positive samples (they are half of num_samples)
        if (samples[i] > 0) {
            avg_amp += samples[i];
            freq++;
        } else {
            if (freq > 0) { // passing from positive values to negative
                avg_freq += freq;
                freq_count++;
            }
            freq = 0; // reset freq
        }
    }
    avg_freq /= freq_count;
    avg_amp /= (num_samples/2);
    //Serial.printf("Audio debug: %d samples, %d min, %d max, %d amp, %d avg\n", num_samples, minsample, maxsample, amp, avg);
    if (debug) log("Avg Amplitude: "+String(avg_amp)+", Avg Frequency:"+String(avg_freq));
    if ((avg_freq < audio_freq) || // if we have a signal with high frequency probably it's an acoustic alarm, we must detect it
        (avg_amp > audio_amp)) { // otherwise filter the anomaly amplitudes using a threshold
        // Sound detected!
        res.amp = avg_amp;
        res.freq = avg_freq;
    }

    if ((res.freq+res.amp > 0) && (freespaceAvailable())) {
        String filename = "/detection.wav";
        if (mem.exists(filename)) {
            mem.remove(filename);
        }
        log("Saving " + filename);
        // Save to file
        const int audio_size = buf_len;
        WAVHeader wavHeader;
        initializeWAVHeader(wavHeader, audio_size);
        File f = mem.open(filename, FILE_WRITE);
        f.write(reinterpret_cast<const uint8_t*>(&wavHeader), sizeof(wavHeader));
        f.write((const byte*)buffer, buf_len);
        f.close();
    }

    free(buffer);

    return res;
}
// ------ MOTION DETECTION -----------
bool isNight() {
    camera_fb_t * fb = NULL;
    // Dispose the first photo
    fb = esp_camera_fb_get();
    esp_camera_fb_return(fb);
    delay(200);

    fb = esp_camera_fb_get();
    if (!fb) {
        log("Camera capture failed");
        esp_camera_fb_return(fb);
        return false;
    }

    //log("Converting jpg to RGB");
    size_t rgb_len = fb->width*fb->height*3; //3d array
    uint8_t *rgb_buf =  (uint8_t *)malloc(rgb_len);
    if (rgb_buf == NULL) {
        log("rgb_buf malloc failed!");
        esp_camera_fb_return(fb);
        return false;
    }
    bool jpeg_converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb_buf);
    if (!jpeg_converted) {
        log("ERROR: Cannot convert jpg to RGB");
    }
    size_t j = 0;
    changes = 0;
    unsigned int pix_avg = 0;
    for (size_t i = 0; i < rgb_len; i+=60) {
        // pixel values from 0 to 255
        unsigned int pixel_blue = rgb_buf[i];
        unsigned int pixel_green = rgb_buf[i+1];
        unsigned int pixel_red = rgb_buf[i+2];
        pix_avg += (pixel_blue + pixel_red + pixel_green) / 3;
        j++;
    }
    free(rgb_buf);
    pix_avg = pix_avg/j;
    // Clear buffer
    esp_camera_fb_return(fb);
    #ifdef DEBUG
    Serial.println("Night mode checking, avg brightness: "+String(pix_avg));
    #endif
    return (pix_avg < brightness_threshold);
}

bool detectMotion() {
    // Detect motion comparing pixels average value to previous photo
    // N.B. We don't need to compare every pixel, just some of them are enough
    camera_fb_t * fb = NULL;
    // Dispose the first photo
    fb = esp_camera_fb_get();
    esp_camera_fb_return(fb);

    if (photoflash) {
        neopixelWrite(RGB_BUILTIN, 255, 255, 255);  // White / Flash
        delay(100);        
        fb = esp_camera_fb_get();
        delay(50);
        neopixelWrite(RGB_BUILTIN, 0, 0, 0);  // Off / black
    } else {
        fb = esp_camera_fb_get();
    }

    if (!fb) {
        log("Camera capture failed");
        esp_camera_fb_return(fb);
        return false;
    }

    //log("Converting jpg to RGB");
    size_t rgb_len = fb->width*fb->height*3; //3d array
    uint8_t *rgb_buf =  (uint8_t *)malloc(rgb_len);
    if (rgb_buf == NULL) {
        log("rgb_buf malloc failed!");
        esp_camera_fb_return(fb);
        return false;
    }
    bool jpeg_converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb_buf);
    if (!jpeg_converted) {
        log("ERROR: Cannot convert jpg to RGB");
    }
    size_t j = 0;
    changes = 0;
    for (size_t i = 0; i < rgb_len; i+=3) {
        // pixel values from 0 to 255
        unsigned int pixel_blue = rgb_buf[i];
        unsigned int pixel_green = rgb_buf[i+1];
        unsigned int pixel_red = rgb_buf[i+2];
        unsigned int pix_avg = (unsigned int)((float)(pixel_blue + pixel_red + pixel_green) / 3);
        //we don't need to store the value of each pixel, but only few pixels are enough (save memory)
        if (i % 10 == 0) { //skip 30 pixels
            if (prev_img[j] != 0) { //0 is the default value, skip the comparation if prev_img[j] is 0
                int pix_diff = pix_avg - prev_img[j];
                pix_diff = pix_diff > 0 ? pix_diff : pix_diff*-1;
                if (pix_diff > pix_diff_threshold) {
                    if (debug) {
                        // For debug purposes, mark as red pixel
                        rgb_buf[i+2] = 255;
                        rgb_buf[i+1] = 0;
                        rgb_buf[i] = 0;
                    }
                    changes++;
                }
            }
            prev_img[j] = pix_avg;
            j++;
        }
    }
    uint8_t *jpg_out = NULL;
    size_t out_len = 0;
    float perc_changes = ((float)changes/j)*100; // j=48000
    if (debug) {
        log("Found "+String(perc_changes)+" changes", (perc_changes == 0));
        fmt2jpg(rgb_buf, rgb_len, fb->width, fb->height, PIXFORMAT_RGB888, 90, &jpg_out, &out_len);
    } else {
        jpg_out = fb->buf;
        out_len = fb->len;
    }
    free(rgb_buf);
    // Check for motion detection
    String msg = "";
    int min_changes = 5; //other configurable threshold, percentage of changed pixels
    bool motion = false;
    if (perc_changes > min_changes) {
        // Motion detected
        motion = true;
        log("Motion detected");
        msg = "MOTION DETECTED. ";
        size_t len = fb->len;
        log("Saving photo of size: "+String(len));
        photoLastID = ++photoLastID % MAX_PHOTO_SAVED;
        String photoname = "/photo"+String(photoLastID)+".jpg";
        // delete old image file if it exists
        if (mem.exists(photoname)) {
            Serial.println(photoname + " already exists, overwriting..");
            mem.remove(photoname);
        }
        // save the new image
        File file = mem.open(photoname, FILE_WRITE);
        if (!file) {
            log("Failed to create file");
        } else {
            if (file.write(jpg_out, out_len)) {
                log("The picture " + String(photoLastID) + " has been saved");
            } else {
                log("Error: writing image, retrying..");
                delay(300);
                if (file.write(jpg_out, out_len)) {
                    log("The picture " + String(photoLastID) + " has been saved");
                } else {
                    log("Sending image via telegram without saving");
                    //Send image directly to telegram and reboot
                    tgbot.sendPhoto(userid, jpg_out, out_len, "MOTION DETECTED. Image not saved!");
                    delay(2000);
                    reboot();
                }
            }
            file.close();
        }
    }
    if (debug) {
        free(jpg_out);
    }
    // Clear buffer
    esp_camera_fb_return(fb);
    return motion;
}

// ---------- TELEGRAM FUNCTIONS ------

void sendMessage(String message) {
    log("Sending message on telegram: "+message);
    tgbot.sendTo(userid, String(CAMID) + ": "+message);
    delay(1000);
}

void sendPhoto() {
    // Send a photo via telegram without saving it on filesystem

    log("Camera capture requested");
    camera_fb_t * fb = NULL;
    // Dispose the first photo to force an update
    fb = esp_camera_fb_get();
    esp_camera_fb_return(fb);
    delay(200);
    
    if (photoflash) {
        neopixelWrite(RGB_BUILTIN, 255, 255, 255);  // White / Flash
        delay(100);        
        fb = esp_camera_fb_get();
        delay(50);
        neopixelWrite(RGB_BUILTIN, 0, 0, 0);  // Off / black
    } else {
        fb = esp_camera_fb_get();
    }
    delay(200);

    if (!fb) {
        log("Camera capture failed");
        return;
    }
    String msg = "Photo from " CAMID;
    tgbot.sendPhoto(userid, fb->buf, fb->len, msg.c_str());
    // Clear buffer
    esp_camera_fb_return(fb);
}

void handleCommands() {
    // A variable to store telegram message data
    TBMessage msg;
    // if there is an incoming message...
    MessageType msgType = tgbot.getNewMessage(msg);
    if (msgType) {
        log("Received "+ String(msg.messageID) + " - " + msg.text + ", last command id was " +String(lastmsgID));
        // we must check if the last command is different from the current command
        // unfortunately, the library fetches always the last command even if it was already read
        if ((msgType == MessageText) && (msg.messageID != lastmsgID)) {
            lastmsgID = msg.messageID;
            log("New message: "+msg.text);
            // Check if the message is a private message (format: /CAMID:command or /GROUP:command)
            int pos = msg.text.indexOf(':');
            if (pos > 0) {
                String camName = msg.text.substring(1, pos);
                String command = "/"+msg.text.substring(pos+1);
                if ((camName == CAMID) || (camName == GROUPID)) {
                    log("Received a private message, command: " + command);
                    msg.text = command;
                } else {
                    log(camName + " is not the ID of this camera, expected: " CAMID);
                    return;
                }
            }
            // Received a text message
            if ((msg.text.startsWith("/photo")) || (msg.text.startsWith("/getphoto"))) {
                short num = -1;
                int separator = msg.text.indexOf(" ");
                if (separator > 0) {
                    num = msg.text.substring(separator+1).toInt();
                }
                log("Sending Photo from CAM");
                if (num < 0) {
                    sendPhoto();
                } else {
                    String photoname = "/photo"+String(num)+".jpg";
                    if (mem.exists(photoname)) {
                        String msg = "Photo " + photoname + " from " CAMID;
                        tgbot.sendPhoto(userid, photoname.c_str(), mem, msg.c_str());
                    } else {
                        sendMessage(photoname+" not found");
                    }
                }
            } else if (msg.text.startsWith("/getaudio")) {
                log("Sending recording to telegram");
                short num = 5;
                int separator = msg.text.indexOf(" ");
                if (separator > 0) {
                    num = msg.text.substring(separator+1).toInt();
                }
                sendAudio(num);
            } else if (msg.text.startsWith("/setaudio")) {
                int separator = msg.text.indexOf(" ");
                if (separator > 0) {
                    String interval = msg.text.substring(separator+1);
                    separator = interval.indexOf("-");
                    if (separator > 0) {
                        audio_amp = interval.substring(0,separator).toInt();
                        audio_freq = interval.substring(separator+1).toInt();
                        sendMessage("Configured audio amp to "+String(audio_amp)+" and audio freq to "+String(audio_freq));
                    }
                } else {
                    audio_amp = DEFAULT_AMP_THRESHOLD;
                    audio_freq = DEFAULT_FREQ_THRESHOLD;
                }
            } else if (msg.text.startsWith("/setnight")) {
                int num = 0;
                int separator = msg.text.indexOf(" ");
                if (separator > 0) {
                    num = msg.text.substring(separator+1).toInt();
                }
                if (num > 0) {
                    log("Setting brightness threshold to "+String(num));
                    brightness_threshold = num;
                }
            } else if (msg.text.equalsIgnoreCase("/flash")) {
                log("Sending Photo from CAM with flash");
                photoflash = true;
                sendPhoto();
                photoflash = false;
            } else if (msg.text.startsWith("/start")) {
                int separator = msg.text.indexOf(" ");
                if (separator > 0) {
                    String interval = msg.text.substring(separator+1);
                    separator = interval.indexOf("-");
                    if (separator > 0) {
                        min_hour = interval.substring(0,separator).toInt();
                        max_hour = interval.substring(separator+1).toInt();
                        sendMessage("Configured min hour to "+String(min_hour)+" and max hour to "+String(max_hour));
                    }
                } else {
                    min_hour = -1;
                    max_hour = -1;
                }
                enable_detection = true;
                night_mode = false;
                saveSettings();
                sendMessage("intrusion detection started");                
            } else if (msg.text.startsWith("/night")) {
                int separator = msg.text.indexOf(" ");
                if (separator > 0) {
                    String interval = msg.text.substring(separator+1);
                    separator = interval.indexOf("-");
                    if (separator > 0) {
                        min_hour = interval.substring(0,separator).toInt();
                        max_hour = interval.substring(separator+1).toInt();
                        sendMessage("Configured min hour to "+String(min_hour)+" and max hour to "+String(max_hour));
                    }
                } else {
                    min_hour = -1;
                    max_hour = -1;
                }
                enable_detection = true;
                night_mode = true;
                sendMessage("intrusion detection started into night mode");
            } else if (msg.text.equalsIgnoreCase("/day")) {
                night_mode = false;
                sendMessage("intrusion detection set to normal mode");
            } else if (msg.text.equalsIgnoreCase("/logs")) {
                String logname = "/logs.txt";
                if (mem.exists(logname)) {
                    File f = mem.open(logname, FILE_READ);
                    if (!f) {
                        sendMessage("Cannot read logs file");
                    } else {
                        tgbot.sendDocument(userid, f, f.size(), AsyncTelegram2::TEXT, "logs.txt", "logs");
                    }
                    f.close();
                } else {
                    sendMessage("Logs file doesn't exist");
                }
            } else if (msg.text.equalsIgnoreCase("/debug")) {
                debug = !debug;
                sendMessage("Setting debug to "+String(debug));
            } else if (msg.text.equalsIgnoreCase("/audio")) {
                audio_detection = !audio_detection;
                sendMessage("Setting audio detection to "+String(audio_detection));
            } else if (msg.text.equalsIgnoreCase("/stop")) {
                enable_detection = false;
                audio_detection = false;
                night_mode = false;
                saveSettings();
                sendMessage("intrusion detection stopped");
            } else if ((msg.text.equalsIgnoreCase("/reboot")) && (!enable_detection)) {
                sendMessage("Rebooting");
                reboot();
            } else if ((msg.text.equalsIgnoreCase("/poweroff")) && (!enable_detection) && ((millis() - boot_time) > 60000*3)) {
                saveSettings();
                log("poweroff received, going to deep sleep");
                sendMessage("bye");
                delay(2000);
                esp_deep_sleep_start();
            } else if (msg.text.equalsIgnoreCase("/status")) {
                float t = 0;
                temp_sensor_read_celsius(&t);
                String msg;
                enable_detection ? msg = "Intrusion detection started." : msg = "Intrusion detection stopped.";
                night_mode ? msg += " Night mode." : msg += " Normal mode.";
                msg += " Free space: " + String(((mem.totalBytes()-mem.usedBytes())/1024)) + "KB.";
                msg += " Temp: "+String(t)+"°C.";
                msg += " SW Ver: " SW_VERSION;
                sendMessage(msg);
            } else {
                Serial.print("Unknown command");
            }
        }
    }
}

void sendAlarm(ALARM_TYPE at) {
    // check if we already sent enough alarms in less than 5 minutes
    unsigned long now = millis();
    Serial.printf("sendAlarm called, alarm count: %d, ms from last alarm: %lu\n", alarms, now-last_alarm_sent);
    if ((alarms < 5) // if we sent less than 5 alarms..
        || ((last_alarm_sent == 0) || (now-last_alarm_sent > 60000*2))) { // or the last time we sent it was more than 2 minutes
        //send the alarm
        if (at == ALARM_TYPE::PHOTO) {
            if (debug) {
                neopixelWrite(RGB_BUILTIN, 208, 52, 223);
                delay(200);
                neopixelWrite(RGB_BUILTIN, 0, 0, 0);
            }
            log("Sending alarm MOTION DETECTED!");
            //send to telegram last image saved (ID)
            String photoname = "/photo"+String(photoLastID)+".jpg";
            String msg = "MOTION DETECTED. Photo " + photoname + " from " CAMID;
            // send text first
            sendMessage(msg);
            // try to upload the picture
            tgbot.sendPhoto(userid, photoname.c_str(), mem, "");
        } else if (at == ALARM_TYPE::AUDIO) {
            String m = "Audio Detected! Value: amp "+String(audio_detected.amp)+" freq "+String(audio_detected.freq);
            sendMessage(m);
            File fwr = mem.open("/detection.wav", FILE_READ);
            log("Sending audio detection of size "+String(fwr.size())+" to telegram");
            // Telegram sendDocument doesn't overwrite the file if it has the same name
            String unique_name = "Detection_" + String(micros()) + ".wav";
            // We must send the wav file as generic binary
            tgbot.sendDocument(userid, fwr, fwr.size(), AsyncTelegram2::BINARY, unique_name.c_str(), "Sound detected!");
            delay(2000);
            fwr.close();
        }
        alarms++;
        if ((last_alarm_sent != 0) && (now-last_alarm_sent > 60000*2)) {
            alarms = 0;
        }
        last_alarm_sent = now;
    }
}

void saveSettings() {
    Settings settings;
    if (!settings.open()) {
        log("ERROR: Cannot open settings!");
    } else {
        settings.save("detection", enable_detection);
        settings.save("photoLastID", photoLastID);
        settings.save("nightMode", night_mode);
        settings.save("lastmsgID", lastmsgID);
        settings.save("photoflash", photoflash);
        settings.save("debug", debug);
        settings.save("audio_detection", audio_detection);
        settings.save("failed_connection_attempts", failed_connection_attempts);
        settings.save("audio_amp", audio_amp);
        settings.save("audio_freq", audio_freq);
        settings.save("min_hour", min_hour);
        settings.save("max_hour", max_hour);
        settings.save("brightness_threshold", brightness_threshold);
        settings.close();
    }
}

void reboot() {
    log("Rebooting..");
    saveSettings();
    ESP.restart();
}