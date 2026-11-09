#include "voice_filter.h"
#include "vendor/include/fvad.h"

static Fvad *s_detector;
static int s_rate, s_mode;
static int16_t s_frame[480]; /* max: 10 ms at 48 kHz */
static size_t s_used, s_frame_samples;
static bool s_last_speech;
static bool s_valid;

bool voice_filter_init(int sample_rate, int mode)
{
    s_valid = false;
    s_used = 0;
    s_last_speech = false;
    if (!s_detector) s_detector = fvad_new();
    if (!s_detector) return false;
    fvad_reset(s_detector);
    if (fvad_set_sample_rate(s_detector, sample_rate) != 0 ||
        fvad_set_mode(s_detector, mode) != 0) return false;
    s_rate = sample_rate;
    s_mode = mode;
    s_frame_samples = (size_t)sample_rate / 100U;
    s_valid = true;
    return true;
}

void voice_filter_reset(void)
{
    if (s_detector) (void)voice_filter_init(s_rate, s_mode);
}

bool voice_filter_process(const int32_t *words, size_t count)
{
    if (!s_valid) return false; /* failure never opens green */
    bool speech = false;
    bool completed = false;
    for (size_t i = 0; i < count; i++) {
        /* Top 16 signed bits, no gain or AGC before classification. */
        s_frame[s_used++] = (int16_t)(words[i] >> 16);
        if (s_used == s_frame_samples) {
            int result = fvad_process(s_detector, s_frame, s_frame_samples);
            s_used = 0;
            if (result < 0) {
                s_valid = false;
                s_last_speech = false;
                return false;
            }
            completed = true;
            s_last_speech = result != 0;
            speech = speech || s_last_speech;
        }
    }
    /* Small configurable FFT blocks may not complete a VAD frame yet. */
    return completed ? speech : s_last_speech;
}
