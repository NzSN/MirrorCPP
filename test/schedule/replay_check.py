#!/usr/bin/env python3
"""Bounded DPM-1 fixture acceptance; does not invoke a model checker."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    with path.open("x", encoding="utf-8") as stream:
        stream.write(json.dumps(value, indent=2, sort_keys=True) + "\n")
    path.chmod(0o600)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    if args.out is None:
        args.out = Path(tempfile.mkdtemp(prefix="mirrorcpp-dpm1-")) / "evidence"
    args.out.mkdir(mode=0o700, parents=True, exist_ok=False)
    source = Path(__file__).resolve().parents[2]
    paths = ["include/mirrorcpp/schedule.hpp", "src/schedule.cpp",
             "examples/scheduled_counter.hpp", "examples/scheduled_counter.cpp",
             "test/unit/schedule_test.cpp", "test/schedule/replay_check.py",
             "CMakeLists.txt", "cmake/mirrorcppConfig.cmake.in",
             "include/mirrorcpp/mirrorcpp.hpp", "examples/CMakeLists.txt",
             "test/unit/CMakeLists.txt", "test/packaging/main.cpp"]
    before = {name: sha(source / name) for name in paths}
    binary_sha = sha(binary)
    seen = set()
    records = []

    def invoke(name, plan=None):
        argv = [str(binary)]
        if plan is not None:
            path = args.out / (name + ".schedule.json")
            write(path, plan)
            argv += ["--replay", str(path)]
        result = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, timeout=10, check=False)
        require(result.returncode == 0, f"{name}: fixture exited {result.returncode}: {result.stderr[:2000]}")
        require(len(result.stdout.encode()) <= 1_048_576, "fixture receipt exceeds bound")
        receipt = json.loads(result.stdout)
        require(receipt["schema"] == "mirrors.checkpoint-replay/v1", "wrong receipt schema")
        require(receipt["passed"] is True and receipt["outcome"] == "completed", "fixture did not complete")
        require(receipt["scheduleCompleted"] is True and receipt["cleanup"] == "confirmed", "unconfirmed cleanup")
        require(receipt["remainingActors"] == [], "remaining actors")
        require(receipt["coverage"] == "recorded-schedule-only", "overstated coverage")
        nonce = receipt["executionId"]
        require(isinstance(nonce, str) and re.fullmatch(r"[0-9a-f]{32}", nonce) and nonce not in seen,
                "invalid or repeated execution identity")
        seen.add(nonce)
        if plan is not None:
            require(receipt["schedule"] == plan, "replay changed recorded inputs or schedule")
        expected_steps = receipt["schedule"]["steps"]
        expected_actors = [{"actor": "a", "operation": "increment-a"},
                           {"actor": "b", "operation": "increment-b"}]
        require(receipt["actors"] == expected_actors, "actor mapping differs")
        require(receipt["checkpoints"] == ["read", "write"], "checkpoint mapping differs")
        events = receipt["events"]
        require(len(events) == 2 * len(expected_steps), "event denominator differs")
        current = {"a": "$start", "b": "$start"}
        for index, step in enumerate(expected_steps):
            for offset, kind, checkpoint in [(0, "permit", current[step["actor"]]),
                                            (1, "arrival", step["checkpoint"])]:
                expected = {"ordinal": 2 * index + offset, "step": index, "kind": kind,
                            "actor": step["actor"], "operation": "increment-" + step["actor"],
                            "checkpoint": checkpoint}
                require(events[2 * index + offset] == expected, "event differs from actual declared-point schedule")
            current[step["actor"]] = step["checkpoint"]
        require(len(receipt["observations"]) == len(expected_steps) + 1, "observation denominator differs")
        file = args.out / (name + ".receipt.json")
        write(file, receipt)
        records.append({"case": name, "file": file.name, "sha256": sha(file)})
        return receipt

    baseline = invoke("baseline")
    require(baseline["observations"][-1]["state"]["value"] == 1, "overlapping execution did not lose an update")
    for overlap in [True, False]:
        for initial in [0, 5]:
            plan = json.loads(json.dumps(baseline["schedule"]))
            plan["inputs"] = {"initial": initial}
            if not overlap:
                plan["steps"] = [{"actor": actor, "checkpoint": point}
                                 for actor in ["a", "b"] for point in ["read", "write", "$done"]]
            projected = None
            for repeat in range(4):
                label = f"{'overlap' if overlap else 'serial'}-{initial}-{repeat}"
                receipt = invoke(label, plan)
                expected_values = ([initial, initial, initial, initial + 1, initial + 1, initial + 1, initial + 1]
                                   if overlap else [initial, initial, initial + 1, initial + 1, initial + 1, initial + 2, initial + 2])
                require(receipt["observations"] == [{"afterSteps": i, "state": {"value": value}}
                                                     for i, value in enumerate(expected_values)],
                        "actual fixture observations differ")
                comparable = {k: v for k, v in receipt.items() if k not in ["generation", "executionId"]}
                if projected is None:
                    projected = comparable
                else:
                    require(comparable == projected, "fresh-run replay diverged")
    require(sha(binary) == binary_sha, "executable changed during acceptance")
    require(before == {name: sha(source / name) for name in paths}, "source changed during acceptance")
    write(args.out / "acceptance.json", {
        "schema": "mirrorcpp.dpm1-fixture-acceptance/v1", "status": "passed",
        "sourceRevision": subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip(),
        "sourceFiles": before, "executableSha256": binary_sha, "replayExecutions": len(records),
        "scheduleShapes": 2, "inputAssignments": 2, "records": records,
        "scope": "portable cooperative fixture; scheduling and actual observations only",
        "modelComparison": "not exercised; DPM-2", "productionWriteSentry": "not exercised; DPM-3",
        "explorationCompleteness": "not claimed", "capabilityPublication": "not claimed",
        "identityLabels": "fixture labels; source and executable hashes retained separately"
    })
    print(f"DPM-1 fixture acceptance passed: {len(records)} fresh executions, two schedules, two input assignments")


if __name__ == "__main__":
    main()
