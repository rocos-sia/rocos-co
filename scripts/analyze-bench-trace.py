#!/usr/bin/env python3
"""Analyze closed SYNC windows in a candump -L trace of the default eRob PDO layout."""
import argparse
import csv
import json
import re
import sys
from collections import Counter
from contextlib import nullcontext
from pathlib import Path

# Classic data, RTR, error, and FD log tokens share the same timestamp prefix.
LINE = re.compile(r"\((\d+)\.(\d{1,9})\)\s+(\S+)\s+([0-9A-Fa-f]{3}|[0-9A-Fa-f]{8})#(\S*)")
FIELDS = ("sync", "start_ns", "period_ns", "feedback_complete_after_ns",
          "last_target_to_sync_ns", "missing", "duplicate", "wrong_dlc", "target_before_feedback")


def read_frames(lines, interface):
    """Preserve capture order and nanosecond precision for one interface."""
    previous = None
    for number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        match = LINE.fullmatch(line.strip())
        if not match:
            raise ValueError(f"line {number}: expected candump -L format")
        sec, frac, name, cob, payload = match.groups()
        if name != interface:
            continue
        stamp = int(sec) * 10**9 + int(frac.ljust(9, "0"))
        if previous is not None and stamp < previous:
            raise ValueError(f"line {number}: capture clock moved backwards")
        previous = stamp
        if len(cob) != 3 or payload.startswith(("#", "R")):
            continue
        if len(payload) > 16 or len(payload) % 2 or not re.fullmatch("[0-9A-Fa-f]*", payload):
            raise ValueError(f"line {number}: invalid classic CAN data")
        yield stamp, int(cob, 16), len(payload) // 2


def analyze(frames, nodes, first, last, writer, period_ns=8_000_000):
    expected = {base + node: size for node in nodes for base, size in ((0x180, 6), (0x280, 5), (0x200, 7))}
    targets = {0x200 + node for node in nodes}
    feedback = expected.keys() - targets
    summary = dict(period_target_ns=period_ns, closed_windows=0, incomplete_windows=0,
                   phase_errors=0, max_abs_sync_jitter_ns=0, max_feedback_complete_after_ns=None,
                   min_last_target_to_sync_ns=None)
    start = None
    sync = 0
    stamps, counts, wrong = {}, Counter(), set()
    anomalies = Counter()

    def labels(ids):
        return " ".join(f"{cob:03X}" for cob in sorted(ids))

    for stamp, cob, size in frames:
        if cob == 0x80:
            if size != 0:
                raise ValueError("expected unnumbered SYNC (COB-ID 080, DLC 0)")
            if start is not None and sync >= first:
                missing = expected.keys() - stamps.keys()
                duplicate = {key for key, count in counts.items() if count > 1}
                invalid = missing | duplicate | wrong
                period = stamp - start
                lag = None if feedback & invalid else max(stamps[key] for key in feedback) - start
                margin = None if targets & invalid else stamp - max(stamps[key] for key in targets)
                early = None if invalid else min(stamps[key] for key in targets) < start + lag
                summary["closed_windows"] += 1
                summary["incomplete_windows"] += int(bool(invalid))
                summary["phase_errors"] += int(early is True)
                anomalies.update(invalid)
                summary["max_abs_sync_jitter_ns"] = max(summary["max_abs_sync_jitter_ns"], abs(period - period_ns))
                for key, value, choose in (("max_feedback_complete_after_ns", lag, max),
                                           ("min_last_target_to_sync_ns", margin, min)):
                    if value is not None:
                        old = summary[key]
                        summary[key] = value if old is None else choose(old, value)
                if writer:
                    writer.writerow((sync, start, period, lag, margin, labels(missing),
                                     labels(duplicate), labels(wrong), early))
                if sync == last:
                    break
            sync += 1
            start = stamp
            stamps, counts, wrong = {}, Counter(), set()
        elif start is not None and cob in expected:
            counts[cob] += 1
            if size == expected[cob]:
                stamps[cob] = stamp
            else:
                wrong.add(cob)
    if not summary["closed_windows"] or (last is not None and summary["closed_windows"] != last - first + 1):
        raise ValueError("capture does not cover the requested closed SYNC windows")
    summary["invalid_windows_by_cob_id"] = {f"{k:03X}": v for k, v in sorted(anomalies.items())}
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--interface", required=True)
    parser.add_argument("--nodes", default="1,2,3,4,5,6", help="decimal Node-IDs using default eRob COB-IDs")
    parser.add_argument("--period-us", type=int, default=8000, help="configured SYNC period in microseconds")
    parser.add_argument("--first-sync", type=int, default=1, help="first measured window, one-based")
    parser.add_argument("--last-sync", type=int, help="final measured window, requires a following SYNC")
    parser.add_argument("--csv", type=Path, help="new file to receive one row per closed window")
    args = parser.parse_args()
    try:
        nodes = [int(item) for item in args.nodes.split(",")]
        if not nodes or len(set(nodes)) != len(nodes) or not all(1 <= n <= 127 for n in nodes):
            raise ValueError("expected different Node-IDs in 1..127")
        if args.period_us <= 0:
            raise ValueError("expected a positive SYNC period")
        if args.first_sync < 1 or (args.last_sync is not None and args.last_sync < args.first_sync):
            raise ValueError("invalid SYNC window range")
        with args.capture.open() as stream, (args.csv.open("x", newline="") if args.csv else nullcontext()) as out:
            writer = csv.writer(out) if out else None
            if writer:
                writer.writerow(FIELDS)
            summary = analyze(read_frames(stream, args.interface), nodes, args.first_sync, args.last_sync, writer,
                              args.period_us * 1000)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    json.dump(summary, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
