/* Compile the real firmware against host-only peripheral stubs. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

typedef int esp_err_t;
typedef int ledc_channel_t;
typedef void *i2s_chan_handle_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define GPIO_MODE_OUTPUT 1
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_CHANNEL_0 0
#define LEDC_CHANNEL_1 1
#define LEDC_TIMER_12_BIT 12
#define LEDC_TIMER_0 0
#define LEDC_AUTO_CLK 0
#define LEDC_SLEEP_MODE_NO_ALIVE_NO_PD 0
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 0
#define I2S_CLK_SRC_DEFAULT 0
#define I2S_MCLK_MULTIPLE_384 384
#define I2S_DATA_BIT_WIDTH_24BIT 24
#define I2S_DATA_BIT_WIDTH_32BIT 32
#define I2S_SLOT_BIT_WIDTH_32BIT 32
#define I2S_SLOT_MODE_STEREO 2
#define I2S_SLOT_MODE_MONO 1
#define I2S_STD_SLOT_LEFT 1
#define I2S_GPIO_UNUSED (-1)
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
static void host_log(const char *tag, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    (void)tag;
}
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGD(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGE(...) host_log(__VA_ARGS__)

typedef struct {
    int speed_mode, duty_resolution, timer_num, freq_hz, clk_cfg;
    bool deconfigure;
} ledc_timer_config_t;
typedef struct {
    int gpio_num, speed_mode, channel, timer_sel;
    uint32_t duty, hpoint;
    int sleep_mode;
    struct { unsigned output_invert; } flags;
    bool deconfigure;
} ledc_channel_config_t;
typedef struct { unsigned dma_frame_num, dma_desc_num; } i2s_chan_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) ((i2s_chan_config_t){0})
typedef struct {
    struct {
        unsigned sample_rate_hz;
        int clk_src;
        unsigned ext_clk_freq_hz, mclk_multiple, bclk_div;
    } clk_cfg;
    struct {
        int data_bit_width, slot_bit_width, slot_mode, slot_mask;
        unsigned ws_width;
        bool ws_pol, bit_shift, left_align, big_endian, bit_order_lsb;
    } slot_cfg;
    struct {
        int mclk, bclk, ws, din, dout;
        struct { bool mclk_inv, bclk_inv, ws_inv; } invert_flags;
    } gpio_cfg;
} i2s_std_config_t;

static uint32_t duty[2];
static unsigned elapsed_ms, animation_events;
static bool trace_animation;
static i2s_std_config_t rx_config;
static int32_t input_words[CONFIG_SONICSYNC_FFT_SIZE];
static size_t input_pos, read_chunk;
static bool read_error, read_empty;
static uint32_t last_timeout_ms;

static esp_err_t gpio_set_direction(int pin, int mode) { return ESP_OK; }
static esp_err_t gpio_set_level(int pin, int level) { return ESP_OK; }
static esp_err_t ledc_timer_config(const ledc_timer_config_t *c) { return ESP_OK; }
static esp_err_t ledc_channel_config(const ledc_channel_config_t *c)
{
    duty[c->channel] = c->duty;
    return ESP_OK;
}
static esp_err_t ledc_set_duty(int mode, int channel, uint32_t value)
{
    duty[channel] = value;
    if (trace_animation) {
        if (animation_events < 36) {
            unsigned flash = animation_events / 2;
            int expected_channel = (flash / 3) % 2;
            assert(channel == expected_channel);
            assert(value == ((animation_events % 2) ? 0U :
                             CONFIG_SONICSYNC_ANIM_BRIGHTNESS));
            assert(elapsed_ms == animation_events * 150U);
        } else {
            assert(animation_events < 38 && value == 0);
        }
        assert(!(duty[0] && duty[1]));
        animation_events++;
    }
    return ESP_OK;
}
static esp_err_t ledc_update_duty(int mode, int channel) { return ESP_OK; }
static void vTaskDelay(unsigned ms) { elapsed_ms += ms; }
static const char *esp_err_to_name(esp_err_t err) { return "test error"; }
static esp_err_t i2s_new_channel(const i2s_chan_config_t *c, void *tx,
                               i2s_chan_handle_t *rx)
{
    *rx = &rx_config;
    return ESP_OK;
}
static esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t h,
                                         const i2s_std_config_t *c)
{
    rx_config = *c;
    return ESP_OK;
}
static esp_err_t i2s_channel_enable(i2s_chan_handle_t h) { return ESP_OK; }
static esp_err_t i2s_channel_read(i2s_chan_handle_t h, void *dest,
                                size_t size, size_t *got, uint32_t timeout)
{
    last_timeout_ms = timeout;
    *got = 0;
    if (read_error) return ESP_ERR_TIMEOUT;
    if (read_empty) return ESP_OK;
    if (size > sizeof(input_words) - input_pos)
        size = sizeof(input_words) - input_pos;
    if (read_chunk && size > read_chunk) size = read_chunk;
    memcpy(dest, (uint8_t *)input_words + input_pos, size);
    input_pos += size;
    *got = size;
    return ESP_OK;
}

#ifndef SONICSYNC_SOURCE
#define SONICSYNC_SOURCE "../src/main.c"
#endif
#include SONICSYNC_SOURCE

static void reset_fixture(void)
{
    s_bass_on = s_voice_on = s_music_on = false;
#ifndef CFG_VOICE_ATTACK_MS
    s_env_bass_db = -120.0f;
#endif
    s_env_voice_db = -120.0f;
    s_block_count = 0;
    duty[0] = duty[1] = 0;
    read_error = read_empty = false;
    read_chunk = 0;
    memset(input_words, 0, sizeof(input_words));
    window_init();
    band_bins_init();
    envelope_init();
#ifdef CFG_BASS_OFFSET
    bass_control_reset();
#endif
}

/* Fixture data follows the mic contract: signed 24-bit PCM in bits 31:8. */
static void tones(float bass, float voice, float dc)
{
    for (int i = 0; i < CFG_FFT_N; i++) {
        float t = (float)i / CFG_SAMPLE_RATE;
        float sample = dc + bass * sinf(2 * FFT_PI * 125 * t)
                          + voice * sinf(2 * FFT_PI * 1000 * t);
        int32_t pcm = (int32_t)lroundf(sample * 8388608.0f);
        input_words[i] = pcm * 256;
    }
}

/* Single-frequency fixture for band-edge checks. */
static void tone_hz(float peak, float hz)
{
    for (int i = 0; i < CFG_FFT_N; i++) {
        float t = (float)i / CFG_SAMPLE_RATE;
        float sample = peak * sinf(2 * FFT_PI * hz * t);
        int32_t pcm = (int32_t)lroundf(sample * 8388608.0f);
        input_words[i] = pcm * 256;
    }
}

static void frames(unsigned count)
{
    while (count--) {
        input_pos = 0;
        process_audio_block();
        assert(duty[0] <= CFG_MAX_BRIGHT && duty[1] <= CFG_MAX_BRIGHT);
#ifdef CFG_VOICE_ATTACK_MS
        assert(duty[1] == 0 || duty[1] == CFG_MAX_BRIGHT);
#endif
    }
}

static void quiet_noise(void)
{
    uint32_t rng = 12345;
    for (int i = 0; i < CFG_FFT_N; i++) {
        rng = rng * 1664525U + 1013904223U;
        int32_t pcm = (int32_t)(rng >> 26) - 32;
        input_words[i] = pcm * 256;
    }
}

static void warm_up_quiet(void)
{
    quiet_noise();
#ifdef CFG_BASS_OFFSET
    frames(BASS_CAL_FRAMES);
#endif
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--green-trace")) {
        reset_fixture();
        const float bass_levels[] = {0, 0.003f, 0.03f, 0.1f, 0.4f};
        const float voice_levels[] = {0, 0.004f, 0.01f, 0.1f, 0.3f};
        for (int block = 0; block < 1200; block++) {
            tones(bass_levels[(block / 17) % 5], voice_levels[(block / 7) % 5], 0);
            read_error = block == 333 || block == 777;
            frames(1);
            printf("%u\n", duty[0]);
        }
        return 0;
    }
#ifdef CFG_BLUE_FORCE_OFF
    if (argc > 1 && !strcmp(argv[1], "--blue-off-smoke")) {
        reset_fixture();
        warm_up_quiet();
        tones(0.1f, 0.1f, 0);
        frames(60);
        assert(CFG_BLUE_FORCE_OFF && s_bass_on && !duty[1] && duty[0] > 0);
        assert(s_blue_on_requests > 0);
        puts("PASS: diagnostic forced OFF affects blue only, green remains active");
        return 0;
    }
#endif
#ifdef CFG_BASS_OFFSET
    if (argc > 1 && !strcmp(argv[1], "--speech-probe")) {
        reset_fixture();
        warm_up_quiet();
        tones(0, 0.1f, 0);
        frames(1);
        uint32_t first = duty[0];
        frames(59);
        uint32_t steady = duty[0];
        tones(0, 0.01f, 0);
        frames(3);
        printf("Speech response: first 32 ms block=%u, steady=%u, "
               "after 96 ms quieter speech=%u\n", first, steady, duty[0]);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--config-smoke")) {
        reset_fixture();
        warm_up_quiet();
        assert(s_bass_calibrated);
        tones(0.1f, 0.1f, 0);
        frames(60);
        assert(duty[0] > 0 && duty[1] > 0);
        quiet_noise();
        frames(1);
        assert(!duty[0] && !duty[1]);
        assert(s_reference_db >= BASE_REFERENCE_DB);
        puts("PASS: configurable calibration/confirmation bypass and silence-to-OFF");
        return 0;
    }
#endif
    /* First test reproduces the reported fault on the original source. */
    reset_fixture();
    quiet_noise();
    frames(60);
    printf("Quiet input: green=%u blue=%u bass=%.2f dB voice=%.2f dB\n",
           duty[0], duty[1], energy_to_db(band_energy(s_bin_bass_lo, s_bin_bass_hi)),
           energy_to_db(band_energy(s_bin_voice_lo, s_bin_voice_hi)));
    fflush(stdout);
    if (argc > 1 && !strcmp(argv[1], "--reproduce")) {
        assert(duty[0] == CFG_MAX_BRIGHT && duty[1] == CFG_MAX_BRIGHT);
        puts("REPRODUCED: quiet noise pins both original outputs at maximum");
        return 0;
    }
    assert(!duty[0] && !duty[1] && !s_bass_on && !s_voice_on);

    if (argc > 1 && !strcmp(argv[1], "--reproduce-ambient")) {
        reset_fixture();
        tones(0.03f, 0, 0);
        frames(100);
        printf("Sustained background bass: green=%u blue=%u\n", duty[0], duty[1]);
        assert(duty[1] > 0);
        puts("REPRODUCED: original shared gate lights blue on background hum");
        return 0;
    }

    reset_fixture();
    frames(10);
    assert(!duty[0] && !duty[1]);
    for (int i = 0; i < CFG_FFT_N; i++) input_words[i] = i & 255;
    frames(10);
    assert(!duty[0] && !duty[1]);
    tones(0, 0, 0.2f);
    frames(10);
    assert(!duty[0] && !duty[1]);
    puts("PASS: silence, quiet noise, unused low bits and DC offset stay off");

    reset_fixture();
    warm_up_quiet();
    tones(0.1f, 0, 0);
    frames(60);
    float bass_db = energy_to_db(band_energy(s_bin_bass_lo, s_bin_bass_hi));
    assert(fabsf(bass_db - (-23.0103f)) < 0.1f);
    assert(!duty[0] && duty[1] == CFG_MAX_BRIGHT);
    printf("PASS: 125 Hz tone -> blue=%u green=0, level=%.2f dBFS\n",
           duty[1], bass_db);

    reset_fixture();
    warm_up_quiet();
    tones(0.04f, 0, 0);
    frames(60);
    assert(duty[1] == CFG_MAX_BRIGHT && !duty[0]);
    puts("PASS: quieter accepted bass uses the same binary ON brightness");

    reset_fixture();
    warm_up_quiet();
    tones(0, 0.1f, 0);
    read_chunk = 68; /* Exercise the real partial-read accumulation loop. */
    frames(60);
    assert(duty[0] > 0 && duty[0] < CFG_MAX_BRIGHT && !duty[1]);
    printf("PASS: 1000 Hz tone -> green=%u blue=0, including partial reads\n", duty[0]);

    reset_fixture();
    warm_up_quiet();
    tones(0.1f, 0.1f, 0);
    frames(60);
    assert(duty[0] > 0 && duty[1] > 0);
    quiet_noise();
    frames(1);
    assert(!duty[0] && !duty[1] && !s_bass_on && !s_voice_on);
    puts("PASS: mixed audio -> both channels, next quiet block -> both off");

    reset_fixture();
    const float rms_to_peak = 1.41421356237f;
    tones(0, rms_to_peak * powf(10, -48.0f / 20), 0);
    frames(60);
    assert(!s_voice_on && !duty[0]);
    tones(0, rms_to_peak * powf(10, -46.0f / 20), 0);
    frames(60);
    assert(s_voice_on && duty[0] > 0);
    tones(0, rms_to_peak * powf(10, -48.0f / 20), 0);
    frames(1);
    assert(s_voice_on);
    tones(0, rms_to_peak * powf(10, -51.0f / 20), 0);
    frames(1);
    assert(!s_voice_on && !duty[0]);
    puts("PASS: green retains -47/-50 dBFS opening/closing hysteresis");

    warm_up_quiet();
    tones(0.1f, 0.1f, 0);
    frames(60);
    read_error = true;
    frames(1);
    assert(!duty[0] && !duty[1] && !s_bass_on && !s_voice_on && !s_music_on);
    assert(s_env_voice_db == -120.0f);
#ifndef CFG_VOICE_ATTACK_MS
    assert(s_env_bass_db == -120.0f);
#endif
    assert(last_timeout_ms < 1000 && last_timeout_ms >= 32);
    read_error = false;
    quiet_noise();
    frames(1);
    assert(!duty[0] && !duty[1]);
    warm_up_quiet();
    tones(0.1f, 0.1f, 0);
    frames(60);
    read_empty = true;
    frames(1);
    assert(!duty[0] && !duty[1] && !s_music_on);
    puts("PASS: timeout/empty capture clears PWM, gates and envelopes");

#ifdef CFG_BASS_OFFSET
    reset_fixture();
    tones(0, 0.1f, 0);
    frames(20);
    assert(!s_bass_calibrated && !duty[1] && duty[0] > 0);
    puts("PASS: blue stays dark during background calibration; green works");

    reset_fixture();
    /* A steady low-frequency background at -33.47 dBFS fooled the old gate. */
    tones(0.03f, 0, 0);
    frames(BASS_CAL_FRAMES + 100);
    assert(s_bass_calibrated && !s_bass_on && !duty[1] && !duty[0]);
    assert(s_bass_gate_db > -30.0f);
    /* Voice should remain useful even with bass background present. */
    tones(0.03f, 0.1f, 0);
    frames(100);
    assert(!duty[1] && duty[0] > 0);
    tones(0.4f, 0.1f, 0);
    frames(60);
    assert(duty[1] > 0 && duty[0] > 0);
    tones(0.03f, 0, 0);
    frames(1);
    assert(!duty[1] && !duty[0]);
    puts("PASS: learned hum suppressed, stronger bass detected, voice preserved");

    reset_fixture();
    warm_up_quiet();
    for (int burst = 0; burst < 20; burst++) {
        tones(0.1f, 0, 0);
        frames(1);
        assert(!s_bass_on && !duty[1]);
        quiet_noise();
        frames(1);
    }
    tones(0.1f, 0, 0);
    frames(2);
    assert(s_bass_on && duty[1] > 0);
    puts("PASS: isolated 32 ms spikes rejected; a 64 ms bass hit responds");

    /* Blue-only low-edge guard: bin 1 (center 31.25 Hz) is skipped when its
       center sits below the configured 40 Hz edge. A sub-band tone leaks
       -6 dB into bin 2 through the Hann kernel, so choose a level whose
       bin-2 leakage stays below the -35 dBFS opening threshold while the
       same level at 125 Hz (in-band) must still open blue. */
    reset_fixture();
    warm_up_quiet();
    tone_hz(0.0355f, 31.25f); /* bin 1 at -32 dB; bin 2 leakage -38 dB */
    frames(8);
    assert(!s_bass_on && !duty[1] && s_blue_on_requests == 0);
    tone_hz(0.0355f, 125.0f); /* same level in-band opens blue */
    frames(4);
    assert(s_bass_on && duty[1] == CFG_MAX_BRIGHT && s_blue_on_requests == 1);
    quiet_noise();
    frames(1);
    assert(!s_bass_on && !duty[1]);
    puts("PASS: sub-band 31.25 Hz bin cannot gate blue; equal-level 125 Hz bass opens it");

    reset_fixture();
    warm_up_quiet();
    tones(rms_to_peak * powf(10, -38.0f / 20), 0, 0);
    frames(60);
    assert(!s_bass_on && !duty[1]);
    tones(rms_to_peak * powf(10, -34.0f / 20), 0, 0);
    frames(60);
    assert(s_bass_on && duty[1] > 0);
    tones(rms_to_peak * powf(10, -38.0f / 20), 0, 0);
    frames(1);
    assert(s_bass_on);
    tones(rms_to_peak * powf(10, -41.0f / 20), 0, 0);
    frames(1);
    assert(!s_bass_on && !duty[1]);
    puts("PASS: blue has its own -35/-40 dBFS hysteresis at a quiet baseline");

    reset_fixture();
    warm_up_quiet();
    float noise_before_music = s_bass_noise_db;
    tones(0.4f, 0, 0);
    frames(600);
    assert(duty[1] > 0 && s_bass_on && s_bass_noise_db == noise_before_music);
    puts("PASS: sustained bass remains active; music is not learned as noise");

    reset_fixture();
    s_reference_db = BASE_REFERENCE_DB;
    uint32_t previous = 0;
    for (int step = 0; step <= 100; step++) {
        float db = CFG_GATE_DB + (BASE_REFERENCE_DB - CFG_GATE_DB) * step / 100.0f;
        uint32_t next = map_brightness(db, CFG_GATE_DB, CFG_VOICE_SENS);
        assert(next >= previous && next <= CFG_MAX_BRIGHT);
        previous = next;
    }
    assert(previous == CFG_MAX_BRIGHT);
    float midpoint = (CFG_GATE_DB + BASE_REFERENCE_DB) / 2.0f;
    uint32_t old_mid = map_brightness(midpoint, CFG_GATE_DB, 100);
    uint32_t green_mid = map_brightness(midpoint, CFG_GATE_DB, CFG_VOICE_SENS);
    assert(green_mid < old_mid);
    printf("PASS: green retains 0..full graduation, mid-level old=%u green=%u\n",
           old_mid, green_mid);

    reset_fixture();
    warm_up_quiet();
    tones(0.45f, 0.45f, 0); /* Combined peak below 1.0; no ADC clipping. */
    frames(300);
    assert(s_reference_db > BASE_REFERENCE_DB && s_reference_db <= 0);
    assert(duty[0] > 0 && duty[0] < CFG_MAX_BRIGHT);
    assert(duty[1] == CFG_MAX_BRIGHT);
    float loud_reference = s_reference_db;
    uint32_t party_green = duty[0], party_blue = duty[1];
    tones(0.1f, 0.1f, 0);
    frames(30);
    assert(duty[0] < party_green && duty[1] == party_blue);
    assert(s_reference_db > BASE_REFERENCE_DB && s_reference_db < loud_reference);
    quiet_noise();
    frames(1);
    assert(!duty[0] && !duty[1]);
    float prior_reference = s_reference_db;
    frames(300);
    assert(s_reference_db >= BASE_REFERENCE_DB && s_reference_db < prior_reference);
    assert(!duty[0] && !duty[1]);
    printf("PASS: loud music normalizes green=%u while blue stays binary=%u; "
           "slow recovery never boosts silence\n", party_green, party_blue);
    read_error = true;
    frames(1);
    assert(s_reference_db == BASE_REFERENCE_DB && !s_bass_calibrated);
    assert(s_bass_cal_count == 0 && !duty[0] && !duty[1]);
    puts("PASS: capture failure clears adaptive reference and restarts background learning");

    reset_fixture();
    tones(0.5f, 0, 0);
    frames(BASS_CAL_FRAMES + 60);
    assert(!s_bass_on && !duty[1] && s_reference_db == BASE_REFERENCE_DB);
    tones(0.5f, 0.1f, 0);
    frames(60);
    assert(duty[0] > 0 && !duty[1] && s_reference_db == BASE_REFERENCE_DB);
    puts("PASS: rejected loud bass background cannot dim green via normalization");

    reset_fixture();
    tones(0.4f, 0, 0); /* Simulate music accidentally playing during calibration. */
    frames(BASS_CAL_FRAMES + 60);
    assert(!duty[1]);
    quiet_noise();
    frames(150);
    tones(0.1f, 0, 0);
    frames(60);
    /* With a locked minimum, recalibrate in quiet instead of self-weakening. */
#ifdef CFG_BLUE_FORCE_OFF
    assert(!duty[1]);
    reset_fixture();
    warm_up_quiet();
    tones(0.1f, 0, 0);
    frames(60);
    assert(duty[1] > 0);
    puts("PASS: startup threshold stays locked; a quiet reboot recalibrates blue");
#else
    assert(duty[1] > 0);
    puts("PASS: calibration during music recovers after a quiet interval");
#endif

#ifdef CFG_VOICE_ATTACK_MS
    reset_fixture();
    warm_up_quiet();
    tones(0, 0.1f, 0);
    frames(1);
    uint32_t first_syllable = duty[0];
    assert(first_syllable > 0 && !duty[1]);
    frames(59);
    uint32_t steady_voice = duty[0];
    assert(first_syllable > steady_voice * 7U / 10U);
    tones(0, 0.01f, 0); /* Quieter voice still above the closing threshold. */
    frames(3);
    assert(duty[0] > 0 && duty[0] < steady_voice * 4U / 10U);
    quiet_noise();
    frames(1);
    assert(!duty[0]);
    printf("PASS: sharper speech onset first block=%u, steady=%u; "
           "quieter syllable releases within 96 ms\n", first_syllable, steady_voice);

    for (int word = 0; word < 10; word++) {
        tones(0, rms_to_peak * powf(10, -46.0f / 20), 0);
        frames(1);
        assert(duty[0] > 0 && !duty[1]);
        quiet_noise();
        frames(1);
        assert(!duty[0] && !duty[1]);
    }
    puts("PASS: short speech-band syllables trigger green without hanging across pauses");

    reset_fixture();
    warm_up_quiet();
    const float hits[] = {0.04f, 0.1f, 0.45f, 0.8f};
    for (unsigned hit = 0; hit < sizeof(hits) / sizeof(hits[0]); hit++) {
        tones(hits[hit], 0, 0);
        frames(1);
        assert(!duty[1]);
        frames(1);
        assert(duty[1] == CFG_MAX_BRIGHT);
        frames(2);
        assert(duty[1] == CFG_MAX_BRIGHT);
        quiet_noise();
        frames(1);
        assert(!duty[1]);
    }
    puts("PASS: bass hits at multiple volumes switch 0 -> cap -> 0 with no graduation");

    reset_fixture();
    warm_up_quiet();
    tones(0, 0.1f, 0);
    frames(60);
    uint32_t voice_without_bass = duty[0];
    tones(0.8f, 0.1f, 0);
    frames(100);
    assert(duty[1] == CFG_MAX_BRIGHT && s_reference_db == BASE_REFERENCE_DB);
    assert(duty[0] >= voice_without_bass - 2 && duty[0] <= voice_without_bass + 2);
    puts("PASS: loud bass cannot dim green through its voice-only normalizer");
#ifdef CFG_BLUE_FORCE_OFF
    reset_fixture();
    tones(rms_to_peak * powf(10, -44.2f / 20), 0, 0);
    frames(BASS_CAL_FRAMES);
    float calibrated_gate = s_bass_gate_db;
    assert(fabsf(calibrated_gate - (-36.0f)) < 0.1f);
    tones(rms_to_peak * powf(10, -33.0f / 20), 0, 0);
    frames(2);
    assert(!duty[1]);
    tones(rms_to_peak * powf(10, -70.0f / 20), 0, 0);
    frames(8);
    tones(rms_to_peak * powf(10, -33.0f / 20), 0, 0);
    frames(2);
    assert(s_bass_gate_db >= calibrated_gate && !duty[1]);
    tones(0.1f, 0, 0);
    frames(2);
    assert(duty[1] == CFG_MAX_BRIGHT && s_blue_on_requests == 1);
    frames(60);
    assert(s_blue_on_requests == 1);
    quiet_noise();
    frames(1);
    assert(!duty[1]);
    printf("PASS: 256 ms quiet gap cannot lower calibrated gate %.1f; "
           "rejected background stays OFF, stronger bass ON\n", calibrated_gate);
    puts("PASS: rising-edge counter counts transitions, not status lines");
#endif
#endif
#endif

    assert(i2s_mic_init() == ESP_OK);
    assert(rx_config.slot_cfg.data_bit_width == 32);
    assert(rx_config.slot_cfg.slot_bit_width == 32);
    assert(rx_config.slot_cfg.slot_mode == I2S_SLOT_MODE_MONO);
    assert(rx_config.slot_cfg.slot_mask == I2S_STD_SLOT_LEFT);
    assert(rx_config.slot_cfg.ws_width == 32 && rx_config.slot_cfg.bit_shift);
    assert(rx_config.clk_cfg.sample_rate_hz == 16000);
    puts("PASS: 32-bit mono-left RX contract, 32-bit slots / 64 clocks per frame");

    assert(led_init() == ESP_OK);
    elapsed_ms = animation_events = 0;
    trace_animation = true;
    run_readiness_animation();
    trace_animation = false;
    assert(animation_events == 38 && elapsed_ms == 5400);
    assert(!duty[0] && !duty[1]);
    puts("PASS: original 18-flash animation, no overlap, 5.4 s, ends off");
    puts("All host regression checks passed (peripherals simulated).");
    return 0;
}
