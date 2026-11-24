/*
 * Sounds for action events (gui-design.md: Every action is an event).
 *
 * One PipeWire stream ("WPS sounds", role Event) with a small buffer
 * (256 frames, 5 ms at 48 kHz: G0 measured 2-5 ms from action to the
 * stream), paused while nothing plays. Sounds are decoded once into
 * memory as 48 kHz stereo floats: with libsndfile (WAV, Ogg Vorbis, FLAC,
 * Opus: the freedesktop sound theme is Ogg) when it was there at build
 * time, else WAV only. Up to 8 play at once.
 */
#ifndef OSD_SOUND_H
#define OSD_SOUND_H

#include <stdbool.h>

/* Start the player; false (and silence) when PipeWire is not there. */
bool sound_init(void);
void sound_shutdown(void);

/* Play a sound file at volume 0..1 (decoded the first time). False if the
 * file cannot be read. Any thread. */
bool sound_play(const char *path, float volume);

/* How many sounds have been started, and whether PipeWire is connected
 * (self-tests). */
unsigned sound_played(void);
bool sound_ready(void);

/* Which formats can be read: "wav" or "wav ogg flac opus". */
const char *sound_formats(void);

#endif
