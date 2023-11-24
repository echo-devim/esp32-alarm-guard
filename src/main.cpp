#include <WiFi.h>
#include <AsyncTelegram2.h>
#include "soc/soc.h"                     // Brownout error fix
#include "soc/rtc_cntl_reg.h"    // Brownout error fix
#define CAMERA_MODEL_AI_THINKER
#include "camera.h"
#include <WiFiClientSecure.h>
#include <SPIFFS.h>                        // spiffs used to store images

WiFiClientSecure client;

//TODO: code refactoring of the project

// CONFIG
const char* ssid = "wifiname";                                                                                // SSID WiFi network
const char* pass = "test1234";                                                                                // Password    WiFi network
const char* token = "xxxxxx:xxxxxx";    // Telegram token
// Check the userid with the help of bot @JsonDumpBot or @getidsbot (work also with groups)
// https://t.me/JsonDumpBot    or    https://t.me/getidsbot
int64_t userid = 0000000;

// Timezone definition to get properly time from NTP server
#define MYTZ "CET-1CEST,M3.5.0,M10.5.0/3" //Europe/Amsterdam
#define PIR_PIN GPIO_NUM_12

bool enable_detection = false;
bool photo_mode = false;
int boot_time = millis();
int reboot_time_interval = 10000; // time in milliseconds after which the esp32 is rebooted switching behaviour
// END CONFIG

AsyncTelegram2 tgbot(client);
String lastcommand;


/* FUNCTION SIGNATURES */
void sendMessage(String message);
void RebootCamera(pixformat_t format);
void RestartCamera(pixformat_t format);
bool checkFile(String filename);
/* -------- */

void reboot() {
    //Note: we must use a workaround to reboot ESP32 chip using ssl client (Telegram) OR camera
    //SSL client doesn't work with camera enabled
    //We use files as switches to remember the previous state
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
    size_t res = file.print(lastcommand);
    Serial.println("Wrote "+String(res) + " bytes, " + lastcommand);
    file.close();
    Serial.println("Triggering reboot..");
    delay(500);
    //abort() or ESP.reset() don't work as expected
    // raise exception
    esp_cpu_reset(1);
    delay(500);
}

void setup() {
    Serial.begin(115200);
    Serial.println();

    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);             // disable brownout detector
    pinMode(LAMP_PIN, OUTPUT);                                             // set the lamp pin as output
    ledcSetup(lampChannel, pwmfreq, pwmresolution);    // configure LED PWM channel
    setLamp(0);                                                                            // set default value
    ledcAttachPin(LAMP_PIN, lampChannel);                        // attach the GPIO pin to the channel

    pinMode(PIR_PIN, INPUT);
    delay(200);

    // Spiffs
    if (!SPIFFS.begin(true)) {
        Serial.println("An Error has occurred while mounting SPIFFS - restarting");
        delay(1000);
        ESP.restart();                                // restart and try again
        delay(5000);
    } else {
        Serial.println("SPI flash Total bytes: " + String(SPIFFS.totalBytes()));
        Serial.println("SPI flash Used bytes: " + String(SPIFFS.usedBytes()));
    }

    // load last command
    File file = SPIFFS.open("/lastcmd.txt", FILE_READ);
    lastcommand = file.readString();
    file.close();
    Serial.println("Last command saved on disk "+lastcommand);

    // load detection mode
    enable_detection = SPIFFS.exists("/detect.txt") && (!SPIFFS.exists("/disable.txt"));
    enable_detection ? Serial.println("Detection Enabled") : Serial.println("Detection Disabled");

    // if detection is enabled, check if on filesystem we have a photo request
    if (enable_detection) {
        //WiFi.disconnect(true);
        //WiFi.mode(WIFI_OFF);
        photo_mode = SPIFFS.exists("/photo_request.txt");
        // Init the camera module (according the camera_config_t defined)
        bool tRes = setupCameraHardware(PIXFORMAT_JPEG);
        if (!tRes) {      // reboot camera
            delay(500);
            Serial.println("Problem starting camera - rebooting it");
            RestartCamera(PIXFORMAT_JPEG); // RestartCamera reboot system if it fails
        } else {
            Serial.println("Camera initialised ok");
            if (photo_mode)
                SPIFFS.remove("/photo_request.txt");
        }
    } else {
        // Detection not enabled - listen for commands on telegram
        photo_mode = false;
        esp_camera_deinit();
        digitalWrite(PWDN_GPIO_NUM, HIGH);
        delay(200);

        // Start WiFi connection
        WiFi.begin(ssid, pass);
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
        }
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
        tgbot.begin() ? Serial.println("OK") : Serial.println("NOK");
    }

}

// ---------- END SETUP ---------------


// ---------- TELEGRAM FUNCTIONS ------

void sendMessage(String message) {
    Serial.println("Sending message on telegram: "+message);
    tgbot.sendTo(userid, message);
}

void doPhotoRequest() {
    File file = SPIFFS.open("/photo_request.txt", FILE_WRITE);
    file.write(0);
    file.close();
    delay(500);
    reboot();
    delay(5000);
}

void handleCommands() {
    // A variable to store telegram message data
    TBMessage msg;
    // if there is an incoming message...
    if (tgbot.getNewMessage(msg)) {
        MessageType msgType = msg.messageType;
        Serial.println("Received "+ String(msg.messageID) + " - " + msg.text + ", last command was " +lastcommand);
        if ((msgType == MessageText) && (!msg.text.equals(lastcommand))) {
            lastcommand = msg.text;
            Serial.println("New message: "+msg.text);
            // Received a text message
            if (msg.text.equalsIgnoreCase("/getphoto")) {
                Serial.println("Sending Photo from CAM");
                doPhotoRequest();
            } else if (msg.text.equalsIgnoreCase("/start")) {
                sendMessage("ESP32 intrusion detection started");
                SPIFFS.remove("/disable.txt");
                reboot();
                delay(5000);
            } else if (msg.text.equalsIgnoreCase("/stop")) {
                sendMessage("ESP32 intrusion detection stopped");
                File file = SPIFFS.open("/disable.txt", FILE_WRITE);
                file.write(0);
                file.close();
                delay(500);
                delay(500);
                reboot();
                delay(5000);
            } else if ((msg.text.equalsIgnoreCase("/reboot")) && (!enable_detection)) {
                sendMessage("Rebooting ESP32");
                reboot();
                delay(5000);
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

// ----------------------------------------------------------------
//              -restart the camera in different mode
// ----------------------------------------------------------------
// switches camera mode - format = PIXFORMAT_GRAYSCALE or PIXFORMAT_JPEG
void RestartCamera(pixformat_t format) {
    esp_camera_deinit();
    bool ok = setupCameraHardware(format);
    if (ok) {
        Serial.println("Camera mode switched ok");
    } else {
        // failed so try again
        esp_camera_deinit();
        delay(50);
        ok = setupCameraHardware(format); //esp_camera_init(&config);
        if (ok) {
            Serial.println("Camera mode switched ok - 2nd attempt");
        } else {
            Serial.println("Camera failed to restart so rebooting camera");        // store in bootlog
            RebootCamera(format);
        }
    }
}

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
    RestartCamera(PIXFORMAT_JPEG);    // restart camera in motion mode
    delay(50);
    // try capturing a frame, if still problem reboot esp32
    if (!capture_image()) {
        Serial.println("Camera failed to reboot so rebooting esp32");    // store in bootlog
        delay(500);
        reboot();
        delay(5000);      // restart will fail without this delay
    }
    if (format == PIXFORMAT_JPEG) RestartCamera(PIXFORMAT_JPEG);                  // if jpg mode required restart camera again
}


// ----------------------------------------------------------------
//              Save jpg in spiffs
// ----------------------------------------------------------------
// filesName = name of jpg to save as in spiffs
bool saveJpgFrame(bool useflash) {
    // Take Picture with Camera;
    Serial.println("Camera capture requested");
    // Take picture with Camera and send to Telegram
    if (useflash) setLamp(100);
    camera_fb_t* fb = esp_camera_fb_get();
    setLamp(0);

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
            }
            file = SPIFFS.open(FileName, FILE_WRITE);
            if (!file.write(fb->buf, fb->len)) Serial.println("Error: Still unable to write image to Spiffs");*/
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
    if (enable_detection) {
        if (photo_mode) {
            saveJpgFrame(true);
            //now reboot and switch mode to send it to telegram
            reboot();
        } else {
            // camera motion detection
            // Check if PIR sensor is detecting motion
            bool motion_detected = digitalRead(PIR_PIN);
            if (motion_detected) {
                if (tCounter >= tCounterTrigger) {                                                // only trigger if movement detected in more than one consequitive frames
                    tCounter = 0;
                    saveJpgFrame(true);
                    reboot();
                } else {
                    Serial.println("Not enough consecutive detections");
                }
            }
            reboot_time_interval = 15000;
        }
    } else { // Telegram mode
        if (checkFile("/photo.jpg")) {
            Serial.println("Found saved image, sending to telegram");
            tgbot.sendPhoto(userid, "/photo.jpg", SPIFFS, "");
            SPIFFS.remove("/photo.jpg");
        } else {
            handleCommands();
        }
        reboot_time_interval = 5000;
    }

    // Check if it is time to switch mode
    if (millis() - boot_time > reboot_time_interval) {
        Serial.println("Switching mode, rebooting");
        reboot();
        delay(1000);
    }
    delay(1000);
}
