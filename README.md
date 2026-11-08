# SonicSync32

**ESP32-C6 audio-reactive LED controller** — listens to music through an
INMP441 I2S microphone and drives two LED channels with real-time FFT
frequency analysis.

| LED channel | Reacts to                     |
|-------------|-------------------------------|
| **Green**   | Voice-range energy (300–3400 Hz) |
| **Blue**    | Bass energy (40–250 Hz)       |

This is frequency-reactive lighting, not speech recognition: each channel's
brightness simply tracks how much audio energy sits in its frequency band, so
the light follows the music.

## Key features

- **ESP-IDF 6.0.1 on ESP32-C6** — RISC-V, 160 MHz single core; built with
  PlatformIO.
- **I2S master capture** — 24-bit Philips format, 32-bit slots (64 BCLK per
  frame, the ratio the INMP441 is designed around), left channel only,
  DMA-driven at 16 kHz.
- **Self-contained FFT DSP** — in-place radix-2 Cooley–Tukey FFT with Hann
  windowing in plain C; zero external DSP libraries.
- **Noise gate with hysteresis** — silence keeps both LEDs fully dark, and
  hysteresis stops flicker at the threshold.
- **Attack/release envelope smoothing** — the lights swell and fade musically
  instead of strobing.
- **Gamma-corrected brightness mapping** with sensitivity and max-brightness
  limits per channel.
- **Failsafe startup** — LED outputs forced off at reset; the one-time
  readiness animation runs only after successful initialization, so a failed
  init never produces a successful-ready indication.
- **Everything tunable via `menuconfig`** — pins, bands, gate, span,
  sensitivity, gamma, smoothing and brightness limits.

## How it works

### Audio pipeline

```
INMP441 ──I2S──▶ DMA ──▶ 512-sample blocks (32 ms @ 16 kHz)
   ──▶ DC removal + Hann window ──▶ 512-point FFT
   ──▶ bass & voice band energy ──▶ dB level
   ──▶ noise gate (+hysteresis) ──▶ attack/release envelope
   ──▶ gamma mapping ──▶ 12-bit LEDC PWM (green GPIO18, blue GPIO19)
```

**Capture.** The ESP32-C6 acts as I2S master and clocks the microphone
directly: 24-bit Philips format in 32-bit slots yields 64 BCLK ticks per
frame — the ratio the INMP441 expects — and the driver's slot mask keeps only
the left channel (the mic's L/R pin is tied to GND). Sampling at 16 kHz gives
a 0–8 kHz analysis range, comfortably covering both bands at a fraction of the
CPU and DMA load of 44.1 kHz. A DMA ring buffer decouples the peripheral from
processing.

**Frequency analysis.** Every 512 samples (32 ms, ≈ 31 frames/s) the firmware
removes the DC component, applies a Hann window to limit spectral leakage,
and runs an in-place radix-2 Cooley–Tukey FFT — roughly 5k butterfly
operations per frame, negligible for the C6. The bin width is 31.25 Hz, so
the default bands map to FFT bins 1–8 (bass, 40–250 Hz) and 10–109 (voice,
300–3400 Hz).

**Levels and gating.** Band energy (sum of squared magnitudes) is normalized
to a dB level. Each band has its own noise gate with hysteresis: the level
must exceed `gate + hysteresis` to switch on and fall below the gate to
switch off, so room noise cannot keep the lights glowing and borderline
levels cannot flicker them. Below the gate the envelope target is clamped to
silence, guaranteeing both LEDs are fully off in a quiet room.

**Smoothing and mapping.** The gated level feeds attack/release exponential
envelopes (60 ms up / 300 ms down by default). The smoothed level maps to a
12-bit LEDC duty cycle through `(level − gate) / span × sensitivity`, clamped
to [0, 1], raised to a gamma curve (1.8) and capped at the configured maximum
brightness — the same loudness-to-display mapping used in audio metering, so
perceived LED brightness tracks perceived loudness.

## Behavior

### Boot readiness animation (once per boot)

On every cold power-on or restart:

1. Both LED outputs initialize **OFF** (pins driven low before PWM setup).
2. LED controls and I2S microphone capture initialize.
3. Only after successful initialization, the readiness animation runs:
   - Green blinks 3× (blue stays off), then blue blinks 3× (green stays off).
   - The pattern repeats for **3 rounds**.
   - Timing: 150 ms ON / 150 ms OFF at reduced brightness (25 % of full
     scale by default). Only one channel is lit at a time.
4. Both LEDs turn OFF; audio capture is already running; the firmware waits
   for music and switches to audio-reactive lighting automatically when music
   is detected.

The animation runs **exactly once per boot** — it is never looped while
waiting and never replayed when music starts or stops. If initialization
fails, both LEDs stay OFF permanently and the failure is logged to the serial
console.

### Normal operation

- Every 32 ms the FFT block is processed and the green/blue LED duties are
  updated (~31 updates/s).
- **Silence keeps both LEDs off.** Noise-gated levels below the threshold
  always map to duty 0.
- Music drives each channel independently from its own band, smoothing and
  gate state, so bass drops and vocal passages are visible on their own
  channel.

## Hardware

### Wiring (ESP32-C6)

| Connection                | ESP32-C6 pin |
|---------------------------|--------------|
| INMP441 VDD/VCC           | 3V3          |
| INMP441 GND               | GND          |
| INMP441 SCK (BCLK)        | GPIO6        |
| INMP441 WS (LRCLK)        | GPIO7        |
| INMP441 SD (data → ESP32) | GPIO10       |
| INMP441 L/R               | GND (left channel) |
| Green LED driver control  | GPIO18       |
| Blue LED driver control   | GPIO19       |

All pins are configurable via `menuconfig`; the table lists the defaults.

### Power and LED driver requirements

- The **LED circuit is supplied from 12 V**; a buck converter supplies **5 V**
  to the ESP32 (e.g., to the 5V pin of the dev board).
- **All grounds are common** (12 V supply, LED drivers, ESP32, INMP441).
- The GPIO18/GPIO19 outputs go to the **LED driver inputs only** — they must
  not sink/source LED current directly.
- **Verify the installed LED driver board and its current limiting before
  full-power testing of the 20 W LEDs.** Start at reduced brightness
  (`SONICSYNC_MAX_BRIGHTNESS`) and check driver/heatsink temperatures.
- The INMP441 runs from 3V3. Its L/R pin goes to GND to select the left
  channel (matching the firmware's left-slot capture).

## Configuration

All tuning values live in `src/Kconfig.projbuild` and are edited at build
time with:

```
pio run -t menuconfig
```

under **SonicSync32 Configuration**. Defaults:

| Option                        | Default | Meaning |
|-------------------------------|---------|---------|
| Sample rate                   | 16000 Hz| I2S capture rate |
| I2S BCLK / WS / SD GPIOs      | 6 / 7 / 10 | Microphone pins |
| Green / Blue GPIOs            | 18 / 19 | LED driver pins |
| Bass band                     | 40–250 Hz | Blue channel band |
| Voice band                    | 300–3400 Hz | Green channel band |
| FFT size                      | 512     | Block length (power of two) |
| Noise gate                    | −50 dB  | Silence threshold |
| Gate hysteresis               | 3 dB    | Anti-flicker |
| Span (gate → full brightness) | 38 dB   | Dynamic range |
| Sensitivity                   | 100 %   | Brightness multiplier |
| Gamma                         | 180 %   | Perceptual curve exponent |
| Attack / release              | 60 / 300 ms | Envelope smoothing |
| Max brightness                | 4095    | 12-bit PWM full scale |
| Animation brightness          | 1023    | Readiness blink level |
| LED PWM frequency             | 1000 Hz | Driver PWM |

Persistent overrides can also go in `sdkconfig.defaults.esp32c6`.

### Tuning tips

- Set `Component config → Log output → Default log verbosity` to `Debug`
  (`pio run -t menuconfig`) to see per-second bass/voice levels and LED
  duties on the console — handy for setting the noise gate.
- If LEDs never light, lower the gate (more negative) and/or raise
  sensitivity. If they flicker on room noise, raise the gate.
- If bass reaction is weak, verify the audio actually contains bass; the
  band edges are configurable.

## Getting started

### Prerequisites

- **PlatformIO** — either the CLI (`pip install platformio`) or the
  PlatformIO IDE extension for VS Code. No other SDK installs are needed:
  the first build downloads the `espressif32` platform, ESP-IDF 6.0.1 and
  the RISC-V toolchain automatically.
- An ESP32-C6 board, an INMP441 microphone module, two LED drivers/LEDs,
  and the 12 V + buck-converter supply described under [Hardware](#hardware).

### Build

```bash
git clone https://github.com/Fjopet360/SonicSync32.git
cd SonicSync32
pio run
```

The firmware image is produced at `.pio/build/esp32-c6-devkitc-1/firmware.bin`.

> **Windows note — paths with spaces.** ESP-IDF builds fail when the project
> path contains whitespace. If your checkout lives in a spaced path (e.g.
> under `C:\Project Aurora`), use the bundled helper instead of `pio` — it
> runs the same commands through a space-free junction created
> automatically, no admin rights needed:
>
> ```powershell
> .\pio.ps1 run                    # build
> .\pio.ps1 run -t menuconfig      # tune settings
> .\pio.ps1 run -t upload -t monitor --upload-port COM5
> ```

### Flash and monitor

Identify the COM port first — *only flash when the correct ESP32-C6 board is
connected*:

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

USB serial chips commonly seen on ESP32-C6 boards: **CP2102N**, **CH340**,
**CH343**, or the built-in **USB-Serial/JTAG**. Confirm the chip with
esptool before flashing:

```powershell
pio pkg exec -p tool-esptoolpy esptool.py --chip esp32c6 -p COMx flash_id
```

Then flash and watch the console:

```bash
pio run -t upload --upload-port COMx
pio device monitor -p COMx -b 115200
```

## Verification status

### Observed on hardware (esptool + serial console)

| Check | Result |
|-------|--------|
| Firmware build (ESP32-C6, ESP-IDF 6.0.1) | **PASS** — `firmware.bin`, 210,928 bytes |
| Chip identification (`esptool flash_id`) | ESP32-C6 (QFN40) rev v0.2, 40 MHz crystal, 16 MB flash |
| Flash + verify | **PASS** — image hash verified by esptool |
| Cold boot: init → readiness animation → READY | **PASS** — confirmed in serial log |
| Readiness animation duration | 5.4 s measured from log timestamps (302 ms → 5712 ms; 3 rounds × 6 blinks × 300 ms) |
| Music-detection transition | **PASS** — "Music detected -> audio-reactive lighting active" logged |
| Silence → LEDs-off transition | Not yet observed on hardware |
| Space-free build via `pio.ps1` junction | **PASS** — no whitespace error |

### Awaiting physical confirmation

| Check | Status |
|-------|--------|
| Readiness blink pattern seen on the LEDs | Pending — PWM duty activity verified via serial logs only |
| LED brightness visually following music | Pending |
| Full-power 20 W LED run at 12 V | Pending — LED driver board and current limiting to be verified first |

## Serial output on boot (observed)

```
I (262) SonicSync32: === SonicSync32 boot (ESP32-C6 + INMP441 I2S) ===
I (272) SonicSync32: Gate -50 dB (+3 dB hysteresis), span 38 dB, sensitivity 100%, gamma 1.80, max brightness 4095
I (282) SonicSync32: FFT block 512 samples, sample rate 16000 Hz
I (282) SonicSync32: LEDs: green=GPIO18 blue=GPIO19, PWM 1000 Hz, 12-bit
I (292) SonicSync32: I2S: BCLK=GPIO6 WS=GPIO7 SD=GPIO10, 16000 Hz, 24-bit, left channel only
I (302) SonicSync32: Initialization OK — running one-time readiness animation
I (302) SonicSync32: Readiness animation: green x3, blue x3, 3 rounds, 150 ms on/off, brightness 1023
I (5712) SonicSync32: Bands: bass 40-250 Hz (bins 1-8), voice 300-3400 Hz (bins 10-109)
I (5712) SonicSync32: Envelope: attack 60 ms, release 300 ms (coefs 0.413/0.101)
I (5722) SonicSync32: READY: audio capture active, waiting for music
I (5742) SonicSync32: Music detected -> audio-reactive lighting active
```

## Project layout

```
SonicSync32/
├── platformio.ini              # board / toolchain / flash configuration
├── CMakeLists.txt              # ESP-IDF project entry point
├── sdkconfig.defaults          # common sdkconfig defaults
├── sdkconfig.defaults.esp32c6  # ESP32-C6 board defaults
├── pio.ps1                     # PowerShell helper for spaced checkout paths
├── README.md
└── src/
    ├── CMakeLists.txt          # component build (I2S + LEDC drivers)
    ├── Kconfig.projbuild       # tunable options exposed via menuconfig
    └── main.c                  # firmware: init, readiness animation, DSP pipeline
```
