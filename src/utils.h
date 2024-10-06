#include <Arduino.h>
#include <LITTLEFS.h>
#include <Preferences.h>

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
    size_t save(const char *key, unsigned long value) {
        return settings.putULong64(key, value);
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
    unsigned long read(const char *key, unsigned long default_value) {
        return settings.getULong64(key, default_value);
    }
    bool read(const char *key, bool default_value) {
        return settings.getBool(key, default_value);
    }
    String read(const char *key, String default_value) {
        return settings.getString(key, default_value);
    }            
};

String getLogs() {
    String logname = "/logs.txt";
    String content = "";
    if (LittleFS.exists(logname)) {
        File f = LittleFS.open(logname, FILE_READ);
        if (!f) {
            Serial.println("ERROR: Cannot read logs file");
            return content;
        }
        content = f.readString();
        f.close();
    }
    return content;
}

void log(String message, bool serial_only = false) {
    //Simple log function with basic rotating mechanism
    Serial.println(message);
    if (serial_only) return;
    String logname = "/logs.txt";
    bool overwrite = false;
    String prev_content = "";
    if (LittleFS.exists(logname)) {
        File f = LittleFS.open(logname, FILE_READ);
        if (!f) {
            Serial.println("ERROR: Cannot read logs file");
            return;
        }
        size_t filesize = f.size();
        if (filesize > 1024) {
            overwrite = true;
            prev_content += f.readString();
        }
        f.close();
    }
    //Format: <date> <message> <newline>
    char datestr[80];
    time_t rawtime; 
    struct tm* timeinfo; 
    time(&rawtime); 
    timeinfo = localtime(&rawtime); 
    strftime(datestr, 80, "%d%m%Y %H:%M:%S",timeinfo);
    message = String(datestr) + " " + message + "\n";
    if (overwrite) {
        message = prev_content.substring(1024) + message;
        File f = LittleFS.open(logname, FILE_WRITE);
        if (!f) {
            Serial.println("ERROR: Cannot open logs file");
            return;
        }
        f.write((uint8_t*) message.c_str(), message.length());
        f.close();
    } else {
        File f = LittleFS.open(logname, FILE_APPEND);
        if (!f) {
            Serial.println("ERROR: Cannot open logs file");
            return;
        }
        f.write((uint8_t*) message.c_str(), message.length());
        f.close();
    }
}

void dumpChipInfo() {
    Serial.println("\n\n================================");
    Serial.print("Chip Model: ");
    Serial.println(ESP.getChipModel());
    Serial.print("Chip version: ");
    Serial.println(ESP.getChipRevision());
    Serial.print("Numer of cores: ");
    Serial.println(ESP.getChipCores());
    Serial.print("Flash Chip Size: ");
    Serial.println(ESP.getFlashChipSize());
    Serial.print("Flash Chip Speed: ");
    Serial.println(ESP.getFlashChipSpeed());
    Serial.print("CPU Freq: ");
    Serial.println(ESP.getCpuFreqMHz());
    Serial.print("Heap size: ");
    Serial.println(ESP.getHeapSize());
    if (psramFound()) {
        Serial.printf("Total PSRAM: %d\n", ESP.getPsramSize());
        Serial.printf("Free PSRAM: %d\n", ESP.getFreePsram());
    } else {
        Serial.println("PSRAM not found");
    }
    Serial.println("================================");
}

String get_wifi_status(int status){
    switch(status){
        case WL_IDLE_STATUS:
        return "WL_IDLE_STATUS";
        case WL_SCAN_COMPLETED:
        return "WL_SCAN_COMPLETED";
        case WL_NO_SSID_AVAIL:
        return "WL_NO_SSID_AVAIL";
        case WL_CONNECT_FAILED:
        return "WL_CONNECT_FAILED";
        case WL_CONNECTION_LOST:
        return "WL_CONNECTION_LOST";
        case WL_CONNECTED:
        return "WL_CONNECTED";
        case WL_DISCONNECTED:
        return "WL_DISCONNECTED";
    }
    return "UNKNOWN";
}

void listDir(fs::FS &fs, const char * dirname, uint8_t levels){
    Serial.printf("Listing directory: %s\r\n", dirname);

    File root = fs.open(dirname);
    if(!root){
        Serial.println("- failed to open directory");
        return;
    }
    if(!root.isDirectory()){
        Serial.println(" - not a directory");
        return;
    }

    File file = root.openNextFile();
    while(file){
        if(file.isDirectory()){
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if(levels){
                listDir(fs, file.path(), levels -1);
            }
        } else {
            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("\tSIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
}