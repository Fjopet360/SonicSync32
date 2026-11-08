/* Compile the real firmware against host-only peripheral stubs. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

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
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
#define ESP_LOGD(tag, fmt, ...) ((void)(tag))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag))
#define ESP_LOGE(tag, fmt, ...) ((void)(tag))

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
    s_env_bass_db = s_env_voice_db = -120.0f;
    s_block_count = 0;
    duty[0] = duty[1] = 0;
    read_error = read_empty = false;
    read_chunk = 0;
    memset(input_words, 0, sizeof(input_words));
    window_init();
    band_bins_init();
    envelope_init();
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

static void frames(unsigned count)
{
    while (count--) {
        input_pos = 0;
        process_audio_block();
        assert(duty[0] <= CFG_MAX_BRIGHT && duty[1] <= CFG_MAX_BRIGHT);
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

int main(int argc, char **argv)
{
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
    tones(0.1f, 0, 0);
    frames(60);
    float bass_db = energy_to_db(band_energy(s_bin_bass_lo, s_bin_bass_hi));
    assert(fabsf(bass_db - (-23.0103f)) < 0.1f);
    assert(!duty[0] && duty[1] > 0 && duty[1] < CFG_MAX_BRIGHT);
    uint32_t loud_blue = duty[1];
    printf("PASS: 125 Hz tone -> blue=%u green=0, level=%.2f dBFS\n",
           duty[1], bass_db);

    reset_fixture();
    tones(0.01f, 0, 0);
    frames(60);
    assert(duty[1] > 0 && duty[1] < loud_blue && !duty[0]);
    puts("PASS: quieter bass gives lower brightness");

    reset_fixture();
    tones(0, 0.1f, 0);
    read_chunk = 68; /* Exercise the real partial-read accumulation loop. */
    frames(60);
    assert(duty[0] > 0 && duty[0] < CFG_MAX_BRIGHT && !duty[1]);
    printf("PASS: 1000 Hz tone -> green=%u blue=0, including partial reads\n", duty[0]);

    reset_fixture();
    tones(0.1f, 0.1f, 0);
    frames(60);
    assert(duty[0] > 0 && duty[1] > 0);
    quiet_noise();
    frames(1);
    assert(!duty[0] && !duty[1] && !s_bass_on && !s_voice_on);
    puts("PASS: mixed audio -> both channels, next quiet block -> both off");

    reset_fixture();
    const float rms_to_peak = 1.41421356237f;
    tones(rms_to_peak * powf(10, -48.0f / 20), 0, 0);
    frames(60);
    assert(!s_bass_on && !duty[1]);
    tones(rms_to_peak * powf(10, -46.0f / 20), 0, 0);
    frames(60);
    assert(s_bass_on && duty[1] > 0);
    tones(rms_to_peak * powf(10, -48.0f / 20), 0, 0);
    frames(1);
    assert(s_bass_on);
    tones(rms_to_peak * powf(10, -51.0f / 20), 0, 0);
    frames(1);
    assert(!s_bass_on && !duty[1]);
    puts("PASS: -47/-50 dBFS opening/closing hysteresis");

    tones(0.1f, 0.1f, 0);
    frames(60);
    read_error = true;
    frames(1);
    assert(!duty[0] && !duty[1] && !s_bass_on && !s_voice_on && !s_music_on);
    assert(s_env_bass_db == -120.0f && s_env_voice_db == -120.0f);
    assert(last_timeout_ms < 1000 && last_timeout_ms >= 32);
    read_error = false;
    quiet_noise();
    frames(1);
    assert(!duty[0] && !duty[1]);
    tones(0.1f, 0.1f, 0);
    frames(60);
    read_empty = true;
    frames(1);
    assert(!duty[0] && !duty[1] && !s_music_on);
    puts("PASS: timeout/empty capture clears PWM, gates and envelopes");

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
