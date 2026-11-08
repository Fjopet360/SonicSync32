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
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not shutil.which(compiler[0]):
        parser.error("install a C11 compiler or set CC")
    config = (root / "src/Kconfig.projbuild").read_text()
    defaults = re.findall(r"\bconfig (\w+)\s+.*?\bdefault (-?\d+)", config, re.S)
    defines = [f"-DCONFIG_{name}={value}" for name, value in defaults]
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
        subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-Wno-unused-function", "-O1", "-g",
                       "-fsanitize=undefined", "-fno-sanitize-recover=all",
                       f"-I{temp}"] + defines +
                       [str(root / "tests/host_audio_test.c"), "-lm", "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)] + (["--reproduce"] if args.reproduce else []), check=True)


if __name__ == "__main__":
    main()
