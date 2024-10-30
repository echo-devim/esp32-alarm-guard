
#pragma once
#include <Arduino.h>
#include <driver/i2s.h>
#include "config.h"

// I2S pins for microphone INMP441
#define I2S_WS            GPIO_NUM_21
#define I2S_SCK           GPIO_NUM_42 
#define I2S_SD            GPIO_NUM_47

// I2S peripheral to use (0 or 1)
#define I2S_PORT          I2S_NUM_0

//---- Sampling ------------
#define SAMPLE_RATE       22050 // Sample rate of the audio 22KHz, 16-bit per sample, mono
#define SAMPLE_BITS       I2S_BITS_PER_SAMPLE_16BIT   // Bits per sample of the audio
#define CHANNELS          I2S_CHANNEL_MONO
#define BYTES_PER_SECOND  (CHANNELS * SAMPLE_RATE * (SAMPLE_BITS / 8))
#define AUDIO_DURATION_FILE_WRITE_MS 500 // Not enough RAM to buffer long audio durations, so need to write out smaller durations to file, e.g. every 500 ms
#define DMA_BUF_LEN       ((BYTES_PER_SECOND * AUDIO_DURATION_FILE_WRITE_MS) / 1000) // Size of the DMA copy buffer that can hold audio data for the duration of AUDIO_DURATION_FILE_WRITE_MS
//---- Audio WAV configuration ------------
// Static audio buffers for each time we write data to a file, for a duration of AudioDurationPerFileWriteInMs
DMA_ATTR static uint8_t DMACopyBuffer[DMA_BUF_LEN];  // DMA buffers are copied into this buffer when calling i2s_read()
//static uint8_t FileWriteBuffer[DMA_BUF_LEN];         // Audio data processed into this buffer and used to write to file

// Calc DMA buffer info to avoid buffer overflow, which will drop audio while copying out of DMA buffers
const int SPIFFSWriteSpeed = 20;  // KB/s Just testing on my own, I only get 20-32KB per second writing to a SPIFFS file :-(
const int SDCardWriteSpeed = 180; // KB/s Just testing on my own, I get 180-220KB per second writing to a SD Card
const int TimeToWriteDataToFileInMs = DMA_BUF_LEN / SDCardWriteSpeed;
const int ExtraProcessingTimeInMs = 5;  // Let's add a few extra ms of processing time to accomodate for copying, rescaling, printfs, etc.
const int EstimatedTimeToProcessAudioBufferInMs = TimeToWriteDataToFileInMs + ExtraProcessingTimeInMs;
const int TotalNumSamples = EstimatedTimeToProcessAudioBufferInMs * SAMPLE_RATE / 1000;
const int TotalBytesRequiredForDMA = TotalNumSamples * CHANNELS * SAMPLE_BITS / 8;

// Derive buf_len and buf_count based on latency preference, i.e. choose high, medium or low. By default this example uses medium
const int NumSamplesPerBuffer_HighLatency = 1024;
const int NumSamplesPerBuffer_MediumLatency = 256;
const int NumSamplesPerBuffer_LowLatency = 64;
const int NumSamplesPerBuffer_VeryLowLatency = 8;
const int NumSamplesPerBuffer = min(NumSamplesPerBuffer_MediumLatency, 1024);                       // i2s buf_len (Must be less than 1024)
const int NumBuffers = min(max(int(float(TotalNumSamples / NumSamplesPerBuffer) + 1.0f), 2), 128);  // i2s buf_count (Must be between 2 and 128)

struct WAVHeader {
    char chunkId[4];          // 4 bytes
    uint32_t chunkSize;       // 4 bytes
    char format[4];           // 4 bytes
    char subchunk1Id[4];      // 4 bytes
    uint32_t subchunk1Size;   // 4 bytes
    uint16_t audioFormat;     // 2 bytes
    uint16_t numChannels;     // 2 bytes
    uint32_t sampleRate;      // 4 bytes
    uint32_t byteRate;        // 4 bytes
    uint16_t blockAlign;      // 2 bytes
    uint16_t bitsPerSample;   // 2 bytes
    char subchunk2Id[4];      // 4 bytes
    uint32_t subchunk2Size;   // 4 bytes
};

void initializeWAVHeader(WAVHeader &header, uint32_t audioSize) {
    strncpy(header.chunkId, "RIFF", 4);
    strncpy(header.format, "WAVE", 4);
    strncpy(header.subchunk1Id, "fmt ", 4);
    strncpy(header.subchunk2Id, "data", 4);
    header.chunkSize = audioSize + sizeof(WAVHeader) - 8;
    header.subchunk1Size = 16; // PCM format size (constant for uncompressed audio)
    header.audioFormat = 1; // PCM audio format (constant for uncompressed audio)
    header.numChannels = CHANNELS;
    header.sampleRate = SAMPLE_RATE;
    header.bitsPerSample = SAMPLE_BITS;
    header.byteRate = BYTES_PER_SECOND;
    header.blockAlign = (SAMPLE_BITS * CHANNELS) / 8;
    header.subchunk2Size = audioSize; // data size
}

// Adjust volume
void i2s_adc_data_scale(uint8_t* d_buff, uint8_t* s_buff, uint32_t len)
{
    uint32_t j = 0;
    uint32_t dac_value = 0;
    for (int i = 0; i < len; i += 2)
    {
        dac_value = ((((uint16_t) (s_buff[i + 1] & 0xf) << 8) | ((s_buff[i + 0]))));
        d_buff[j++] = 0;
        d_buff[j++] = dac_value * 256 / 2048;
    }
}

bool i2s_init()
{
    i2s_config_t i2s_config =
    {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = SAMPLE_BITS,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = NumBuffers,
        .dma_buf_len = NumSamplesPerBuffer,
        .use_apll = true
    };

    esp_err_t e = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (e != ESP_OK) {
        return false;
    }

    const i2s_pin_config_t pin_config =
    {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD
    };

    e = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (e != ESP_OK) {
        return false;
    }

    return true;
}

size_t recordAudio(fs::File& outfile, int recording_seconds = 5) {
    const int audio_size = recording_seconds * BYTES_PER_SECOND;
    WAVHeader wavHeader;
    initializeWAVHeader(wavHeader, audio_size);
    Serial.println("Recording");
    outfile.write(reinterpret_cast<const uint8_t*>(&wavHeader), sizeof(wavHeader));
    size_t numBytesRead;
    size_t numBytesWrittenToFile = 0;
    // Drain a bit of the DMA buffers before starting the recording
    i2s_read(I2S_NUM_0, (void*)DMACopyBuffer, DMA_BUF_LEN, &numBytesRead, portMAX_DELAY);
    Serial.printf("\nRecording %d seconds of audio ...\n", recording_seconds);
    // Time the total duration
    unsigned long startTime = millis();
    while (numBytesWrittenToFile < audio_size)
    {
        // Read data from DMA buffers into our copy buffer
        unsigned long startRead = millis();
        i2s_read(I2S_NUM_0, (void*)DMACopyBuffer, DMA_BUF_LEN, &numBytesRead, portMAX_DELAY);
        unsigned long stopRead = millis();

        // Write to file
        unsigned long startWrite = millis();
        outfile.write((const byte*)DMACopyBuffer, DMA_BUF_LEN);
        unsigned long stopWrite = millis();

        numBytesWrittenToFile += DMA_BUF_LEN;

        Serial.printf("    %u%% (i2s_read %d ms, file.write %d ms )\n", numBytesWrittenToFile * 100 / audio_size, stopRead - startRead, stopWrite - startWrite);
    }

    Serial.printf("Done. Recorded %d seconds in %d ms\n", audio_size, millis() - startTime);
    Serial.flush();

    return numBytesWrittenToFile;
}