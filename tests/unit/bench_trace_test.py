"""Exercise data loss, ambiguous windows and capture boundaries without a CAN interface."""
import csv
import io
import runpy
import sys

tool = runpy.run_path(sys.argv[1])
nodes = range(1, 7)
frames = [(100, 0x80, 0)]
for n in nodes:
    frames.extend([(100+n, 0x180+n, 6), (110+n, 0x280+n, 5)])
frames.extend((200+n, 0x200+n, 7) for n in nodes)
end = 8_000_100
frames.extend([(end, 0x80, 0), (end+10, 0x181, 5), (end+20, 0x201, 7),
               (end+30, 0x201, 7), (end+8_000_050, 0x80, 0)])
table = io.StringIO()
result = tool["analyze"](frames, nodes, 1, 2, csv.writer(table))
assert result["closed_windows"] == 2 and result["incomplete_windows"] == 1
assert result["max_abs_sync_jitter_ns"] == 50 and result["max_feedback_complete_after_ns"] == 16
assert result["min_last_target_to_sync_ns"] == end - 206
assert result["invalid_windows_by_cob_id"]["201"] == 1
rows = list(csv.reader(io.StringIO(table.getvalue())))
assert rows[1][6:8] == ["201", "181"] and "286" in rows[1][5].split()
# A valid target arriving before all feedback is an independently observable phase error.
early = sorted([(100, 0x80, 0), (end, 0x80, 0)] +
               [(150+n, 0x180+n, 6) for n in nodes] + [(170+n, 0x280+n, 5) for n in nodes] +
               [(110+n, 0x200+n, 7) for n in nodes])
assert tool["analyze"](early, nodes, 1, 1, None)["phase_errors"] == 1

# A three-axis 10 ms bench must use its actual period and expected COB-ID set.
three = [(100, 0x80, 0)]
for n in range(1, 4):
    three.extend([(100+n, 0x180+n, 6), (110+n, 0x280+n, 5), (200+n, 0x200+n, 7)])
three = sorted(three + [(10_000_100, 0x80, 0)])
result = tool["analyze"](three, range(1, 4), 1, 1, None, 10_000_000)
assert result["period_target_ns"] == 10_000_000 and result["max_abs_sync_jitter_ns"] == 0
assert result["incomplete_windows"] == 0 and result["phase_errors"] == 0

lines = ["(1730000000.123456789) can0 080#", "(1730000000.123456790) can1 181#1234",
         "(1730000000.123456791) can0 00000181#1234", "(1730000000.123456792) can0 181#1234"]
assert list(tool["read_frames"](lines, "can0")) == [
    (1730000000123456789, 0x80, 0), (1730000000123456792, 0x181, 2)]
for fn in (lambda: tool["analyze"](frames, nodes, 2, 3, None),
           lambda: list(tool["read_frames"](reversed(lines), "can0"))):
    try:
        fn()
    except ValueError:
        pass
    else:
        raise AssertionError("incomplete range or backwards capture clock accepted")
