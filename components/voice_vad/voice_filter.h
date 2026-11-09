#ifndef SONICSYNC_VOICE_FILTER_H
#define SONICSYNC_VOICE_FILTER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One persistent detector; allocation occurs only during initialization. */
bool voice_filter_init(int sample_rate, int mode);
void voice_filter_reset(void);
/* Signed INMP441 samples in bits 31:8. Carries incomplete 10 ms frames. */
bool voice_filter_process(const int32_t *words, size_t count);
#endif
