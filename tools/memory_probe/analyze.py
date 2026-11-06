"""Summarize numeric memory diagnostics without copying raw logs or identities."""
import argparse
import json
import re
from pathlib import Path


def summarize(report):
    phases = {}
    quiet = []
    for row in report.get("rows", []):
        if "MEMORY_PROBE phase=" in row:
            fields = dict(re.findall(r"(phase|cycle|pass|free|tasks|minimum)=(-?\d+)", row))
            if not {"phase", "pass", "free", "tasks", "minimum"} <= fields.keys():
                continue
            phase = phases.setdefault(fields["phase"], [])
            phase.append({key: int(value) for key, value in fields.items()})
        if "MEMORY_TCP phase=4 " in row:
            fields = dict(re.findall(r"(wait|free|active|pcb_size)=(-?\d+)", row))
            if {"wait", "free", "active", "pcb_size"} <= fields.keys():
                quiet.append({key: int(value) for key, value in fields.items()})
    result = {"diagnostic_sha256": report.get("diagnostic_sha256"),
              "passed": report.get("passed", False), "phases": {}}
    for name, values in phases.items():
        heaps = [v["free"] for v in values]
        tasks = [v["tasks"] for v in values]
        result["phases"][name] = {
            "cycles": len(values), "passed": sum(v["pass"] for v in values),
            "first_heap": heaps[0], "last_heap": heaps[-1],
            "min_settled_heap": min(heaps), "max_settled_heap": max(heaps),
            "task_count_range": [min(tasks), max(tasks)],
            "diagnostic_low_water": min(v["minimum"] for v in values),
        }
    if quiet:
        result["quiet_recovery"] = {
            "samples": len(quiet), "first_wait": quiet[0]["wait"],
            "last_wait": quiet[-1]["wait"], "last_active": quiet[-1]["active"],
            "recovered_heap": quiet[-1]["free"] - quiet[0]["free"],
            "first_heap": quiet[0]["free"], "last_heap": quiet[-1]["free"],
            "pcb_bytes": quiet[-1]["pcb_size"],
        }
    result["limits"] = "Diagnostic configuration and bounded workload only; not a lifetime no-leak claim."
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    text = json.dumps(summarize(json.loads(args.report.read_text(encoding="utf-8-sig"))), indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
