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

WiFiClientSecure client; //global ssl connection object

// The onboard LED works with inverted logic
#define LED_ON digitalWrite(ONBOARD_LED_PIN, LOW);
#define LED_OFF digitalWrite(ONBOARD_LED_PIN, HIGH);

int boot_time = millis();
int reboot_time_interval = 15000; // time in milliseconds after which the esp32 is rebooted switching behaviour
bool enable_detection;

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

/* FUNCTION SIGNATURES */
void sendMessage(String message);
void RebootCamera(pixformat_t format);
bool checkFile(String filename);
/* -------- */

/* Utility Classes */

class Settings {
    /* Settings utility class.
    Decouple the way settings are stored on flash memory.
    Implementation based on Preferences object, based in turn on nvs */
private:
    Preferences settings;
public:
    bool open() {
        return settings.begin("alarmguard", false); //false means read/write access
    }
    void close() {
        settings.end();
    }
    size_t save(const char *key, int16_t value) {
        return settings.putShort(key, value);
    }
    size_t save(const char *key, int32_t value) {
        return settings.putInt(key, value);
    }
    size_t save(const char *key, bool value) {
        return settings.putBool(key, value);
    }
    size_t save(const char *key, String value) {
        return settings.putString(key, value);
    }
    int16_t read(const char *key, int16_t default_value) {
        return settings.getShort(key, default_value);
    }
    int32_t read(const char *key, int32_t default_value) {
        return settings.getInt(key, default_value);
    }
    bool read(const char *key, bool default_value) {
        return settings.getBool(key, default_value);
    }
    String read(const char *key, String default_value) {
        return settings.getString(key, default_value);
    }            
};

/* ---- */

void reboot(BootMode mode) {
    //Save settings
    Serial.println("Rebooting into mode "+String(mode));
    Settings settings;
    if (!settings.open()) {
        Serial.println("ERROR: Cannot open settings!");
    } else {
        settings.save("bootmode", mode);
        settings.save("photoflash", photoflash);
        settings.save("PhotoName", photoname);
        settings.save("detection", enable_detection);
        settings.save("photoLastID", photoLastID);
        //Save the last received Telegram command ID in order to avoid to read the same command after the reboot
        settings.save("lastcmdid", lastmsgID);
        settings.close();
    }
    Serial.println("Triggering reboot..");
    delay(500);
    //abort() or ESP.reset() don't work as expected
    // raise exception forcing the board to reboot
    esp_cpu_reset(1);
    delay(500);
}

esp_sleep_wakeup_cause_t get_wakeup_reason(){
    /* Function that analyzes the cause that woke up the board.
    Depending on the cause, we have different behaviors. */
    esp_sleep_wakeup_cause_t wakeup_reason;

    wakeup_reason = esp_sleep_get_wakeup_cause();
    Serial.printf("wakeup_reason = %d\n", wakeup_reason);

    switch(wakeup_reason)
    {
    case 1  : Serial.println("Wakeup caused by external signal using RTC_IO"); break;
    case 2  : Serial.println("Wakeup caused by external signal using RTC_CNTL"); break;
    case 3  : Serial.println("Wakeup caused by timer"); break;
    case 4  : Serial.println("Wakeup caused by touchpad"); break;
    case 5  : Serial.println("Wakeup caused by ULP program"); break;
    default : Serial.println("Wakeup was not caused by deep sleep"); break;
    }

    return wakeup_reason;
}

void setup() {
    Serial.begin(115200);
    Serial.println();

    /*nvs_flash_erase(); // erase the NVS partition and...
    nvs_flash_init(); // initialize the NVS partition.
    delay(100);*/

    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable brownout detector
    pinMode(LAMP_PIN, OUTPUT);
    pinMode(PIR_PIN, INPUT);

    // Spiffs
    if (!SPIFFS.begin(true)) {
        Serial.println("An Error has occurred while mounting SPIFFS - restarting");
        delay(200);
        ESP.restart();                                // restart and try again
        delay(5000);
    } else {
        Serial.println("SPI flash Total bytes: " + String(SPIFFS.totalBytes()));
        Serial.println("SPI flash Used bytes: " + String(SPIFFS.usedBytes()));
    }

    Settings settings;
    if (!settings.open()) {
        Serial.println("ERROR: Cannot open settings!");
        ESP.restart();
    } else {
        // load all saved values
        lastmsgID = settings.read("lastcmdid", 0);
        photoname = settings.read("PhotoName", String("")); //pass String obj as param, otherwise it calls the read bool overload
        enable_detection = settings.read("detection", false);
        photoflash = settings.read("photoflash", false);
        photoLastID = settings.read("photoLastID", 0);
        // load detection mode
        current_mode = (BootMode)settings.read("bootmode", BootMode::TELEGRAM);
        settings.close();
    }
    delay(100);
    Serial.println("Woke up in mode: "+String(current_mode));
    enable_detection ? Serial.println("Detection Enabled") : Serial.println("Detection Disabled");

    esp_sleep_wakeup_cause_t wakeup_reason = get_wakeup_reason();
    if (wakeup_reason == 2) {
        // if wakeup is caused by PIR sensor, take a photo
        current_mode = BootMode::PHOTO;
    } else if ((current_mode == BootMode::MONITORING) && ((wakeup_reason == 4) || (!enable_detection))) {
        // if wakeup is caused by timer while monitoring or detection is disabled, just switch mode
        current_mode = BootMode::TELEGRAM;
    }

    // check if we have a photo request, if that's true we initialize camera
    if (current_mode == BootMode::PHOTO) {
        Serial.println("Initialiting camera..");
        // Init the camera module (according the camera_config_t defined)
        bool tRes = setupCameraHardware(PIXFORMAT_JPEG);
        if (!tRes) {      // reboot camera
            delay(500);
            Serial.println("Problem starting camera - rebooting it");
            RebootCamera(PIXFORMAT_JPEG); // reboot system if it fails
        } else {
            Serial.println("Camera initialised ok");
        }
    } else if ((current_mode == BootMode::MONITORING) && (enable_detection)) {
        // if deep sleep was not called, go to sleep
        Serial.println("Going to sleep");
        esp_sleep_enable_timer_wakeup(waiting_time*1000000);
        esp_sleep_enable_ext0_wakeup(PIR_PIN, 1);
        delay(100);
        esp_deep_sleep_start();
    } else {
        // Detection not enabled - listen for commands on telegram

        // Start WiFi connection
        Serial.println("Connecting");
        WiFi.begin(ssid, pass);
        int attempts = 12;
        while ((WiFi.status() != WL_CONNECTED) && (attempts > 0)) {
            delay(1000);
            Serial.print(".");
            attempts--;
        }
        if (attempts == 0) ESP.restart();
        Serial.println("");
        Serial.println("WiFi connected");
        Serial.println(WiFi.localIP());

        // Sync time with NTP
        configTzTime(MYTZ, "time.google.com", "time.windows.com", "pool.ntp.org");
        client.setInsecure();

        client.setTimeout(8000);

        // Set the Telegram bot properies
        tgbot.setUpdateTime(1000);
        tgbot.setTelegramToken(token);

        // Check if all things are ok
        Serial.print("\nTest Telegram connection... ");
        bool tg_res = tgbot.begin();
        if (tg_res) {
            Serial.println("OK");
        } else {
            Serial.println("NOK");
            ESP.restart();
        }
    }
}

// ---------- END SETUP ---------------


// ---------- TELEGRAM FUNCTIONS ------

void sendMessage(String message) {
    Serial.println("Sending message on telegram: "+message);
    tgbot.sendTo(userid, String(CAMID) + ": "+message);
}

void doPhotoRequest(bool flash) {
    /* photos are request via telegram, this means we were in telegram mode.
       Thus, we need to reboot the board in order to init the camera module.
    */
    photoflash = flash;
    Serial.println("rebooting into photo mode");
    delay(500);
    reboot(BootMode::PHOTO);
}

void handleCommands() {
    // A variable to store telegram message data
    TBMessage msg;
    // if there is an incoming message...
    if (tgbot.getNewMessage(msg)) {
        MessageType msgType = msg.messageType;
        Serial.println("Received "+ String(msg.messageID) + " - " + msg.text + ", last command id was " +String(lastmsgID));
        // we must check if the last command is different from the current command
        // unfortunately, the library fetches always the last command even if it was already read
        if ((msgType == MessageText) && (msg.messageID != lastmsgID)) {
            lastmsgID = msg.messageID;
            Serial.println("New message: "+msg.text);
            // Received a text message
            if (msg.text.startsWith("/photo")) {
                short num = -1;
                int separator = msg.text.indexOf(" ");
                if (separator > 0) {
                    num = msg.text.substring(separator+1).toInt();
                }
                Serial.println("Sending Photo from CAM");
                if (num < 0) {
                    doPhotoRequest(false);
                } else {
                    // Continue to telegram mode, but trigger the check in the loop function 
                    // marking the specified photo to be sent via telegram
                    photoname = "/photo"+String(num)+".jpg";
                }
            } else if (msg.text.equalsIgnoreCase("/flash")) {
                Serial.println("Sending Photo from CAM with flash");
                doPhotoRequest(true);
            } else if (msg.text.equalsIgnoreCase("/start")) {
                enable_detection = true;
                sendMessage("intrusion detection started");
                // start now to monitor
                reboot(BootMode::MONITORING);
            } else if ((msg.text.equalsIgnoreCase("/stop") || (msg.text.equalsIgnoreCase("/poweroff")))) {
                enable_detection = false;
                sendMessage("intrusion detection stopped");
            } else if ((msg.text.equalsIgnoreCase("/reboot")) && (!enable_detection)) {
                sendMessage("Rebooting");
                reboot(BootMode::TELEGRAM);
            } else if (msg.text.equalsIgnoreCase("/status")) {
                enable_detection ? sendMessage("intrusion detection stopped") : sendMessage("intrusion detection started");
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
    //Serial.println("ERROR: Problem with camera detected so resetting it");
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
        Serial.println("Camera failed to reboot so rebooting esp32");    // store in bootlog
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
    Serial.println("Camera capture requested");
    //Dispose first picture because of bad quality
    camera_fb_t * fb = NULL;
    fb = esp_camera_fb_get();
    esp_camera_fb_return(fb); // dispose the buffered image
    
    // Take picture with Camera and send to Telegram
    if (useflash) {
        digitalWrite(LAMP_PIN, HIGH);
    }
    fb = esp_camera_fb_get();
    delay(50);
    digitalWrite(LAMP_PIN, LOW);

    // grab frame
    if (!fb) {
        Serial.println("Camera capture failed - rebooting camera");
        RebootCamera(PIXFORMAT_JPEG);
        fb = esp_camera_fb_get();                       // try again to capture frame
    }

    if (!fb) {
        Serial.println("Camera capture failed");
        return false;
    }
    delay(100);
    size_t len = fb->len;
    int ID = ++photoLastID % 30;
    Serial.println("Saving photo with ID "+ String(ID) +" of size: "+String(len));
    String FileName = "/photo"+String(ID)+".jpg";
    photoname = FileName;
    // delete old image file if it exists
    if (SPIFFS.exists(FileName))
        SPIFFS.remove(FileName);
    // save the new image
    File file = SPIFFS.open(FileName, FILE_WRITE);
    if (!file) {
        Serial.println("Failed to create file in Spiffs");
    } else {
        if (file.write(fb->buf, fb->len)) {
            Serial.print("The picture has been saved");
        } else {
            Serial.println("Error: writing image to Spiffs");
            // format spiffs
            /*
            if (!SPIFFS.format()) {
                Serial.println("Error: Unable to format Spiffs");
                return false;
            }*/
            file = SPIFFS.open(FileName, FILE_WRITE);
            if (!file.write(fb->buf, fb->len)) Serial.println("Error: Still unable to write image to Spiffs");
        }
        file.close();
    }
    delay(200);
    // Clear buffer
    esp_camera_fb_return(fb);
    delay(50);
    return true;
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
        Serial.println("taking photo");
        saveJpgFrame(photoflash);
        //now reboot and switch mode to send it to telegram
        reboot(BootMode::TELEGRAM);
    } else if (current_mode == BootMode::TELEGRAM) { // Telegram mode
        handleCommands();
        if ((photoname != "") && (checkFile(photoname))) {
            Serial.println("Found saved image, sending to telegram");
            tgbot.sendPhoto(userid, photoname.c_str(), SPIFFS, "Photo from " CAMID);
            photoname = "";
        }
        // when detection is enabled reduce telegram time
        // otherwise if we are always in telegram mode increase polling time
        if (enable_detection) {
            reboot_time_interval = 5000;
        } else {
            reboot_time_interval = 15000;
            delay(2000);
        }
    }

    // Check if it is time to switch mode
    if (millis() - boot_time > reboot_time_interval) {
        Serial.println("Switching mode, rebooting");
        if (current_mode == BootMode::TELEGRAM) {
            reboot(BootMode::MONITORING);
        } else {
            reboot(BootMode::TELEGRAM);
        }
        delay(1000);
    }
    delay(1000);
}
