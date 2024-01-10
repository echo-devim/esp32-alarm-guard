# ESP32 Alarm Guard

ESP32 Alarm Guard is a security system based on esp32cam with a PIR sensor.
The camera uses Telegram as Command & Control communication channel, furthermore using a Telegram group you can control multiple cameras.
The system can detect movements using the PIR sensor, then it captures a photo storing it in its flash memory (4MB) using a rotating ID. In this way the system keeps always the last 30 photos.
Each time a new photo is acquired it'll be sent via Telegram using SSL.
You can start or stop the alarm, query its status, manually ask for photos with or without flash.

p.s. Manually removing the IR filter from the front of the included camera you can enable night vision

Working on Esp32Cam I had issues when the camera module is initialized at the same time we are trying to use ssl connection.
In order to workaround the issue, the esp32 board uses the camera module **or** the ssl connection switching among different states.



