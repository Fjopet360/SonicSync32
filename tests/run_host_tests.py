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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="alternate main.c for regression reproduction")
    parser.add_argument("--reproduce", action="store_true", help="expect the original stuck-on bug")
    parser.add_argument("--reproduce-ambient", action="store_true", help="expect pre-tuning hum response")
    parser.add_argument("--define", action="append", default=[], metavar="CONFIG_NAME=VALUE",
                        help="override a Kconfig default for a configuration check")
    parser.add_argument("--config-smoke", action="store_true", help="run a short configuration check")
    parser.add_argument("--speech-probe", action="store_true", help="print speech-envelope timing response")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = shlex.split(os.environ["CC"]) if "CC" in os.environ else [
        next((name for name in ("cc", "gcc", "clang") if shutil.which(name)), "cc")]
    if not shutil.which(compiler[0]):
        parser.error("install a C11 compiler or set CC")
    config = (root / "src/Kconfig.projbuild").read_text()
    defaults = re.findall(r"\bconfig (\w+)\s+.*?\bdefault (-?\d+)", config, re.S)
    values = {f"CONFIG_{name}": value for name, value in defaults}
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
        subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-Wno-unused-function", "-O1", "-g",
                       f"-I{temp}"] + sanitizers + defines +
                       [str(root / "tests/host_audio_test.c"), "-lm", "-o", str(binary)],
                       check=True)
        mode = (["--speech-probe"] if args.speech_probe else ["--config-smoke"] if args.config_smoke else ["--reproduce"] if args.reproduce else
                ["--reproduce-ambient"] if args.reproduce_ambient else [])
        subprocess.run([str(binary)] + mode, check=True)


if __name__ == "__main__":
    main()
