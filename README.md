# ESP32 Alarm Guard

Project status: Beta

ESP32 Alarm Guard is a security system based on Freenove esp32-s3-wroom-1 board with OV2640 camera and mic INMP441 for motion and audio detection.
The camera uses Telegram as Command & Control communication channel, thus you need to setup a new Telegram bot using BotFather.
Telegram offers many advantages, among them you can join the bot into a private group where one or more people can control multiple cameras (all linked to the same bot if you wish).
The system can detect movements using a custom algorithm that compares the pixels changed in two images. When a movement is detected, it saves the photo storing it in its flash memory (8MB) using a rotating ID. In this way the system keeps always the last 20 photos. The esp32 supports a microsd card storage up to 32GB.
Each time a new photo is acquired it'll be sent via Telegram using SSL.
You can start or stop the alarm, query its status, manually ask for photos with or without flash.

The audio detection is performed analyzing the sound wave. If there are too many anomalies above a configurable threshold an alarm is sent. Each anomaly is represented by the samples in the signal greater than a constant value. High frequency signals, tipically alarm sounds, are sent without checking the threshold. 
So we have two kind of triggers: too many samples with enough big amplitude or signal with high frequency (even with a low amplitude!). 

p.s. You can enable a quite decent night vision manually removing the IR filter from the front of the included camera. You need also to set the camera lens with the right focus, because the camera doesn't have auto-focus.

## Issues

Compared to esp32cam, this board can use both camera sensor and Telegram SSL connection without issues.
However, the camera sensors OV5640 tends to overheat a lot, even with a heatsink. I used a OV2640 sensor that seems to work well.

The attempt with microphone MAX9814 failed, because it doesn't support I2S interface. The microphone INMP441 can successfully record audio to wav file.





