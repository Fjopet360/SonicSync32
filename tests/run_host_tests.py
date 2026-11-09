#!/usr/bin/env python3
"""Run the real main.c with fake I2S/LEDC peripherals and synthetic audio.

Requires Python 3 and a C11 compiler (CC, cc, gcc or clang). No ESP-IDF SDK
or board is needed. This complements, and does not replace, an ESP32 build
and physical microphone/LED tests.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import wave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="alternate main.c for regression reproduction")
    parser.add_argument("--reproduce", action="store_true", help="expect the original stuck-on bug")
    parser.add_argument("--reproduce-ambient", action="store_true", help="expect pre-tuning hum response")
    parser.add_argument("--define", action="append", default=[], metavar="CONFIG_NAME=VALUE",
                        help="override a Kconfig default for a configuration check")
    parser.add_argument("--config-smoke", action="store_true", help="run a short configuration check")
    parser.add_argument("--speech-probe", action="store_true", help="print speech-envelope timing response")
    parser.add_argument("--green-trace", action="store_true", help="emit deterministic green-duty trace")
    parser.add_argument("--blue-off-smoke", action="store_true", help="check blue-only diagnostic override")
    parser.add_argument("--voice-fixture", action="store_true", help="test real VAD with upstream speech WAV")
    parser.add_argument("--filter-smoke", action="store_true", help="test speech-filter tone rejection and error handling")
    parser.add_argument("--blue-soft-probe", action="store_true", help="test rejected music cannot ratchet blue's gate upward")
    parser.add_argument("--reproduce-blue-soft", action="store_true", help="expect the c436c89 soft-bass noise-learning defect")
    parser.add_argument("--replay-wav", type=Path, help="run a mono PCM16 WAV through production detectors; emit CSV")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = shlex.split(os.environ["CC"]) if "CC" in os.environ else [
        next((name for name in ("cc", "gcc", "clang") if shutil.which(name)), "cc")]
    if not shutil.which(compiler[0]):
        parser.error("install a C11 compiler or set CC")
    config = (root / "src/Kconfig.projbuild").read_text()
    defaults = re.findall(r"\bconfig (\w+)\s+.*?\bdefault (-?\d+)", config, re.S)
    values = {f"CONFIG_{name}": value for name, value in defaults}
    # Existing pure-tone tests exercise FFT/gates/brightness, not speech identity.
    # A separate run below uses the real classifier and a speech WAV.
    if not (args.voice_fixture or args.replay_wav or args.filter_smoke):
        values["CONFIG_SONICSYNC_VOICE_FILTER_ENABLE"] = "0"
    for override in args.define:
        if not re.fullmatch(r"CONFIG_\w+=-?\d+", override):
            parser.error("--define expects CONFIG_NAME=integer")
        name, value = override.split("=", 1)
        if name not in values:
            parser.error(f"unknown Kconfig name: {name}")
        values[name] = value
    defines = [f"-D{name}={value}" for name, value in values.items()]
    if args.source:
        defines.append(f'-DSONICSYNC_SOURCE="{args.source.resolve().as_posix()}"')
    with tempfile.TemporaryDirectory(prefix="sonicsync-host-") as folder:
        temp = Path(folder)
        # Types and stub implementations live in host_audio_test.c. Empty
        # stand-in includes let it include the unmodified production source.
        for name in ("freertos/FreeRTOS.h", "freertos/task.h", "esp_err.h",
                     "esp_log.h", "driver/gpio.h", "driver/ledc.h", "driver/i2s_std.h"):
            header = temp / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text("/* Host test: declarations are supplied by the harness. */\n")
        binary = temp / ("audio_test.exe" if os.name == "nt" else "audio_test")
        sanitizers = [] if os.name == "nt" else ["-fsanitize=undefined", "-fno-sanitize-recover=all"]
        math_link = [] if os.name == "nt" else ["-lm"]
        vad_dir = root / "components/voice_vad"
        vad_sources = sorted((vad_dir / "vendor/src").rglob("*.c"))
        vad_sources.append(vad_dir / "voice_filter.c")
        subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-Wno-unused-function", "-O1", "-g",
                       f"-I{temp}", f"-I{vad_dir}"] + sanitizers + defines +
                       [str(root / "tests/host_audio_test.c")] +
                       [str(p) for p in vad_sources] + math_link + ["-o", str(binary)],
                       check=True)
        if args.voice_fixture or args.replay_wav:
            wav_path = args.replay_wav or root / "tests/data/upstream-speech-16k.wav"
            with wave.open(str(wav_path), "rb") as wav:
                if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, int(values["CONFIG_SONICSYNC_SAMPLE_RATE"])):
                    parser.error("WAV must be mono PCM16 at the configured sample rate")
                raw_path = temp / "input.pcm"
                raw_path.write_bytes(wav.readframes(wav.getnframes()))
            subprocess.run([str(binary), "--voice-fixture" if args.voice_fixture else
                            "--replay-pcm", str(raw_path)], check=True)
            return
        mode = (["--blue-soft-probe"] if args.blue_soft_probe else ["--reproduce-blue-soft"] if args.reproduce_blue_soft else ["--filter-smoke"] if args.filter_smoke else ["--green-trace"] if args.green_trace else ["--blue-off-smoke"] if args.blue_off_smoke else
                ["--speech-probe"] if args.speech_probe else ["--config-smoke"] if args.config_smoke else ["--reproduce"] if args.reproduce else
                ["--reproduce-ambient"] if args.reproduce_ambient else [])
        subprocess.run([str(binary)] + mode, check=True)
        if not mode and not args.source:
            subprocess.run([str(binary), "--blue-soft-probe"], check=True)


if __name__ == "__main__":
    main()
