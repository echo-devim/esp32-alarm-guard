#include <WiFi.h>
#include <AsyncTelegram2.h>
#include "soc/soc.h"                     // Brownout error fix
#include "soc/rtc_cntl_reg.h"    // Brownout error fix
#define CAMERA_MODEL_AI_THINKER
#include "camera.h"
#include <WiFiClientSecure.h>
#include <SPIFFS.h>                        // spiffs used to store images
#include "config.h"
#include <Preferences.h>
#include <nvs_flash.h>
#include "utils.h"

WiFiClientSecure client; //global ssl connection object

// The onboard LED works with inverted logic
#define LED_ON digitalWrite(ONBOARD_LED_PIN, LOW);
#define LED_OFF digitalWrite(ONBOARD_LED_PIN, HIGH);

unsigned long boot_time = millis();
int reboot_time_interval = 16000; // time in milliseconds after which the esp32 is rebooted switching behaviour
bool enable_detection;
bool night_mode;
int current_time;
int last_detection;
uint8_t prev_img[48000] = {0,}; // (800*600)/10
unsigned int changes = 0; // How many pixel changed in two consecutive photos
int min_changes = 5; //other configurable threshold, percentage of changed pixels

AsyncTelegram2 tgbot(client);
int lastmsgID;

enum BootMode {
    MONITORING,
    PHOTO,
    TELEGRAM
};

BootMode current_mode;

bool photoflash;
String photoname;
int photoLastID;
int16_t motion_detected;
bool debug;

/* FUNCTION SIGNATURES */
void sendMessage(String message);
void RebootCamera(pixformat_t format);
bool checkFile(String filename);
bool detectMotion();
/* -------- */

void reboot(BootMode mode) {
    //Save settings
    log("Rebooting into mode "+String(mode));
    Settings settings;
    if (!settings.open()) {
        log("ERROR: Cannot open settings!");
    } else {
        settings.save("bootmode", mode);
        settings.save("photoflash", photoflash);
        settings.save("PhotoName", photoname);
        settings.save("detection", enable_detection);
        settings.save("photoLastID", photoLastID);
        //Save the last received Telegram command ID in order to avoid to read the same command after the reboot
        settings.save("lastcmdid", lastmsgID);
        settings.save("nightMode", night_mode);
        settings.save("time", current_time);
        settings.save("motionDetected", motion_detected);
        settings.save("motionTimestamp", last_detection);
        settings.save("debug", debug);
        settings.save("min_changes", min_changes);
        settings.close();
    }
    log("Triggering reboot..");
    delay(500);
    //abort() or ESP.reset() don't work as expected
    // raise exception forcing the board to reboot
    esp_cpu_reset(1);
    delay(500);
}

void setup() {
    Serial.begin(115200);

    /*nvs_flash_erase(); // erase the NVS partition and...
    nvs_flash_init(); // initialize the NVS partition.
    delay(100);*/

    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable brownout detector
    pinMode(LAMP_PIN, OUTPUT);

    // Spiffs
    if (!SPIFFS.begin(true)) {
        log("An Error has occurred while mounting SPIFFS - restarting");
        delay(200);
        ESP.restart();
        delay(5000);
    } else {
    #ifdef DEBUG
        log("SPI flash Total bytes: " + String(SPIFFS.totalBytes()));
        log("SPI flash Used bytes: " + String(SPIFFS.usedBytes()));
    #endif
    }

    Settings settings;
    if (!settings.open()) {
        log("ERROR: Cannot open settings!");
        ESP.restart();
    } else {
        // load all saved values
        lastmsgID = settings.read("lastcmdid", 0);
        photoname = settings.read("PhotoName", String("")); //pass String obj as param, otherwise it calls the read bool overload
        enable_detection = settings.read("detection", false);
        photoflash = settings.read("photoflash", false);
        debug = settings.read("debug", false);
        photoLastID = settings.read("photoLastID", 0);
        min_changes = settings.read("min_changes", 5);
        night_mode = settings.read("nightMode", false);
        last_detection = settings.read("motionTimestamp", 0);
        motion_detected = settings.read("motionDetected", (int16_t)0);
        current_time = settings.read("time", 1); //hours * minutes * seconds 
        // load detection mode
        current_mode = (BootMode)settings.read("bootmode", BootMode::TELEGRAM);
        settings.close();
    }
    delay(100);
    log("Woke up in mode: "+String(current_mode));

    // if we are in night mode
    if (night_mode) {
        short current_hour = current_time/3600; //current_time must be != 0, so we initialize it to 1 (above)
        // and we're out from the night time range
        if ((current_hour > 6) && (current_hour != 23)) {
            log("Hour: "+String(current_hour)+", day time - disabling detection");
            enable_detection = false;
        } else {
            log("Hour: "+String(current_hour)+", night time - enabling detection");
            enable_detection = true;
        }
    }

    enable_detection ? log("Detection Enabled") : log("Detection Disabled");

    // check if we have a photo request or we're in monitoring mode, if that's true we initialize camera
    if ((current_mode == BootMode::PHOTO) || (current_mode == BootMode::MONITORING)) {
        log("Initialiting camera..");
        // Init the camera module (according the camera_config_t defined)
        bool tRes = setupCameraHardware(PIXFORMAT_JPEG);
        if (!tRes) {      // reboot camera
            delay(500);
            log("Problem starting camera - rebooting it");
            RebootCamera(PIXFORMAT_JPEG); // reboot system if it fails
        } else {
            log("Camera initialised ok");
        }
    } else if (current_mode == BootMode::TELEGRAM) {
        // Detection not enabled - listen for commands on telegram

        // Start WiFi connection
        log("Connecting");
        WiFi.begin(ssid, pass);
        int attempts = 12;
        while ((WiFi.status() != WL_CONNECTED) && (attempts > 0)) {
            delay(1000);
            Serial.print(".");
            attempts--;
        }
        if (attempts == 0) ESP.restart();
        log("WiFi connected");
        log(WiFi.localIP().toString());

        // Sync time with NTP
        configTzTime(MYTZ, "time.google.com", "time.windows.com", "pool.ntp.org");
        client.setInsecure();

        client.setTimeout(8000);

        // Set the Telegram bot properies
        tgbot.setUpdateTime(2000);
        tgbot.setTelegramToken(token);

        // Check if all things are ok
        Serial.print("\nTest Telegram connection... ");
        bool tg_res = tgbot.begin();
        if (tg_res) {
            log("OK");
        } else {
            log("NOK");
            ESP.restart();
        }

        //update time
        struct tm timeinfo;
        if(!getLocalTime(&timeinfo)){
            log("Failed to obtain time");
        } else {
            Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
            current_time = timeinfo.tm_hour*timeinfo.tm_min*timeinfo.tm_sec;
        }
    }
}

// ---------- END SETUP ---------------


// ---------- TELEGRAM FUNCTIONS ------

void sendMessage(String message) {
    log("Sending message on telegram: "+message);
    tgbot.sendTo(userid, String(CAMID) + ": "+message);
    delay(500);
}

void doPhotoRequest(bool flash) {
    /* photos are request via telegram, this means we were in telegram mode.
       Thus, we need to reboot the board in order to init the camera module.
    */
    photoflash = flash;
    log("rebooting into photo mode");
    delay(500);
    reboot(BootMode::PHOTO);
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
            // Check if the message is a private message (format: /CAMID:command)
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
                    doPhotoRequest(false);
                } else {
                    // Continue to telegram mode, but trigger the check in the loop function 
                    // marking the specified photo to be sent via telegram
                    photoname = "/photo"+String(num)+".jpg";
                }
            } else if (msg.text.equalsIgnoreCase("/flash")) {
                log("Sending Photo from CAM with flash");
                doPhotoRequest(true);
            } else if (msg.text.equalsIgnoreCase("/start")) {
                enable_detection = true;
                night_mode = false;
                sendMessage("intrusion detection started");
                // start now to monitor
                reboot(BootMode::MONITORING);
            } else if (msg.text.equalsIgnoreCase("/night")) {
                enable_detection = true;
                night_mode = true;
                sendMessage("intrusion detection started into night mode");
                // start now to monitor
                reboot(BootMode::MONITORING);
            } else if (msg.text.equalsIgnoreCase("/day")) {
                night_mode = false;
                sendMessage("intrusion detection set to normal mode");
                reboot(BootMode::MONITORING);
            } else if (msg.text.equalsIgnoreCase("/logs")) {
                sendMessage(getLogs());
            } else if (msg.text.equalsIgnoreCase("/debug")) {
                debug = !debug;
                sendMessage("Setting debug to "+String(debug));
            } else if (msg.text.startsWith("/set")) {
                short num = 5;
                int separator = msg.text.indexOf(" ");
                if (separator > 0) {
                    num = msg.text.substring(separator+1).toInt();
                }
                min_changes = num;
                sendMessage("Setting min_changes to "+String(num));
            } else if ((msg.text.equalsIgnoreCase("/stop") || (msg.text.equalsIgnoreCase("/poweroff")))) {
                enable_detection = false;
                night_mode = false;
                sendMessage("intrusion detection stopped");
            } else if ((msg.text.equalsIgnoreCase("/reboot")) && (!enable_detection)) {
                sendMessage("Rebooting");
                reboot(BootMode::TELEGRAM);
            } else if (msg.text.equalsIgnoreCase("/status")) {
                String msg;
                enable_detection ? msg = "Intrusion detection started." : msg = "Intrusion detection stopped.";
                night_mode ? msg += " Night mode." : msg += " Normal mode.";
                msg += " SW Ver: " SW_VERSION;
                sendMessage(msg);
            } else {
                Serial.print("Unknown command");
            }
        }
    }
}

// ---------- END TELEGRAM ------------


// ---------- CAMERA FUNCTIONS --------

// reboot camera (used if camera is failing to respond)
//      restart camera in motion mode, capture a test frame to check it is now responding ok
//      format = PIXFORMAT_GRAYSCALE or PIXFORMAT_JPEG
void RebootCamera(pixformat_t format) {
    //log("ERROR: Problem with camera detected so resetting it");
    // turn camera off then back on
    digitalWrite(PWDN_GPIO_NUM, HIGH);
    delay(200);
    digitalWrite(PWDN_GPIO_NUM, LOW);
    delay(400);
    esp_camera_deinit();
    delay(50);
    bool ok = setupCameraHardware(format);
    delay(50);
    // try capturing a frame, if still problem reboot esp32
    if ((!ok) || (!capture_image())) {
        log("Camera failed to reboot so rebooting esp32");    // store in bootlog
        delay(500);
        ESP.restart();
        delay(5000);      // restart will fail without this delay
    }
}


// ----------------------------------------------------------------
//              Save jpg in spiffs
// ----------------------------------------------------------------
// filesName = name of jpg to save as in spiffs
bool saveJpgFrame(bool useflash) {
    // Take Picture with Camera;
    log("Camera capture requested");
    //Dispose first picture because of bad quality
    camera_fb_t * fb = NULL;
    //fb = esp_camera_fb_get();
    //esp_camera_fb_return(fb); // dispose the buffered image
    
    // Take picture with Camera and send to Telegram
    if (useflash) {
        digitalWrite(LAMP_PIN, HIGH);
    }
    fb = esp_camera_fb_get();
    delay(50);
    digitalWrite(LAMP_PIN, LOW);

    // grab frame
    if (!fb) {
        log("Camera capture failed - rebooting camera");
        RebootCamera(PIXFORMAT_JPEG);
        fb = esp_camera_fb_get();                       // try again to capture frame
    }

    if (!fb) {
        log("Camera capture failed");
        return false;
    }
    delay(100);
    size_t len = fb->len;
    photoLastID = ++photoLastID % MAX_PHOTO_SAVED;
    log("Saving photo with ID "+ String(photoLastID) +" of size: "+String(len));
    String FileName = "/photo"+String(photoLastID)+".jpg";
    photoname = FileName;
    // delete old image file if it exists
    if (SPIFFS.exists(FileName))
        SPIFFS.remove(FileName);
    // save the new image
    File file = SPIFFS.open(FileName, FILE_WRITE);
    bool res = false;
    if (!file) {
        log("Failed to create file in Spiffs");
    } else {
        if (file.write(fb->buf, fb->len)) {
            log("The picture has been saved");
            res = true;
        } else {
            log("Error: writing image to Spiffs");
            // format spiffs
            /*
            if (!SPIFFS.format()) {
                log("Error: Unable to format Spiffs");
                return false;
            }*/
            file.close();
            delay(100);
            file = SPIFFS.open(FileName, FILE_WRITE);
            if (!file.write(fb->buf, fb->len)) {
                log("Error: Still unable to write image to Spiffs");
            } else {
                res = true;
            }
        }
        file.close();
    }
    delay(200);
    // Clear buffer
    esp_camera_fb_return(fb);
    delay(50);
    return res;
}


// check file saved to spiffs ok - by making sure file exists and is greater than 100 bytes
bool checkFile(String filename) {
    File f_pic = SPIFFS.open(filename);
    uint16_t pic_sz = f_pic.size();
    f_pic.close();
    return (pic_sz > 100);
}

// ---------- END CAMERA --------------


// ---------- MAIN LOOP ---------------
void loop() {

    if (current_mode == BootMode::PHOTO) {
        log("taking photo");
        bool res = saveJpgFrame(photoflash);
        // reset flash to false
        photoflash = false;
        if (res) { // photo acquired
            //now reboot and switch mode to send it to telegram
            reboot(BootMode::TELEGRAM);
        } else { //else retry
            reboot(BootMode::PHOTO);
        }
    } else if (current_mode == BootMode::TELEGRAM) { // Telegram mode
        // handle new commands
        handleCommands();
        // if motion was detected send alarm
        String msg = "";
        if (motion_detected > 2) {
            log("Motion detected");
            msg = "MOTION DETECTED!";
            motion_detected = 0;
            sendMessage(msg);
        }
        // if there is photo, send it to telegram
        if ((photoname != "") && (checkFile(photoname))) {
            log("Found saved image, sending to telegram");
            msg = "Photo "+photoname+" from " CAMID;
            tgbot.sendPhoto(userid, photoname.c_str(), SPIFFS, msg.c_str());
            photoname = "";
        }

        reboot_time_interval = enable_detection ? 5000 : 30000;
    } else if (current_mode == BootMode::MONITORING) {
        if (enable_detection) {
            log("detecting motion...");
            //detectMotion(); //initialize
            if (detectMotion()) {
                motion_detected++;
            }
            reboot_time_interval = 8000;
        } else {
            reboot_time_interval = 0;
        }
    }

    // Check if it is time to switch mode
    if (millis() - boot_time > reboot_time_interval) {
        log("Switching mode, rebooting");
        if (current_mode == BootMode::TELEGRAM) {
            reboot(BootMode::MONITORING);
        } else {
            reboot(BootMode::TELEGRAM);
        }
        delay(1000);
    }
    delay(1000);
}

bool detectMotion() {
    // Detect motion comparing pixels average value to previous photo
    // N.B. We don't need to compare every pixel, just some of them are enough
    camera_fb_t * fb = NULL;   
    if (photoflash) {
        digitalWrite(LAMP_PIN, HIGH);
        delay(100);        
        fb = esp_camera_fb_get();
        delay(50);
        digitalWrite(LAMP_PIN, LOW);
    } else {
        fb = esp_camera_fb_get();
        delay(50); 
    }
    // grab frame
    if (!fb) {
        log("Camera capture failed - rebooting camera");
        RebootCamera(PIXFORMAT_JPEG);
        fb = esp_camera_fb_get();                       // try again to capture frame
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
    bool motion = false;
    if (perc_changes > min_changes) {
        // Motion detected
        motion = true;
        log("Motion detected");
        size_t len = fb->len;
        log("Saving photo with of size: "+String(len));
        photoLastID = ++photoLastID % MAX_PHOTO_SAVED;
        photoname = "/photo"+String(photoLastID)+".jpg";
        // delete old image file if it exists
        if (SPIFFS.exists(photoname))
            SPIFFS.remove(photoname);
        // save the new image
        File file = SPIFFS.open(photoname, FILE_WRITE);
        if (!file) {
            log("Failed to create file in Spiffs");
        } else {
            if (file.write(jpg_out, out_len)) {
                log("The picture " + String(photoLastID) + " has been saved");
            } else {
                log("Error: writing image to Spiffs, retrying..");
                delay(300);
                if (file.write(jpg_out, out_len)) {
                    log("The picture " + String(photoLastID) + " has been saved");
                } else {
                    log("Sending image via telegram without saving");
                    //Send image directly to telegram and reboot
                    tgbot.sendPhoto(userid, jpg_out, out_len, "MOTION DETECTED. Image not saved!");
                    delay(2000);
                    reboot(BootMode::MONITORING);
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
