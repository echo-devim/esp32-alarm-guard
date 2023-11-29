#include <WiFi.h>
#include <AsyncTelegram2.h>
#include "soc/soc.h"                     // Brownout error fix
#include "soc/rtc_cntl_reg.h"    // Brownout error fix
#define CAMERA_MODEL_AI_THINKER
#include "camera.h"
#include <WiFiClientSecure.h>
#include <SPIFFS.h>                        // spiffs used to store images

WiFiClientSecure client;

// CONFIG
const char* ssid = "wifiname";
const char* pass = "test1234";
const char* token = "xxxxxx:xxxxxx";    // Telegram token
// Check the userid with the help of bot @JsonDumpBot or @getidsbot (work also with groups)
// https://t.me/JsonDumpBot    or    https://t.me/getidsbot
int64_t userid = 0000000;

// Timezone definition to get properly time from NTP server
#define MYTZ "CET-1CEST,M3.5.0,M10.5.0/3" //Europe/Amsterdam
#define PIR_PIN GPIO_NUM_12
#define ONBOARD_LED_PIN GPIO_NUM_33
// The onboard LED works with inverted logic
#define LED_ON digitalWrite(ONBOARD_LED_PIN, LOW);
#define LED_OFF digitalWrite(ONBOARD_LED_PIN, HIGH);

bool enable_detection = false;
int photo_mode = 0;
int boot_time = millis();
int reboot_time_interval = 10000; // time in milliseconds after which the esp32 is rebooted switching behaviour
// END CONFIG

AsyncTelegram2 tgbot(client);
int lastmsgID;


/* FUNCTION SIGNATURES */
void sendMessage(String message);
void RebootCamera(pixformat_t format);
bool checkFile(String filename);
/* -------- */

void reboot() {
    //switch mode
    if (enable_detection) {
        SPIFFS.remove("/detect.txt");
    } else {
        File file = SPIFFS.open("/detect.txt", FILE_WRITE);
        file.write(0);
        file.close();
    }
    //Save the last command
    File file = SPIFFS.open("/lastcmd.txt", FILE_WRITE);
    size_t res = file.print(lastmsgID);
    file.close();
    Serial.println("Triggering reboot..");
    delay(500);
    //abort() or ESP.reset() don't work as expected
    // raise exception
    esp_cpu_reset(1);
    delay(500);
}

esp_sleep_wakeup_cause_t get_wakeup_reason(){
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

    pinMode(ONBOARD_LED_PIN, OUTPUT);
    delay(50);
    LED_ON;
    delay(100);
    LED_OFF;

    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);             // disable brownout detector
    pinMode(LAMP_PIN, OUTPUT);                                             // set the lamp pin as output
    ledcSetup(lampChannel, pwmfreq, pwmresolution);    // configure LED PWM channel
    setLamp(0);                                                                            // set default value
    //ledcAttachPin(LAMP_PIN, lampChannel);                        // attach the GPIO pin to the channel

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

    // load last command
    File file = SPIFFS.open("/lastcmd.txt", FILE_READ);
    lastmsgID = file.readString().toInt();
    file.close();

    // load detection mode
    enable_detection = SPIFFS.exists("/detect.txt") && (!SPIFFS.exists("/disable.txt"));
    enable_detection ? Serial.println("Detection Enabled") : Serial.println("Detection Disabled");

    // if detection is enabled, check if on filesystem we have a photo request
    if (enable_detection) {
        //photo_mode = 0 no photo, 1 photo without flash, 2 photo with flash
        if (SPIFFS.exists("/photo_request.txt")) {
            File file = SPIFFS.open("/photo_request.txt", "r");
            file.read((uint8_t*)&photo_mode, 1); // this is 0 (photo without flash) or 1 (photo with flash) 
            file.close();
            photo_mode++;
        } else {
            photo_mode = 0;
        }

        // Init the camera module (according the camera_config_t defined)
        bool tRes = setupCameraHardware(PIXFORMAT_JPEG);
        if (!tRes) {      // reboot camera
            delay(500);
            Serial.println("Problem starting camera - rebooting it");
            RebootCamera(PIXFORMAT_JPEG); // reboot system if it fails
        } else {
            Serial.println("Camera initialised ok");
            if (photo_mode) {
                SPIFFS.remove("/photo_request.txt");
            } else {
                esp_sleep_wakeup_cause_t wakeup_reason = get_wakeup_reason();
                if (wakeup_reason == 2) {
                    // if wakeup is caused by PIR sensor, take a photo
                } else if (wakeup_reason == 4) {
                    // if wakeup is caused by timer, just switch mode
                    if (!photo_mode) {
                        reboot();
                    }
                } else {
                    // if deep sleep was not called, go to sleep
                    Serial.println("Going to sleep");
                    int seconds = 20;
                    esp_sleep_enable_timer_wakeup(seconds*1000000);
                    esp_sleep_enable_ext0_wakeup(PIR_PIN, 1);
                    delay(100);
                    esp_deep_sleep_start();
                } 
            }
        }
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
    tgbot.sendTo(userid, message);
}

void doPhotoRequest(bool flash) {
    File file = SPIFFS.open("/photo_request.txt", FILE_WRITE);
    file.write(flash);
    file.close();
    Serial.println("wrote photo request");
    delay(500);
    reboot();
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
            if (msg.text.equalsIgnoreCase("/getphoto")) {
                Serial.println("Sending Photo from CAM");
                doPhotoRequest(false);
            } else if (msg.text.equalsIgnoreCase("/getphotoflash")) {
                Serial.println("Sending Photo from CAM with flash");
                doPhotoRequest(true);
            } else if (msg.text.equalsIgnoreCase("/start")) {
                sendMessage("ESP32 intrusion detection started");
                SPIFFS.remove("/disable.txt");
                reboot();
            } else if ((msg.text.equalsIgnoreCase("/stop") || (msg.text.equalsIgnoreCase("/poweroff")))) {
                sendMessage("ESP32 intrusion detection stopped");
                File file = SPIFFS.open("/disable.txt", FILE_WRITE);
                file.write(0);
                file.close();
                delay(500);
                reboot();
            } else if ((msg.text.equalsIgnoreCase("/reboot")) && (!enable_detection)) {
                sendMessage("Rebooting ESP32");
                reboot();
            } else if (msg.text.equalsIgnoreCase("/status")) {
                (SPIFFS.exists("/disable.txt")) ? sendMessage("intrusion detection stopped") : sendMessage("intrusion detection started");
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
    // Take picture with Camera and send to Telegram
    if (useflash) {
        digitalWrite(LAMP_PIN, HIGH);
    }
    camera_fb_t* fb = esp_camera_fb_get();
    delay(20);
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
    size_t len = fb->len;
    Serial.println("Saving photo of size: "+String(len));
    
    String FileName = "/photo.jpg";
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
    // Clear buffer
    esp_camera_fb_return(fb);
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
    reboot_time_interval = 5000;
    if (enable_detection) {
        Serial.println("taking photo");
        saveJpgFrame(photo_mode == 2);
        //now reboot and switch mode to send it to telegram
        reboot();
    } else { // Telegram mode
        if (checkFile("/photo.jpg")) {
            Serial.println("Found saved image, sending to telegram");
            tgbot.sendPhoto(userid, "/photo.jpg", SPIFFS, "");
            SPIFFS.remove("/photo.jpg");
        } else {
            handleCommands();
        }
    }

    // Check if it is time to switch mode
    if (millis() - boot_time > reboot_time_interval) {
        Serial.println("Switching mode, rebooting");
        reboot();
        delay(1000);
    }
    delay(1000);
}
