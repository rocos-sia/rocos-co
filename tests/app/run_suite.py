"""Run the App scenarios on one prepared vcan interface and retain the evidence index."""
import json
import os
from pathlib import Path
import subprocess
import sys

runner = Path(__file__).with_name("integration.py")
master_id = 700000 + os.getpid()
runs = []
for scenario in ("normal", "normal", "wrong-id", "wrong-name", "wrong-width", "fault"):
    command = [
        sys.executable, str(runner), *sys.argv[1:],
        "--scenario", scenario, "--test-shm-id", str(master_id),
    ]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    print(result.stdout, flush=True)
    if result.returncode:
        raise RuntimeError(result.stderr)
    directory = Path(result.stdout.strip().split("passed: ")[-1])
    runs.append({"directory": directory.name, **json.loads((directory / "result.json").read_text())})

assert runs[0]["instance"] != runs[1]["instance"], "restart reused the old SHM instance"
(directory.parent / "last-suite.json").write_text(json.dumps(runs, indent=2) + "\n")
