// CONFIG

//Rename this file into config.h !!
const char* ssid = "your wifi name";
const char* pass = "your wifi password";
const char* token = "0000000000:xxxxxxxxxxxxxxxxxxxxxxxxxx";    // Telegram token
long long int userid = 0; // group chat id or user id
int waiting_time = 15; // deep sleep time in seconds used to wait input from PIR sensor

// Timezone definition to get properly time from NTP server
#define MYTZ "CET-1CEST,M3.5.0,M10.5.0/3" //Europe/Amsterdam
#define PIR_PIN GPIO_NUM_12
#define ONBOARD_LED_PIN GPIO_NUM_33
#define CAMID "CAM1" //Unique name of this camera, you can connect multiple cameras to the same telegram group