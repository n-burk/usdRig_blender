#!/usr/bin/env python3
"""Convert a .blend and compare native rig evaluation with Blender control edits.

Requires Blender, NumPy, and the same USD/RigExec environment as convert.py.
Writes an editable native USD, numerical reference measurements, logs, and a
parity report. It never creates a sampled USD playback replacement.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, help="Directory for native USD and parity evidence")
    parser.add_argument("--blender", default=os.environ.get("USDBLENDERRIG_BLENDER", "blender"))
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if not source.is_file() or source.suffix.lower() != ".blend":
        parser.error("source must be an existing .blend file")
    output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[1]
    before = digest(source)
    manifest = {"source": str(source), "sha256": before, "native_only": True,
                "run_id": uuid.uuid4().hex, "steps": []}
    flags = [args.blender, "--background", "--factory-startup", "--disable-autoexec", "--python-exit-code", "1"]
    steps = [
        ("extract", flags + ["--python", str(root / "blender/extract.py"), "--", str(source), str(output / "rig.blendrig")], None),
        ("reference", flags + ["--python", str(root / "tests/captureRigReference.py"), "--", str(source), str(output)], None),
        ("convert", [sys.executable, str(root / "tools/convert.py"), str(output / "rig.blendrig"), str(output / "native.usdc")], None),
        ("compare", [sys.executable, str(root / "tests/compareRigReference.py"), str(output / "native.usdc"),
                     str(output / "reference.json"), str(output / "native-report.json"), "--write-measurements"],
                    {**os.environ, "USDBLENDERRIG_BLENDER": "/blender-unavailable-during-native-evaluation",
                     "USDBLENDERRIG_VALIDATION_RUN": manifest["run_id"]}),
    ]
    status = 0
    try:
        for name, command, environment in steps:
            print(name + ": " + str(source), flush=True)
            if name == "compare":
                # A compile/evaluation exception must not leave an earlier
                # successful report looking like the result of this run.
                (output / "native-report.json").write_text(json.dumps({
                    "run_id": manifest["run_id"], "source": str(source),
                    "completed": False, "passed": False, "poses": []}, indent=2))
            with (output / (name + ".log")).open("w") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=environment)
            manifest["steps"].append({"name": name, "exit_code": result.returncode})
            if result.returncode:
                status = result.returncode if result.returncode > 0 else 2
                print(name + " failed; see " + str(output / (name + ".log")), flush=True)
                break
    finally:
        manifest["source_unchanged"] = before == digest(source)
        (output / "source.json").write_text(json.dumps(manifest, indent=2))
    if not manifest["source_unchanged"]:
        raise RuntimeError("source file changed during validation")
    if any(step["name"] == "convert" and step["exit_code"] == 0 for step in manifest["steps"]):
        print("Native USD: " + str(output / "native.usdc"))
    if any(step["name"] == "compare" for step in manifest["steps"]) and (output / "native-report.json").is_file():
        print("Parity report: " + str(output / "native-report.json"))
    raise SystemExit(status)


if __name__ == "__main__":
    main()
