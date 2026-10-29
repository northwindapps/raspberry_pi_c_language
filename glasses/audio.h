#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>

#define AUDIO_RATE   16000          /* output: 16 kHz mono int16 */
#define AUDIO_CHUNK  1280           /* samples per chunk (80 ms) */
#define AUDIO_BUFFER_SECONDS 10

typedef struct audio_sub audio_sub_t;

/* Start the capture thread. Returns 0 on success. */
int audio_start(void);

/* Copy up to max_samples of the most recent audio; returns samples copied. */
size_t audio_get_recent(int16_t *dst, size_t max_samples);

/* Live stream: each subscriber gets its own queue of chunks.
 * audio_sub_read blocks until a chunk is ready (or 5 s pass, returning 0). */
audio_sub_t *audio_subscribe(void);
int audio_sub_read(audio_sub_t *sub, int16_t chunk[AUDIO_CHUNK]);
void audio_unsubscribe(audio_sub_t *sub);

#endif
