#pragma once
#include <FS.h>
#include <LittleFS.h>
#include <SD_MMC.h>

// CONFIG

const char* ssid = "your wifi name";
const char* pass = "your wifi password";
const char* token = "0000000000:xxxxxxxxxxxxxxxxxxxxxxxxxx";    // Telegram token
long long int userid = 0; // group chat id or user id

//#define USE_MICROPHONE
#define MAX_PHOTO_SAVED 20 // Maximum number of photo stored
#define CAMID "CAM1"
#define GROUPID "room1"
#define SW_VERSION "1.0.2"
#define DEBUG 1
#define LOG_SIZE 2097152 // 2MB
#define RGB_BRIGHTNESS 64 // Change white brightness (max 255)
// Timezone definition to get properly time from NTP server
#define MYTZ "CET-1CEST,M3.5.0,M10.5.0/3" //Europe/Amsterdam

//Select the type of memory to use
#define USE_MICROSD

//Do not touch this
#ifdef USE_MICROSD
fs::SDMMCFS mem = SD_MMC; //External microsd (up to 32GB)
#else
fs::LittleFSFS mem = LittleFS; //Internal memory (8MB)
#endif