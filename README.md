# ESP32 Alarm Guard

Project status: Experimental

ESP32 Alarm Guard is a security system based on Freenove esp32-s3-wroom-1 board with a mic MAX9814 for motion and audio detection.
The camera uses Telegram as Command & Control communication channel, thus you need to setup a new Telegram bot using BotFather.
Telegram offers many advantages, among them you can join the bot into a private group where one or more people can control multiple cameras (all linked to the same bot if you wish).
The system can detect movements using a custom algorithm that compares the pixels changed in two images. When a movement is detected, it saves the photo storing it in its flash memory (8MB) using a rotating ID. In this way the system keeps always the last 30 photos. The esp32 supports a microsd card storage, but actually I didn't implement it, because I don't need at the moment.
Each time a new photo is acquired it'll be sent via Telegram using SSL.
You can start or stop the alarm, query its status, manually ask for photos with or without flash.

p.s. You can enable a quite decent night vision manually removing the IR filter from the front of the included camera. You need also to set the camera lens with the right focus, because the camera doesn't have auto-focus.

## Issues

Compared to esp32cam, this board can use both camera (OV5640 sensor) and Telegram SSL connection without issues.
However, the camera sensors tends to overheat a lot, so it is necessary to use a heatsink.



