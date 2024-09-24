// CONFIG

const char* ssid = "your wifi name";
const char* pass = "your wifi password";
const char* token = "0000000000:xxxxxxxxxxxxxxxxxxxxxxxxxx";    // Telegram token
long long int userid = 0; // group chat id or user id

//#define USE_MICROPHONE
#define MICROPHONE_PIN GPIO_NUM_2
#define MAX_PHOTO_SAVED 30 // Maximum number of photo stored
#define CAMID "CAM1"
#define GROUPID "room1"
#define SW_VERSION "1.0.1"
#define DEBUG 1
// Timezone definition to get properly time from NTP server
#define MYTZ "CET-1CEST,M3.5.0,M10.5.0/3" //Europe/Amsterdam