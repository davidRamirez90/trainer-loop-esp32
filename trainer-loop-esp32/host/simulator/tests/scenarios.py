#!/usr/bin/env python3
"""Run deterministic end-to-end scenarios through the simulator JSONL protocol."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import selectors
import shutil
import subprocess
import sys
import tempfile
from typing import Any


HERE = pathlib.Path(__file__).resolve().parent
SCENARIOS = HERE / "scenarios"
ROOT = HERE.parents[2]
RESPONSE_TIMEOUT_SECONDS = 20.0


def read_response(process: subprocess.Popen[str], selector: selectors.BaseSelector,
                  index: int) -> str:
    assert process.stdout is not None
    if not selector.select(RESPONSE_TIMEOUT_SECONDS):
        raise AssertionError(f"timed out waiting {RESPONSE_TIMEOUT_SECONDS:g}s for response {index + 1}")
    line = process.stdout.readline()
    if not line:
        raise AssertionError(f"simulator exited before response {index + 1}")
    return line


def value_at(document: Any, path: str) -> Any:
    value = document
    for part in path.split("."):
        if isinstance(value, list):
            value = value[int(part)]
        elif isinstance(value, dict):
            value = value[part]
        else:
            raise AssertionError(f"cannot read {path!r} through {type(value).__name__}")
    return value


def assert_state(state: dict[str, Any], checks: dict[str, Any]) -> None:
    for path, expected in checks.items():
        actual = value_at(state, path)
        if isinstance(expected, dict) and "op" in expected:
            op = expected["op"]
            wanted = expected.get("value")
            passed = {
                "eq": lambda: actual == wanted,
                "ne": lambda: actual != wanted,
                "gt": lambda: actual > wanted,
                "gte": lambda: actual >= wanted,
                "lt": lambda: actual < wanted,
                "lte": lambda: actual <= wanted,
                "contains": lambda: wanted in actual,
                "not_contains": lambda: wanted not in actual,
                "any_contains": lambda: any(wanted in item for item in actual),
                "len_gte": lambda: len(actual) >= wanted,
                "len_eq": lambda: len(actual) == wanted,
                "len_gt": lambda: len(actual) > wanted,
                "max_gap_lte": lambda: all(
                    current["active_ms"] - previous["active_ms"] <= wanted
                    for previous, current in zip(actual, actual[1:])
                ),
                "max_active_lte": lambda: max(
                    (sample["active_ms"] for sample in actual), default=0
                ) <= wanted,
                "ordered_contains": lambda: _ordered_contains(actual, wanted),
                "truthy": lambda: bool(actual),
                "falsy": lambda: not actual,
            }.get(op)
            if passed is None:
                raise AssertionError(f"unsupported assertion operator {op!r}")
            if not passed():
                raise AssertionError(f"{path}: {actual!r} does not satisfy {op} {wanted!r}")
        elif actual != expected:
            raise AssertionError(f"{path}: expected {expected!r}, got {actual!r}")


def _ordered_contains(actual: list[Any], wanted: list[str]) -> bool:
    cursor = 0
    for item in actual:
        if cursor < len(wanted) and wanted[cursor] in item:
            cursor += 1
    return cursor == len(wanted)


def run_scenario(binary: pathlib.Path, scenario_path: pathlib.Path,
                 artifacts: pathlib.Path) -> None:
    scenario = json.loads(scenario_path.read_text())
    run_root = artifacts / scenario_path.stem
    if run_root.exists():
        shutil.rmtree(run_root)
    run_root.mkdir(parents=True)
    storage = run_root / "run"
    process = subprocess.Popen(
        [str(binary), "--storage", str(storage)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    responses: list[dict[str, Any]] = []
    commands: list[dict[str, Any]] = []
    failure: str | None = None
    try:
        assert process.stdin is not None and process.stdout is not None
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        for index, step in enumerate(scenario["steps"]):
            command = step["command"]
            commands.append(command)
            process.stdin.write(json.dumps(command, separators=(",", ":")) + "\n")
            process.stdin.flush()
            line = read_response(process, selector, index)
            response = json.loads(line)
            responses.append(response)
            expects_error = step.get("error")
            if expects_error is None and not response.get("ok"):
                raise AssertionError(f"step {index + 1} {command}: {response.get('error')}")
            if expects_error is not None:
                if response.get("ok") or expects_error not in response.get("error", ""):
                    raise AssertionError(f"step {index + 1}: expected error containing {expects_error!r}, got {response}")
            assert_state(response["state"], step.get("expect", {}))

        for glob in scenario.get("fit_globs", []):
            fits = sorted((storage / "storage").glob(glob))
            if not fits:
                raise AssertionError(f"no FIT files matched {glob!r}")
            validate_fit_files(fits)
    except Exception as exc:  # Preserve the protocol transcript and current artifacts.
        failure = f"{type(exc).__name__}: {exc}"
        if process.poll() is None and process.stdin is not None and process.stdout is not None:
            try:
                screenshot = run_root / "failure.png"
                process.stdin.write(json.dumps({"op": "screenshot", "path": str(screenshot)}) + "\n")
                process.stdin.flush()
                if 'selector' in locals():
                    read_response(process, selector, len(commands))
            except Exception:
                pass
    finally:
        if process.stdin is not None:
            process.stdin.close()
        if process.poll() is None:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
        stderr = process.stderr.read() if process.stderr is not None else ""
        exit_code = process.wait(timeout=10)

    if failure is None and exit_code != 0:
        failure = f"simulator exited with status {exit_code}"

    if failure:
        (run_root / "failure.json").write_text(json.dumps({
            "scenario": scenario,
            "commands": commands,
            "responses": responses,
            "stderr": stderr,
            "exit_code": exit_code,
            "failure": failure,
        }, indent=2) + "\n")
        raise AssertionError(f"{scenario['name']}: {failure}; artifacts: {run_root}")
    print(f"PASS {scenario['name']}")


def validate_fit_files(paths: list[pathlib.Path]) -> None:
    configured_python = os.environ.get("TRAINER_FIT_PYTHON")
    if configured_python:
        python = pathlib.Path(configured_python)
        if not python.is_file():
            resolved = shutil.which(configured_python)
            if resolved is None:
                raise AssertionError(f"TRAINER_FIT_PYTHON is not executable: {configured_python}")
            python = pathlib.Path(resolved)
    else:
        python = ROOT / ".venv-fit" / "bin" / "python"
    if not python.is_file():
        raise AssertionError(f"Garmin FIT SDK environment is missing: {python}")
    validator = HERE / "validate_fits.py"
    if not validator.is_file():
        raise AssertionError(f"FIT validator is missing: {validator}")
    subprocess.run([str(python), str(validator), *(str(path) for path in paths)], check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=pathlib.Path,
                        default=ROOT / "build-simulator" / "host" / "simulator" / "trainer_simulator")
    parser.add_argument("--artifacts", type=pathlib.Path,
                        default=pathlib.Path(tempfile.gettempdir()) / "trainer-loop-scenario-artifacts")
    parser.add_argument("scenarios", nargs="*", help="scenario JSON files or names")
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"simulator binary not found: {binary}; build with cmake --build build-simulator")
    args.artifacts.mkdir(parents=True, exist_ok=True)
    selected = args.scenarios or [p.stem for p in sorted(SCENARIOS.glob("*.json"))]
    failures = []
    for entry in selected:
        path = pathlib.Path(entry)
        scenario_path = path if path.is_file() else SCENARIOS / (entry if entry.endswith(".json") else entry + ".json")
        if not scenario_path.is_file():
            parser.error(f"scenario file not found: {scenario_path}")
        try:
            run_scenario(binary, scenario_path, args.artifacts)
        except AssertionError as exc:
            failures.append(f"{scenario_path.stem}: {exc}")
            print(f"FAIL {failures[-1]}", file=sys.stderr)
    if failures:
        print(f"{len(failures)} of {len(selected)} scenarios failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        raise SystemExit(1)
