# SonicSync32 (formerly BassVoice)

Frequency-reactive LED controller for the **ESP32-C6**. It reads music through
an **INMP441 I2S microphone** and drives two external LED channels:

| LED channel | Follows                    |
|-------------|----------------------------|
| **Green**   | Voice-range audio energy (300–3400 Hz) |
| **Blue**    | Bass energy (40–250 Hz)    |

This is frequency-reactive lighting, **not** speech recognition: the LEDs
simply brighten with the amount of energy in their band.

> **Migration note:** no BassVoice source code could be located on this
> machine, so SonicSync32 was written fresh (standard ESP32-C6 firmware,
> PlatformIO + ESP-IDF 6.0.1). There is no legacy source to preserve.

## Hardware wiring (ESP32-C6)

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

All pins are configurable via `menuconfig` (see below), but the table above
is the superseding pin assignment used by default.

I2S is configured as master, 24-bit Philips format, 32-bit slots
(64 BCLK per frame, the ratio the INMP441 expects), capturing the **left**
channel only.

## Power wiring (important)

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


## Behavior

### Startup (once per boot)

1. Both LED outputs are initialized **OFF** (pins driven low before PWM
   setup).
2. LED controls and the I2S microphone capture are initialized.
3. On **successful** initialization only, the readiness animation runs:
   - Green blinks 3× (blue stays off), then blue blinks 3× (green stays off).
   - The pattern repeats for **3 rounds**.
   - Timing: 150 ms ON / 150 ms OFF at reduced brightness
     (`SONICSYNC_ANIM_BRIGHTNESS`, 25 % by default). Only one channel is lit
     at a time.
4. Both LEDs turn OFF; audio capture is already active; the firmware waits
   for music.
5. When music is detected, audio-reactive lighting begins automatically.

The animation runs **exactly once per boot** — it is never looped while
waiting and never replayed when music starts or stops. If initialization
fails, both LEDs stay OFF and no ready indication is ever shown (the error
is logged to the serial console).

### Normal operation

- Each 32 ms (512 samples @ 16 kHz), the firmware FFTs the latest audio
  block and sums the energy in the bass and voice bands.
- **Noise gate:** levels below the gate (default −50 dB) keep the LED OFF —
  silence keeps both LEDs dark. Hysteresis prevents flicker.
- **Smoothing:** attack/release envelope filters make the light follow the
  music instead of strobing.
- **Brightness mapping:** gated level is normalized over a configurable span,
  raised to a gamma curve, scaled by sensitivity and capped at the maximum
  brightness.

## Configuration

All tuning values live in `src/Kconfig.projbuild`. Edit them with:

```
pio run -t menuconfig
```

then open `SonicSync32 Configuration`. Defaults:

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

Values can also be set permanently in `sdkconfig.defaults.esp32c6`.

### Tuning tips

- Set `Component config → Log output → Default log verbosity` to `Debug`
  (`pio run -t menuconfig`) to see per-second bass/voice levels and LED
  duties on the console — handy for setting the noise gate.
- If LEDs never light, lower the gate (more negative) and/or raise
  sensitivity. If they flicker on room noise, raise the gate.
- If bass reaction is weak, verify the audio actually contains bass; the
  band edges are configurable.

## Build, flash, monitor

PlatformIO + ESP-IDF (ESP32-C6, RISC-V) is used. Requirements: PlatformIO
with the `espressif32` platform and the ESP-IDF framework (already installed
on this machine).

> **Path note:** the ESP-IDF toolchain rejects project paths containing
> spaces, and this project lives in `C:\Project Aurora`. Use the included
> `pio.ps1` helper — it runs every PlatformIO command through the space-free
> junction `C:\ProjectAurora -> C:\Project Aurora` (created automatically on
> first use, no admin rights needed). The real project folder stays exactly
> where it is.

```powershell
cd "C:\Project Aurora\SonicSync32"

# Compile for ESP32-C6
.\pio.ps1 run

# Flash (adjust the COM port to the one found below)
.\pio.ps1 run -t upload --upload-port COM5

# Flash and watch the serial console
.\pio.ps1 run -t upload -t monitor --upload-port COM5

# Tune settings ("SonicSync32 Configuration" menu)
.\pio.ps1 run -t menuconfig
```

### Finding the board before flashing

Identify the COM port first — *only flash when the correct ESP32-C6 board is
connected*:

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

USB serial chips commonly seen with ESP32-C6 boards: **CP2102N**, **CH340**,
**CH343**, or the built-in **USB-Serial/JTAG** (two ports, flash via the
JTAG one). On Windows you can inspect the device manager under "Ports
(COM & LPT)" to read the chip name for each COM port.

To confirm the chip is an ESP32-C6, run `esptool` flash-id on the chosen
port (the chip is then identified in the output):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\esptool.py" --chip esp32c6 -p COMx flash_id
```

## Serial output on boot (expected)

```
I (…) SonicSync32: === SonicSync32 boot (ESP32-C6 + INMP441 I2S) ===
I (…) SonicSync32: LEDs: green=GPIO18 blue=GPIO19, PWM 1000 Hz, 12-bit
I (…) SonicSync32: I2S: BCLK=GPIO6 WS=GPIO7 SD=GPIO10, 16000 Hz, 24-bit, left channel only
I (…) SonicSync32: Initialization OK — running one-time readiness animation
I (…) SonicSync32: READY: audio capture active, waiting for music
I (…) SonicSync32: Music detected -> audio-reactive lighting active
```

## Project layout

```
SonicSync32/
├── platformio.ini              # board/toolchain configuration
├── CMakeLists.txt              # ESP-IDF project entry
├── sdkconfig.defaults.esp32c6  # board defaults merged into sdkconfig
├── README.md
└── src/
    ├── CMakeLists.txt          # component build (I2S + LEDC drivers)
    ├── Kconfig.projbuild       # all tunable options (menuconfig)
    └── main.c                  # firmware: init, readiness animation, DSP
```

