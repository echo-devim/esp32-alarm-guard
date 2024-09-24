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

/* Code for FREENOVE ESP32-S3 WROOM 1 chip equipped with a MAX9814 microphone */

#define RGB_BRIGHTNESS 64 // Change white brightness (max 255)
WiFiClientSecure client; //global ssl connection object
AsyncTelegram2 tgbot(client);
int lastmsgID;
bool enable_detection;
bool night_mode;
bool photoflash;
int photoLastID;
uint8_t prev_img[30720] = {0,}; // (640*480)/10
unsigned int changes = 0; // How many pixel changed in two consecutive photos
TaskHandle_t task_mic; //background task for microphone
int audio_detected = 0;
bool debug = false;

/* Function signatures */
void saveSettings();
void reboot();
void sendAlarm();
void doPhotoRequest();
bool detectMotion();
void handleCommands();
bool isNight();
void sendMessage(String message);
int audioDetection(uint8_t seconds, int threshold = 1700);
/* --- */

// Arduino code by default runs on CORE 1
// Create a parallel function to run on CORE 2
void task_worker( void * parameter) {
    for(;;) {
        if (enable_detection) {
            // Listen for 2 second to detect sounds
            audio_detected = audioDetection(2);
        }
        delay(200);
    }
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

    #ifdef DEBUG
    dumpChipInfo();
    #endif
    initTempSensor();

    // Initialize Spiffs
    if (!LittleFS.begin(true)) {
        log("An Error has occurred while mounting LittleFS - restarting");
        delay(200);
        reboot(); // restart and try again
    } else {
        log("SPI flash Total bytes: " + String(LittleFS.totalBytes()));
        log("SPI flash Used bytes: " + String(LittleFS.usedBytes()));
        listDir(LittleFS, "/", 1);
    }

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

    // Warm up the camera by taking a photo
    camera_fb_t * fb = NULL;
    fb = esp_camera_fb_get();
    delay(100);
    esp_camera_fb_return(fb); // dispose the buffered image

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
    if (attempts == 0) reboot();
    neopixelWrite(RGB_BUILTIN, 0, 0, 0);  // Off / black
    log("");
    log("WiFi connected");
    log(WiFi.localIP().toString());

    // Sync time with NTP
    configTzTime(MYTZ, "time.google.com", "time.windows.com", "pool.ntp.org");
    client.setInsecure();

    client.setTimeout(30000);

    // Set the Telegram bot properties
    tgbot.setUpdateTime(1000);
    tgbot.setTelegramToken(token);

    // Check if all things are ok
    Serial.print("\nTest Telegram connection... ");
    bool tg_res = tgbot.begin();
    delay(1000);
    if (tg_res) {
        log("OK");
    } else {
        log("NOK");
        reboot();
    }

    enable_detection ? log("Detection Enabled") : log("Detection Disabled");

    #ifdef USE_MICROPHONE
    xTaskCreatePinnedToCore(
        task_worker, /* Function to implement the task */
        "task_mic", /* Name of the task */
        10000,  /* Stack size in words */
        NULL,  /* Task input parameter */
        0,  /* Priority of the task */
        &task_mic,  /* Task handle. */
        0); /* Core where the task should run */
    #endif
    log("Setup done");
}

void loop() {
    //Serial.println("loop function");
    // check wifi connection status
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

    // handle new commands
    handleCommands();
    delay(1000);

    if (night_mode) {
        if (isNight()) {
            if (!enable_detection) {
                log("Night mode: enabling detection");
                enable_detection = true;
            }
        } else {
            if (enable_detection) {
                log("Night mode: disabling detection");
                enable_detection = false;
            }
        }
    }

    if (enable_detection) {
        if (audio_detected > 0) {
            String m = "Audio Detected. Value: "+String(audio_detected);
            log(m);
            sendMessage(m);
        }
        //log("detecting motion...");
        //detectMotion(); //initialize
        bool confident_detection = true;
        for(int i = 0; i < 1; i++) {
            delay(2000);
            confident_detection = confident_detection && detectMotion();
        }
        if (confident_detection) {
            sendAlarm();
        }
    } else {
        delay(1000);
    }
    //Serial.println("end loop");
}
// ------ AUDIO DETECTION ------------
int audioDetection(uint8_t seconds, int threshold) {
    // Listen the mic for input seconds looking for loud sounds
    // Returns the ADC value
    for (int i = 0; i < 20*seconds; i++) {
        int r = analogRead(MICROPHONE_PIN);
        //Serial.println(String(r));
        if (r > threshold) {
            return r;
        }
        delay(50);
    }
    return 0;
}
// ------ MOTION DETECTION -----------
bool isNight() {
    camera_fb_t * fb = esp_camera_fb_get();
    delay(50); 

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
    for (size_t i = 0; i < rgb_len; i+=3) {
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
    log("Night mode checking, avg brightness: "+String(pix_avg));
    #endif
    return (pix_avg < 50);
}

bool detectMotion() {
    // Detect motion comparing pixels average value to previous photo
    // N.B. We don't need to compare every pixel, just some of them are enough
    camera_fb_t * fb = NULL;   
    if (photoflash) {
        neopixelWrite(RGB_BUILTIN, 255, 255, 255);  // White / Flash
        delay(100);        
        fb = esp_camera_fb_get();
        delay(50);
        neopixelWrite(RGB_BUILTIN, 0, 0, 0);  // Off / black
    } else {
        fb = esp_camera_fb_get();
        delay(50); 
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
        unsigned int pix_avg = (pixel_blue + pixel_red + pixel_green) / 3;
        //we don't need to store the value of each pixel, but only few pixels are enough (save memory)
        if (i % 10 == 0) { //skip 30 pixels
            if (prev_img[j] != 0) { //0 is the default value, skip the comparation if prev_img[j] is 0
                int pix_diff = pix_avg - prev_img[j];
                pix_diff = pix_diff > 0 ? pix_diff : pix_diff*-1;
                int threshold = 40;
                if (pix_diff > threshold) {
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
        log("Found "+String(perc_changes)+" changes");
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
        if (LittleFS.exists(photoname)) {
            Serial.println(photoname + " already exists, overwriting..");
            LittleFS.remove(photoname);
        }
        // save the new image
        File file = LittleFS.open(photoname, FILE_WRITE);
        if (!file) {
            log("Failed to create file in LittleFS");
        } else {
            if (file.write(jpg_out, out_len)) {
                log("The picture " + String(photoLastID) + " has been saved");
            } else {
                log("Error: writing image to LittleFS, retrying..");
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

void doPhotoRequest() {
    // Send a photo via telegram without saving it on filesystem

    log("Camera capture requested");
    camera_fb_t * fb = NULL;
    
    if (photoflash) {
        neopixelWrite(RGB_BUILTIN, 255, 255, 255);  // White / Flash
        delay(100);        
        fb = esp_camera_fb_get();
        delay(50);
        neopixelWrite(RGB_BUILTIN, 0, 0, 0);  // Off / black
    } else {
        fb = esp_camera_fb_get();
        delay(50);
    }

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
    if (tgbot.getNewMessage(msg)) {
        MessageType msgType = msg.messageType;
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
                if ((camName != CAMID)||(camName != GROUPID)) {
                    log(camName + " is not the ID of this camera, expected: " CAMID);
                    return;
                }
                log("Received a private message, command: " + command);
                msg.text = command;
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
                    doPhotoRequest();
                } else {
                    String photoname = "/photo"+String(num)+".jpg";
                    if (LittleFS.exists(photoname)) {
                        String msg = "Photo " + photoname + " from " CAMID;
                        tgbot.sendPhoto(userid, photoname.c_str(), LittleFS, msg.c_str());
                    } else {
                        sendMessage(photoname+" not found");
                    }
                }
            } else if (msg.text.equalsIgnoreCase("/flash")) {
                log("Sending Photo from CAM with flash");
                photoflash = true;
                doPhotoRequest();
                photoflash = false;
            } else if (msg.text.equalsIgnoreCase("/start")) {
                enable_detection = true;
                night_mode = false;
                saveSettings();
                sendMessage("intrusion detection started");                
            } else if (msg.text.equalsIgnoreCase("/night")) {
                enable_detection = true;
                night_mode = true;
                sendMessage("intrusion detection started into night mode");
            } else if (msg.text.equalsIgnoreCase("/day")) {
                night_mode = false;
                sendMessage("intrusion detection set to normal mode");
            } else if (msg.text.equalsIgnoreCase("/logs")) {
                sendMessage(getLogs());
            } else if (msg.text.equalsIgnoreCase("/debug")) {
                debug = !debug;
                sendMessage("Setting debug to "+String(debug));
            } else if (msg.text.equalsIgnoreCase("/stop")) {
                enable_detection = false;
                night_mode = false;
                saveSettings();
                sendMessage("intrusion detection stopped");
            } else if ((msg.text.equalsIgnoreCase("/reboot")) && (!enable_detection)) {
                sendMessage("Rebooting");
                reboot();
            } else if ((msg.text.equalsIgnoreCase("/poweroff")) && (!enable_detection)) {
                saveSettings();
                log("poweroff received, going to deep sleep");
                sendMessage("bye");
                delay(2000);
                esp_deep_sleep_start();
            } else if (msg.text.equalsIgnoreCase("/status")) {
                String msg;
                enable_detection ? msg = "Intrusion detection started." : msg = "Intrusion detection stopped.";
                night_mode ? msg += " Night mode." : msg += " Normal mode.";
                msg += " Free space: " + String(((LittleFS.totalBytes()-LittleFS.usedBytes())/1024)) + "KB";
                msg += " SW Ver: " SW_VERSION;
                sendMessage(msg);
            } else {
                Serial.print("Unknown command");
            }
        }
    }
}

void sendAlarm() {
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
    tgbot.sendPhoto(userid, photoname.c_str(), LittleFS, "");
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
        settings.close();
    }
}

void reboot() {
    log("Rebooting..");
    saveSettings();
    ESP.restart();
}