#pragma once

#include <Arduino.h>
#include "esp_camera.h"

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

#define LAMP_PIN 4

// Image Settings
// Image sizes: 160x120 (QQVGA), 128x160 (QQVGA2), 176x144 (QCIF), 240x176 (HQVGA), 320x240 (QVGA), 400x296 (CIF), 640x480 (VGA, default), 800x600 (SVGA), 1024x768 (XGA), 1280x1024 (SXGA), 1600x1200 (UXGA)
#define FRAME_SIZE_PHOTO FRAMESIZE_VGA
//   ---------------------------------------------------------------------------------------------------------------------


// misc
#define WIDTH 320                       // motion sensing frame size from QVGA
#define HEIGHT 240
uint16_t tCounter = 0;                  // count number of consecutive triggers (i.e. how many times in a row movement has been detected)
uint16_t tCounterTrigger = 0;           // number of consequitive triggers required to count as movement detected


// forward delarations
bool setupCameraHardware(framesize_t);
bool capture_image();
esp_err_t cameraImageSettings(framesize_t);


// ---------------------------------------------------------------
//                     -Setup camera hardware
// ---------------------------------------------------------------

bool setupCameraHardware(pixformat_t format) {
    // camera configuration settings
    camera_config_t config;
    framesize_t frame_size = FRAME_SIZE_PHOTO;

    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.ledc_timer = LEDC_TIMER_1;
    config.ledc_channel = LEDC_CHANNEL_1;
    config.xclk_freq_hz = 20000000;      // XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    config.pixel_format = format;        // PIXFORMAT_ + YUV422, GRAYSCALE, RGB565, JPEG, RGB888?
    config.frame_size = frame_size;      // FRAMESIZE_ + QVGA, CIF, VGA, SVGA, XGA, SXGA, UXGA
    config.jpeg_quality = 12;            // 0-63 lower number means higher quality (can cause failed image capture if set too low at higher resolutions)
    config.fb_count = 1;                 // if more than one, i2s runs in continuous mode. Use only with JPEG

   
    // Best picture quality, but first frame requestes get lost sometimes (comment/uncomment to try)
    if (psramFound()) {
        Serial.println("PSRAM found");
        config.jpeg_quality = 10;
    }
    
    //initialize the camera
    Serial.println("Camera init... ");
    esp_err_t err = esp_camera_init(&config);

    if (err != ESP_OK) {
        delay(100);    // need a delay here or the next serial o/p gets missed
        Serial.printf("\n\nCRITICAL FAILURE: Camera sensor failed to initialise.\n\n");
        Serial.printf("A full (hard, power off/on) reboot will probably be needed to recover from this.\n");
        return false;
    } else {
        Serial.println("succeeded");

        // Get a reference to the sensor
        sensor_t* s = esp_camera_sensor_get();

        // Dump camera module, warn for unsupported modules.
        switch (s->id.PID) {
            case OV9650_PID: Serial.println("WARNING: OV9650 camera module is not properly supported, will fallback to OV2640 operation"); break;
            case OV7725_PID: Serial.println("WARNING: OV7725 camera module is not properly supported, will fallback to OV2640 operation"); break;
            case OV2640_PID: Serial.println("OV2640 camera module detected"); break;
            case OV3660_PID: Serial.println("OV3660 camera module detected"); break;
            default: Serial.println("WARNING: Camera module is unknown and not properly supported, will fallback to OV2640 operation");
        }
    }
    
    esp_err_t camerr = cameraImageSettings(frame_size);       // apply camera sensor settings

    return (camerr == ESP_OK);                    // return boolean result of camera initilisation
}


// ---------------------------------------------------------------
//             -apply camera sensor/image settings
// ---------------------------------------------------------------
esp_err_t cameraImageSettings(framesize_t fsize) {
    sensor_t *s = esp_camera_sensor_get();

    if (s == NULL) {
        Serial.println("Error: problem getting camera sensor settings");
        return ESP_ERR_NO_MEM;
    }

    s->set_brightness(s, 0);     // -2 to 2
    s->set_contrast(s, 0);       // -2 to 2
    s->set_saturation(s, 0);     // -2 to 2
    s->set_special_effect(s, 0); // 0 to 6 (0 - No Effect, 1 - Negative, 2 - Grayscale, 3 - Red Tint, 4 - Green Tint, 5 - Blue Tint, 6 - Sepia)
    s->set_whitebal(s, 1);       // 0 = disable , 1 = enable
    s->set_awb_gain(s, 1);       // 0 = disable , 1 = enable
    s->set_wb_mode(s, 0);        // white balance 0 to 4 - if awb_gain enabled (0 - Auto, 1 - Sunny, 2 - Cloudy, 3 - Office, 4 - Home)
    s->set_exposure_ctrl(s, 1);  // 0 = disable , 1 = enable
    s->set_aec2(s, 1);           // 0 = disable , 1 = enable
    s->set_ae_level(s, 0);       // -2 to 2
    s->set_aec_value(s, 300);    // 0 to 1200
    s->set_gain_ctrl(s, 1);      // 0 = disable , 1 = enable
    s->set_agc_gain(s, 0);       // 0 to 30
    s->set_gainceiling(s, (gainceiling_t)2);  // 0 to 6
    s->set_bpc(s, 0);            // 0 = disable , 1 = enable
    s->set_wpc(s, 1);            // 0 = disable , 1 = enable
    s->set_raw_gma(s, 1);        // 0 = disable , 1 = enable
    s->set_lenc(s, 1);           // 0 = disable , 1 = enable
    s->set_hmirror(s, 0);        // 0 = disable , 1 = enable
    s->set_vflip(s, 0);          // 0 = disable , 1 = enable
    s->set_dcw(s, 1);            // 0 = disable , 1 = enable
    s->set_colorbar(s, 0);       // 0 = disable , 1 = enable
    return ESP_OK;
}


// ---------------------------------------------------------------
//                          -capture image
// ---------------------------------------------------------------
bool capture_image() {
    camera_fb_t *frame_buffer = esp_camera_fb_get();          // capture frame from camera
    if (!frame_buffer) {                                      // if there was a problem grabbing a frame try again
        frame_buffer = esp_camera_fb_get();
        if (!frame_buffer) return false;                      // failed to capture image
    }

    esp_camera_fb_return(frame_buffer);                       // return frame so memory can be released
    return true;
}
