#!/usr/bin/env python3
"""Capture every detector frame from a confirmed board port, or summarize a log.

Example: python tools/capture_detector.py --port COM6 --seconds 30 --label bass_hits
Requires pyserial for live capture. Does not flash, change settings or reset the board.
Keep music off until startup calibration completes, then start a labelled capture.
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import time


def summarize(lines):
    records = []
    ansi = re.compile(r"\x1b\[[0-9;]*m")
    for line in lines:
        line = ansi.sub("", line)
        if "AUDIO_FRAME " not in line:
            continue
        fields = dict(re.findall(r"(\w+)=([^\s]+)", line.split("AUDIO_FRAME ", 1)[1]))
        if all(k in fields for k in ("n", "bass", "gate", "open", "blue", "speech", "reason")):
            records.append(fields)
    usable = [r for r in records if r["reason"] != "calibrating"]
    blue = [int(r["blue"]) for r in usable]
    rises = sum(b > 0 and (i == 0 or blue[i-1] == 0) for i, b in enumerate(blue))
    frame_numbers = [int(r["n"]) for r in records]
    gaps = sum(b != a+1 for a, b in zip(frame_numbers, frame_numbers[1:]))
    levels = [float(r["bass"]) for r in usable]
    opens = [float(r["open"]) for r in usable]
    margins = [float(r["bass"]) - float(r["open"]) for r in usable]
    excluded = [float(r["excluded"]) for r in usable if "excluded" in r]
    unfiltered = [float(r["raw"]) for r in usable if "raw" in r]
    return {
        "records": len(records), "after_calibration": len(usable),
        "frame_gaps_or_resets": gaps,
        "forced_off_seen": any(r["reason"] == "forced_off" for r in usable),
        "blue_on_frames": sum(b > 0 for b in blue),
        "blue_rises_in_capture": rises,
        "first_frame_already_on": bool(blue and blue[0]),
        "blue_duties": sorted(set(blue)),
        "bass_db_range": [min(levels), max(levels)] if levels else None,
        "opening_db_range": [min(opens), max(opens)] if opens else None,
        "maximum_db_above_opening": max(margins) if margins else None,
        "excluded_low_bin_db_range": [min(excluded), max(excluded)] if excluded else None,
        "unfiltered_bass_db_range": [min(unfiltered), max(unfiltered)] if unfiltered else None,
        "voice_filter_values": sorted(set(r.get("vf", "unknown") for r in usable)),
        "speech_frames": sum(r["speech"] == "1" for r in usable),
        "note": "ON frames are not bass hit counts. First ON frame may predate capture. Compare labelled sound and physical LEDs."
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="confirmed board serial port")
    source.add_argument("--from-log", type=Path)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--label", default="unlabelled")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--expect-off", action="store_true", help="fail on any blue ON or incomplete/forced-off data")
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    if args.from_log:
        lines = args.from_log.read_text(errors="replace").splitlines()
    else:
        try:
            import serial
        except ImportError:
            parser.error("pyserial required; use PlatformIO Python or install pyserial")
        destination = args.output or Path(".sonicsync-captures") / (datetime.now().strftime("%Y%m%d-%H%M%S") + ".log")
        destination.parent.mkdir(parents=True, exist_ok=True)
        lines = []
        # Set control-line states before opening; never request a board reset.
        port = serial.Serial(port=None, baudrate=args.baud, timeout=0.25)
        port.dtr = False
        port.rts = False
        port.port = args.port
        with destination.open("x", encoding="utf-8") as output, port:
            output.write("# label=" + args.label + "\n")
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                line = port.readline().decode("utf-8", errors="replace")
                if line:
                    output.write(line)
                    lines.append(line)
        print("Saved", destination)
    summary = summarize(lines)
    summary["label"] = args.label
    print(json.dumps(summary, indent=2))
    invalid = not summary["after_calibration"] or summary["forced_off_seen"] or summary["frame_gaps_or_resets"]
    if invalid or (args.expect_off and summary["blue_on_frames"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
