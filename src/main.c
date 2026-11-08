/*
 * SonicSync32 (formerly BassVoice)
 * =================================
 * Frequency-reactive LED controller for ESP32-C6.
 *
 * Reads music through an INMP441 I2S microphone (left channel) and drives
 * two external LED channels:
 *   - GREEN: brightness follows voice-range audio energy (300-3400 Hz)
 *   - BLUE : brightness follows bass energy (40-250 Hz)
 *
 * This is frequency-reactive lighting, not speech recognition.
 *
 * Wiring (ESP32-C6):
 *   INMP441 VDD/VCC -> 3V3     INMP441 GND -> GND
 *   INMP441 SCK     -> GPIO6   (BCLK)
 *   INMP441 WS      -> GPIO7   (LRCLK)
 *   INMP441 SD      -> GPIO10  (mic data into ESP32)
 *   INMP441 L/R     -> GND     (left channel)
 *   Green LED driver -> GPIO18   Blue LED driver -> GPIO19
 *
 * Boot sequence (once per power-on/restart):
 *   1. Both LED outputs OFF.
 *   2. LED + I2S initialization.
 *   3. Readiness animation (only on successful init):
 *      green blinks 3x (blue off), blue blinks 3x (green off), 3 rounds,
 *      150 ms on / 150 ms off at reduced brightness. One channel lit at a time.
 *   4. Both LEDs OFF, audio capture active, waiting for music.
 *   Initialization failure: LEDs stay OFF, no ready indication, ever.
 *
 * Normal operation: silence keeps both LEDs OFF; music drives the LEDs
 * through band energy -> noise gate -> attack/release smoothing ->
 * gamma brightness mapping. All bands, sensitivity, smoothing and
 * brightness limits are configurable (see src/Kconfig.projbuild).
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2s_std.h"

static const char *TAG = "SonicSync32";

#define FFT_PI 3.14159265358979f

/* ------------------------------------------------------------------------- */
/* Configuration (all values come from "SonicSync32 Configuration" Kconfig)  */
/* ------------------------------------------------------------------------- */
#define CFG_SAMPLE_RATE   CONFIG_SONICSYNC_SAMPLE_RATE
#define CFG_BCLK_GPIO     CONFIG_SONICSYNC_I2S_BCLK_GPIO
#define CFG_WS_GPIO       CONFIG_SONICSYNC_I2S_WS_GPIO
#define CFG_DATA_GPIO     CONFIG_SONICSYNC_I2S_DATA_GPIO
#define CFG_GREEN_GPIO    CONFIG_SONICSYNC_LED_GREEN_GPIO
#define CFG_BLUE_GPIO     CONFIG_SONICSYNC_LED_BLUE_GPIO
#define CFG_BASS_LO_HZ    CONFIG_SONICSYNC_BASS_LOW_HZ
#define CFG_BASS_HI_HZ    CONFIG_SONICSYNC_BASS_HIGH_HZ
#define CFG_VOICE_LO_HZ   CONFIG_SONICSYNC_VOICE_LOW_HZ
#define CFG_VOICE_HI_HZ   CONFIG_SONICSYNC_VOICE_HIGH_HZ
#define CFG_FFT_N         CONFIG_SONICSYNC_FFT_SIZE
#define CFG_GATE_DB       CONFIG_SONICSYNC_NOISE_GATE_DB
#define CFG_HYST_DB       CONFIG_SONICSYNC_GATE_HYSTERESIS_DB
#define CFG_SPAN_DB       CONFIG_SONICSYNC_FULL_SCALE_DB
#define CFG_SENSITIVITY   CONFIG_SONICSYNC_SENSITIVITY
#define CFG_GAMMA         CONFIG_SONICSYNC_GAMMA
#define CFG_ATTACK_MS     CONFIG_SONICSYNC_ATTACK_MS
#define CFG_RELEASE_MS    CONFIG_SONICSYNC_RELEASE_MS
#define CFG_MAX_BRIGHT    CONFIG_SONICSYNC_MAX_BRIGHTNESS
#define CFG_ANIM_BRIGHT   CONFIG_SONICSYNC_ANIM_BRIGHTNESS
#define CFG_PWM_FREQ_HZ   CONFIG_SONICSYNC_LED_PWM_FREQ

/* 12-bit LEDC resolution: duty range 0..4095 */
#define LED_DUTY_MAX 4095

/* Three audio blocks plus scheduling margin; i2s_channel_read takes ms. */
#define AUDIO_READ_TIMEOUT_MS \
    (3U * ((1000U * CFG_FFT_N + CFG_SAMPLE_RATE - 1U) / CFG_SAMPLE_RATE) + 50U)

/* Signed 24-bit PCM occupies the high 24 bits of each 32-bit RX word. */
#define PCM24_FULL_SCALE 8388608.0f

/* ------------------------------------------------------------------------- */
/* LED channels                                                              */
/* ------------------------------------------------------------------------- */
typedef enum {
    LED_GREEN = 0,
    LED_BLUE  = 1,
} led_t;

static void led_set(led_t led, uint32_t duty)
{
    if (duty > LED_DUTY_MAX) {
        duty = LED_DUTY_MAX;
    }
    ledc_channel_t ch = (led == LED_GREEN) ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

/*
 * Initialize both LED outputs OFF, then the PWM hardware.
 * The GPIOs are first driven low explicitly so the LED drivers
 * can never see a floating/high level during reset or boot.
 */
static esp_err_t led_init(void)
{
    gpio_set_direction(CFG_GREEN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(CFG_BLUE_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CFG_GREEN_GPIO, 0);
    gpio_set_level(CFG_BLUE_GPIO, 0);

    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_12_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = CFG_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
        .deconfigure     = false,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t green = {
        .gpio_num            = CFG_GREEN_GPIO,
        .speed_mode          = LEDC_LOW_SPEED_MODE,
        .channel             = LEDC_CHANNEL_0,
        .timer_sel           = LEDC_TIMER_0,
        .duty                = 0,
        .hpoint              = 0,
        .sleep_mode          = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags.output_invert = 0,
        .deconfigure         = false,
    };
    err = ledc_channel_config(&green);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "green ledc_channel_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t blue = green;
    blue.gpio_num = CFG_BLUE_GPIO;
    blue.channel  = LEDC_CHANNEL_1;
    err = ledc_channel_config(&blue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "blue ledc_channel_config failed: %s", esp_err_to_name(err));
        return err;
    }

    led_set(LED_GREEN, 0);
    led_set(LED_BLUE, 0);
    ESP_LOGI(TAG, "LEDs: green=GPIO%d blue=GPIO%d, PWM %d Hz, 12-bit",
             CFG_GREEN_GPIO, CFG_BLUE_GPIO, CFG_PWM_FREQ_HZ);
    return ESP_OK;
}


/* ------------------------------------------------------------------------- */
/* Readiness animation (runs exactly once per boot, after successful init)   */
/* ------------------------------------------------------------------------- */
static void anim_blink(led_t led, int times)
{
    for (int i = 0; i < times; i++) {
        led_set(led, CFG_ANIM_BRIGHT);
        vTaskDelay(pdMS_TO_TICKS(150));
        led_set(led, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void run_readiness_animation(void)
{
    ESP_LOGI(TAG, "Readiness animation: green x3, blue x3, 3 rounds, "
             "150 ms on/off, brightness %lu",
             (unsigned long)CFG_ANIM_BRIGHT);
    for (int round = 1; round <= 3; round++) {
        ESP_LOGD(TAG, "readiness round %d/3", round);
        anim_blink(LED_GREEN, 3);   /* blue stays off */
        anim_blink(LED_BLUE, 3);    /* green stays off */
    }
    led_set(LED_GREEN, 0);
    led_set(LED_BLUE, 0);
}

/* ------------------------------------------------------------------------- */
/* DSP state                                                                 */
/* ------------------------------------------------------------------------- */
static i2s_chan_handle_t s_rx_chan = NULL;

static int32_t s_raw[CFG_FFT_N];   /* 24-bit I2S samples (in 32-bit words) */
static float   s_re[CFG_FFT_N];
static float   s_im[CFG_FFT_N];
static float   s_win[CFG_FFT_N];
static float   s_window_power;

static float s_env_bass_db  = -120.0f;   /* smoothed level envelopes (dB) */
static float s_env_voice_db = -120.0f;
static bool  s_bass_on      = false;    /* noise gate states */
static bool  s_voice_on     = false;
static bool  s_music_on     = false;    /* music/silence state (logging) */

static float s_attack_coef;             /* per-block smoothing coefficients */
static float s_release_coef;

static int s_bin_bass_lo, s_bin_bass_hi;    /* FFT bins per band */
static int s_bin_voice_lo, s_bin_voice_hi;

static uint32_t s_block_count;          /* for periodic debug logging */

/* ------------------------------------------------------------------------- */
/* I2S microphone (INMP441, left channel, master clocks)                     */
/* ------------------------------------------------------------------------- */
static esp_err_t i2s_mic_init(void)
{
    ESP_LOGI(TAG, "I2S: BCLK=GPIO%d WS=GPIO%d SD=GPIO%d, %d Hz, "
             "32-bit RX words (24-bit mic), mono left",
             CFG_BCLK_GPIO, CFG_WS_GPIO, CFG_DATA_GPIO, CFG_SAMPLE_RATE);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                            I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = CFG_FFT_N;
    chan_cfg.dma_desc_num  = 6;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Two 32-bit slots on the wire give 64 BCLK ticks per frame, which is
     * the ratio the INMP441 expects. On the C6, use MONO + LEFT to discard
     * the unused right slot; stereo RX includes both slots regardless of
     * slot_mask. Receive 32 bits so DMA matches int32_t: 24-bit RX on the
     * C6 is packed into 3-byte samples, not left-aligned int32_t words.
     * The mic's signed 24-bit payload is in bits 31:8; discard bits 7:0.
     */
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz  = CFG_SAMPLE_RATE,
            .clk_src         = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple   = I2S_MCLK_MULTIPLE_384,
            .bclk_div        = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,
            .slot_mode      = I2S_SLOT_MODE_MONO,
            .slot_mask      = I2S_STD_SLOT_LEFT,
            .ws_width       = 32,
            .ws_pol         = false,
            .bit_shift      = true,
            .left_align     = true,
            .big_endian     = false,
            .bit_order_lsb  = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CFG_BCLK_GPIO,
            .ws   = CFG_WS_GPIO,
            .din  = CFG_DATA_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* Read exactly one FFT block of samples (blocks until the mic delivers). */
static int read_audio_block(void)
{
    size_t total = 0;
    while (total < sizeof(s_raw)) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan,
                                         (uint8_t *)s_raw + total,
                                         sizeof(s_raw) - total,
                                         &got, AUDIO_READ_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
            return -1;
        }
        if (got == 0) {
            return -1;
        }
        total += got;
    }
    return 0;
}



/* ------------------------------------------------------------------------- */
/* DSP: window, FFT, band energy, gate, envelope, brightness mapping         */
/* ------------------------------------------------------------------------- */

/* Iterative in-place radix-2 complex FFT (Cooley-Tukey). */
static void fft_c(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * FFT_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k;
                int b = i + k + len / 2;
                float vr = re[b] * cr - im[b] * ci;
                float vi = re[b] * ci + im[b] * cr;
                float ur = re[a], ui = im[a];
                re[a] = ur + vr; im[a] = ui + vi;
                re[b] = ur - vr; im[b] = ui - vi;
                float nr = cr * wr - ci * wi;
                ci = ci * wr + cr * wi;
                cr = nr;
            }
        }
    }
}

static void window_init(void)
{
    s_window_power = 0.0f;
    for (int i = 0; i < CFG_FFT_N; i++) {
        s_win[i] = 0.5f * (1.0f - cosf(2.0f * FFT_PI * (float)i /
                                       (float)(CFG_FFT_N - 1)));
        s_window_power += s_win[i] * s_win[i];
    }
}

/* Clamp an FFT bin index into [1, N/2-1] (bin 0 is DC). */
static int clamp_bin(int bin)
{
    if (bin < 1) {
        bin = 1;
    }
    if (bin > CFG_FFT_N / 2 - 1) {
        bin = CFG_FFT_N / 2 - 1;
    }
    return bin;
}

static void band_bins_init(void)
{
    s_bin_bass_lo  = clamp_bin((int)lroundf((float)CFG_BASS_LO_HZ *
                                           (float)CFG_FFT_N /
                                           (float)CFG_SAMPLE_RATE));
    s_bin_bass_hi  = clamp_bin((int)lroundf((float)CFG_BASS_HI_HZ *
                                           (float)CFG_FFT_N /
                                           (float)CFG_SAMPLE_RATE));
    s_bin_voice_lo = clamp_bin((int)lroundf((float)CFG_VOICE_LO_HZ *
                                           (float)CFG_FFT_N /
                                           (float)CFG_SAMPLE_RATE));
    s_bin_voice_hi = clamp_bin((int)lroundf((float)CFG_VOICE_HI_HZ *
                                           (float)CFG_FFT_N /
                                           (float)CFG_SAMPLE_RATE));

    if (s_bin_bass_lo > s_bin_bass_hi) {
        ESP_LOGW(TAG, "bass band collapses to one bin (check band edges)");
        s_bin_bass_hi = s_bin_bass_lo;
    }
    if (s_bin_voice_lo > s_bin_voice_hi) {
        ESP_LOGW(TAG, "voice band collapses to one bin (check band edges)");
        s_bin_voice_hi = s_bin_voice_lo;
    }

    ESP_LOGI(TAG, "Bands: bass %d-%d Hz (bins %d-%d), voice %d-%d Hz "
             "(bins %d-%d)",
             CFG_BASS_LO_HZ, CFG_BASS_HI_HZ, s_bin_bass_lo, s_bin_bass_hi,
             CFG_VOICE_LO_HZ, CFG_VOICE_HI_HZ, s_bin_voice_lo, s_bin_voice_hi);
}

static void envelope_init(void)
{
    float dt = (float)CFG_FFT_N / (float)CFG_SAMPLE_RATE;
    s_attack_coef  = 1.0f - expf(-dt / ((float)CFG_ATTACK_MS / 1000.0f));
    s_release_coef = 1.0f - expf(-dt / ((float)CFG_RELEASE_MS / 1000.0f));
    ESP_LOGI(TAG, "Envelope: attack %d ms, release %d ms (coefs %.3f/%.3f)",
             CFG_ATTACK_MS, CFG_RELEASE_MS, (double)s_attack_coef,
             (double)s_release_coef);
}

static float band_energy(int lo, int hi)
{
    float e = 0.0f;
    for (int b = lo; b <= hi; b++) {
        e += s_re[b] * s_re[b] + s_im[b] * s_im[b];
    }
    return e;
}

/* One-sided band RMS power in dBFS, correcting for FFT and Hann gain. */
static float energy_to_db(float energy)
{
    float norm = 2.0f * energy / ((float)CFG_FFT_N * s_window_power);
    if (norm < 1e-12f) {
        norm = 1e-12f;
    }
    return 10.0f * log10f(norm);
}

/* Noise gate with hysteresis (per band, sticky state). */
static bool gate_pass(float db, bool *state)
{
    if (*state) {
        if (db < (float)CFG_GATE_DB) {
            *state = false;
        }
    } else {
        if (db > (float)CFG_GATE_DB + (float)CFG_HYST_DB) {
            *state = true;
        }
    }
    return *state;
}

static void env_update(float *env, float target)
{
    float coef = (target > *env) ? s_attack_coef : s_release_coef;
    *env += coef * (target - *env);
}

/* Envelope level -> PWM duty (0 when below gate, capped at max brightness). */
static uint32_t map_brightness(float env_db)
{
    float frac = (env_db - (float)CFG_GATE_DB) / (float)CFG_SPAN_DB;
    frac *= (float)CFG_SENSITIVITY / 100.0f;
    if (frac <= 0.0f) {
        return 0;
    }
    if (frac > 1.0f) {
        frac = 1.0f;
    }
    frac = powf(frac, (float)CFG_GAMMA / 100.0f);
    return (uint32_t)(frac * (float)CFG_MAX_BRIGHT + 0.5f);
}

/* ------------------------------------------------------------------------- */
/* Normal audio-reactive processing                                          */
/* ------------------------------------------------------------------------- */
static void reset_audio_state(void)
{
    s_bass_on = false;
    s_voice_on = false;
    s_music_on = false;
    s_env_bass_db = -120.0f;
    s_env_voice_db = -120.0f;
    led_set(LED_GREEN, 0);
    led_set(LED_BLUE, 0);
}

static void process_audio_block(void)
{
    if (read_audio_block() != 0) {
        /* No stale gate/envelope or PWM may survive a capture failure. */
        reset_audio_state();
        vTaskDelay(pdMS_TO_TICKS(50));
        return;
    }

    /*
     * Normalize signed PCM to [-1, 1) BEFORE measuring energy in dBFS.
     * Raw 24-bit counts inflate the old dB levels by about 138 dB, opening
     * both noise gates and pinning both PWM outputs even on quiet input.
     */
    float mean = 0.0f;
    for (int i = 0; i < CFG_FFT_N; i++) {
        s_re[i] = (float)(s_raw[i] >> 8) / PCM24_FULL_SCALE;
        mean += s_re[i];
    }
    mean /= (float)CFG_FFT_N;
    for (int i = 0; i < CFG_FFT_N; i++) {
        s_re[i] = (s_re[i] - mean) * s_win[i];
        s_im[i] = 0.0f;
    }
    fft_c(s_re, s_im, CFG_FFT_N);

    float e_bass  = band_energy(s_bin_bass_lo, s_bin_bass_hi);
    float e_voice = band_energy(s_bin_voice_lo, s_bin_voice_hi);

    float db_bass  = energy_to_db(e_bass);
    float db_voice = energy_to_db(e_voice);

    s_bass_on  = gate_pass(db_bass, &s_bass_on);
    s_voice_on = gate_pass(db_voice, &s_voice_on);

    bool music = s_bass_on || s_voice_on;
    if (music && !s_music_on) {
        ESP_LOGI(TAG, "Music detected -> audio-reactive lighting active");
    } else if (!music && s_music_on) {
        ESP_LOGI(TAG, "Silence -> LEDs off");
    }
    s_music_on = music;

    env_update(&s_env_bass_db,  s_bass_on  ? db_bass  : -120.0f);
    env_update(&s_env_voice_db, s_voice_on ? db_voice : -120.0f);

    /* A closed gate overrides smoothing immediately, including after music. */
    uint32_t green = s_voice_on ? map_brightness(s_env_voice_db) : 0;
    uint32_t blue  = s_bass_on  ? map_brightness(s_env_bass_db)  : 0;
    led_set(LED_GREEN, green);
    led_set(LED_BLUE, blue);

    s_block_count++;
    if (s_block_count % (CFG_SAMPLE_RATE / CFG_FFT_N) == 0) {
        ESP_LOGI(TAG, "bass %6.1f dBFS %s | voice %6.1f dBFS %s | "
                 "green %4lu blue %4lu",
                 (double)db_bass, s_bass_on ? "on" : "off",
                 (double)db_voice, s_voice_on ? "on" : "off",
                 (unsigned long)green, (unsigned long)blue);
    }
}

/* ------------------------------------------------------------------------- */
/* Boot                                                                      */
/* ------------------------------------------------------------------------- */
static void log_config(void)
{
    ESP_LOGI(TAG, "Gate %d dBFS (+%d dB hysteresis), span %d dB, "
             "sensitivity %d%%, gamma %.2f, max brightness %lu",
             CFG_GATE_DB, CFG_HYST_DB, CFG_SPAN_DB,
             CFG_SENSITIVITY, (double)CFG_GAMMA / 100.0,
             (unsigned long)CFG_MAX_BRIGHT);
    ESP_LOGI(TAG, "FFT block %d samples, sample rate %d Hz",
             CFG_FFT_N, CFG_SAMPLE_RATE);
}

static void idle_forever(void)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== SonicSync32 boot (ESP32-C6 + INMP441 I2S) ===");
    log_config();

    if (CFG_FFT_N < 8 || (CFG_FFT_N & (CFG_FFT_N - 1)) != 0) {
        ESP_LOGE(TAG, "FFT size %d is not a power of two >= 8 — aborting, "
                 "LEDs stay OFF", CFG_FFT_N);
        idle_forever();
        return;
    }

    /*
     * 1. LED outputs OFF (led_init drives both pins low first).
     * 2. Microphone / I2S capture / LED controls.
     * 3. Readiness animation only when everything initialized.
     * On any failure: no ready indication, LEDs stay off forever.
     */
    esp_err_t err = led_init();
    if (err == ESP_OK) {
        err = i2s_mic_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initialization FAILED: %s — readiness animation "
                 "suppressed, LEDs remain OFF", esp_err_to_name(err));
        led_set(LED_GREEN, 0);
        led_set(LED_BLUE, 0);
        idle_forever();
        return;
    }

    ESP_LOGI(TAG, "Initialization OK — running one-time readiness animation");
    run_readiness_animation();

    window_init();
    band_bins_init();
    envelope_init();
    ESP_LOGI(TAG, "READY: audio capture active, waiting for music");

    for (;;) {
        process_audio_block();
    }
}

