#ifndef PVZ2NATIVE_AUDIO_SOUND_REPOSITORY_H
#define PVZ2NATIVE_AUDIO_SOUND_REPOSITORY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *name;
    uint8_t *buffer;
    size_t buffer_size;
    int freq;
    int num_channels;
    int bits_per_sample;
} pvz2native_sound_resource_t;

typedef struct {
    char *name;
    size_t resources_count;
    pvz2native_sound_resource_t resources[4];
} pvz2native_sound_t;

pvz2native_sound_resource_t *pvz2native_get_sound_buffer(char *name);

#endif