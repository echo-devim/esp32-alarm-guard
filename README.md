# ESP32 Alarm Guard

Project status: Experimental

ESP32 Alarm Guard is a security system based on esp32cam board (AI Thinker) **without** a PIR sensor for motion detection.
The camera uses Telegram as Command & Control communication channel, thus you need to setup a new Telegram bot using BotFather.
Telegram offers many advantages, among them you can join the bot into a private group where one or more people can control multiple cameras (all linked to the same bot if you wish).
The system can detect movements using a custom algorithm that compares the pixels changed in two images. When a movement is detected, it saves the photo storing it in its flash memory (4MB) using a rotating ID. In this way the system keeps always the last 30 photos. The esp32cam supports up to 4GB microsd card storage, but actually I didn't implement it, because I don't need at the moment.
Each time a new photo is acquired it'll be sent via Telegram using SSL.
You can start or stop the alarm, query its status, manually ask for photos with or without flash.

p.s. You can enable a quite decent night vision manually removing the IR filter from the front of the included camera. You need also to set the camera lens with the right focus, because the camera doesn't have auto-focus.

While working with Esp32Cam I had issues when the camera module is initialized at the same time the code is trying to use a ssl connection.
In order to workaround the issue, the project uses the camera module **or** the ssl connection switching among different statuses (aka bootmodes).



