/*
 * Audio output to the on-board PCM5101 I2S DAC (Waveshare ESP32-S3-Touch-LCD-1.85:
 * BCK=GPIO48, WS=GPIO38, DIN=GPIO47). The PCM5101 has no digital volume
 * register, so volume is applied in software by scaling the PCM samples.
 */
#pragma once

/* Init the I2S TX channel to the PCM5101 DAC. Safe to call repeatedly. */
void audio_init(void);

/* Set the output volume, 0-100. Scales every sample written to the DAC. */
void audio_set_volume(int vol);

/* Current volume, 0-100. */
int audio_get_volume(void);

/* Play a short, soft test tone at the current volume. Blocks for its
 * duration (~140 ms); no-op if audio is not initialized or volume is 0. */
void audio_play_beep(void);
