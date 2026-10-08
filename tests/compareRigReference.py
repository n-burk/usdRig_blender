"""Compare an editable native RigExec stage against independent Blender poses."""
import argparse
import hashlib
import json
import os
from pathlib import Path

import numpy as np
from pxr import Usd, UsdGeom


def resolve_edit_attribute(edit, controls, meshes):
    """Resolve source edits through the native control/shape channel contract."""
    if "shape_key" in edit:
        mesh = meshes[edit["id"]]
        matches = [a for a in mesh.GetAttributes()
                   if a.GetCustomDataByKey("blender:shapeKey") == edit["shape_key"]]
        if len(matches) != 1:
            raise ValueError("missing or ambiguous native shape channel: " + edit["shape_key"])
        return matches[0]
    return controls[edit["id"]].GetAttribute("avars:" + edit["channel"])


def compare_pose(sample, reference, reference_path, providers, meshes, baseline,
                 sample_index, read_points, read_frame, valid=True,
                 diagnostics=(), measurement_prefix=None):
    """Compare reference data to a caller's actual mesh/frame source.

    usdview supplies its published snapshot; the CLI supplies evaluator output.
    Readers return points and a row-major frame matrix, or raise KeyError.
    Measurements are numerical evidence only and never author playback USD.
    """
    result = {"name": sample["name"], "valid": valid,
              "diagnostics": list(diagnostics), "meshes": [], "bones": {},
              "passed": valid}
    for mesh in sample["meshes"]:
        expected = np.load(reference_path.parent / mesh["file"])
        row = {"name": mesh["name"], "expected_vertices": len(expected["points"]), "passed": False}
        prim = meshes.get(mesh["id"])
        if prim is None:
            row["error"] = "mesh missing from native conversion"
        else:
            try:
                points = np.asarray(read_points(prim), dtype=np.float64)
            except KeyError:
                row["error"] = "native evaluation did not produce mesh points"
                result["meshes"].append(row)
                result["passed"] = False
                continue
            row["actual_vertices"] = len(points)
            counts = np.asarray(prim.GetAttribute("faceVertexCounts").Get(), dtype=np.int32)
            indices = np.asarray(prim.GetAttribute("faceVertexIndices").Get(), dtype=np.int32)
            row["topology_matches"] = (np.array_equal(counts, expected["counts"])
                                      and np.array_equal(indices, expected["indices"]))
            if points.shape == expected["points"].shape:
                errors = np.linalg.norm(points - expected["points"], axis=1)
                row["max_error"] = float(errors.max(initial=0))
                row["rms_error"] = float(np.sqrt(np.mean(errors ** 2))) if len(errors) else 0
                row["worst_vertices"] = [int(i) for i in np.argsort(errors)[-8:][::-1]]
                row["passed"] = row["topology_matches"] and row["max_error"] <= reference["tolerance"]
                if sample_index == 0:
                    baseline[mesh["id"]] = (expected["points"].copy(), points.copy())
                if mesh["id"] in baseline:
                    source_start, native_start = baseline[mesh["id"]]
                    source_delta = expected["points"] - source_start
                    native_delta = points - native_start
                    row["reference_max_displacement"] = float(np.linalg.norm(source_delta, axis=1).max(initial=0))
                    row["native_max_displacement"] = float(np.linalg.norm(native_delta, axis=1).max(initial=0))
                    row["max_displacement_error"] = float(np.linalg.norm(native_delta - source_delta, axis=1).max(initial=0))
            else:
                row["error"] = "evaluated topology differs from native source mesh"
            uv_primvar = UsdGeom.PrimvarsAPI(prim).GetPrimvar("st")
            actual_uv = np.asarray(uv_primvar.ComputeFlattened() if uv_primvar else [], dtype=np.float64).reshape(-1, 2)
            row["uv_matches"] = (actual_uv.shape == expected["uv"].shape
                                 and bool(np.allclose(actual_uv, expected["uv"], atol=reference["tolerance"], rtol=0)))
            row["passed"] &= row["uv_matches"]
            if measurement_prefix is not None:
                name = f"{measurement_prefix.name}-pose-{sample_index:02d}-mesh-{len(result['meshes']):03d}.npz"
                np.savez_compressed(measurement_prefix.parent / name, points=points, counts=counts, indices=indices)
                row["native_measurement"] = name
        result["meshes"].append(row)
        result["passed"] &= row["passed"]
    maximum = 0
    failing = 0
    worst = []
    deform_failing = 0
    rig_failing = 0
    missing = []
    for ident, expected in sample["bones"].items():
        prim = providers.get(ident)
        if prim is None:
            failing += 1
            missing.append(ident)
            continue
        try:
            frame = read_frame(prim)
        except KeyError:
            failing += 1
            missing.append(ident)
            continue
        if frame is None:
            failing += 1
            missing.append(ident)
            continue
        matrix = np.asarray(frame).reshape(4, 4)
        expected = np.asarray(expected).reshape(4, 4)
        error = float(np.max(np.abs(matrix - expected)))
        maximum = max(maximum, error)
        failing += error > reference["tolerance"]
        ident_parts = json.loads(ident)
        rig_failing += error > reference["tolerance"] and ident_parts[2] == reference["rig"]
        source_data = json.loads(prim.GetCustomDataByKey("blender:source"))
        deform_failing += error > reference["tolerance"] and source_data.get("deform", False) and ident_parts[2] == reference["rig"]
        worst.append({"id": ident, "matrix_error": error,
                      "origin_error": float(np.linalg.norm(matrix[3, :3] - expected[3, :3]))})
    result["bones"] = {"compared": len(sample["bones"]), "failing": failing, "max_matrix_error": maximum,
                       "missing_frames": missing,
                       "character_rig_failing": int(rig_failing), "character_deform_failing": int(deform_failing),
                       "worst": sorted(worst, key=lambda row: row["matrix_error"], reverse=True)[:20]}
    result["passed"] &= failing == 0
    return result


def main():
    import rigexec
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path)
    parser.add_argument("reference", type=Path)
    parser.add_argument("report", type=Path)
    parser.add_argument("--write-measurements", action="store_true", help="Save native point measurements for inspection; does not author playback USD")
    parser.add_argument("--incremental", action="store_true", help="Reset channels through ordinary edits, without clearing the session layer between poses")
    args = parser.parse_args()
    reference = json.loads(args.reference.read_text())
    stage = Usd.Stage.Open(str(args.stage.resolve()))
    if not stage:
        raise RuntimeError("cannot open native rig")
    rig = rigexec.Rig(stage, "/Rig")
    rig.compile()
    providers = {}
    meshes = {}
    for prim in stage.Traverse():
        ident = prim.GetCustomDataByKey("blender:id")
        if not ident:
            continue
        if prim.IsA(UsdGeom.Mesh):
            meshes[ident] = prim
        elif prim.GetTypeName() in {"RigExecJoint", "RigExecControl"}:
            providers[ident] = prim
    controls = {}
    for ident, provider in providers.items():
        relation = provider.GetRelationship("blender:channelControl")
        targets = relation.GetTargets() if relation else []
        controls[ident] = stage.GetPrimAtPath(targets[0]) if len(targets) == 1 else provider
        if not controls[ident]:
            raise RuntimeError(f"missing native channel control for {ident}")
    stage.SetEditTarget(stage.GetSessionLayer())
    report = {"stage": str(args.stage.resolve()), "source": reference["source"],
        "run_id": os.environ.get("USDBLENDERRIG_VALIDATION_RUN"),
        "blender_version": reference["blender_version"], "script_autoexec": reference["script_autoexec"],
        "edit_mode": "incremental" if args.incremental else "session-clear",
        "invalid_source_drivers": reference.get("invalid_drivers", 0),
        "tolerance": reference["tolerance"], "poses": [], "completed": False, "passed": not reference.get("geometry_errors"),
        "reference_geometry_errors": reference.get("geometry_errors", []),
        "skipped_controls": reference.get("skipped_controls", []),
        "conversion_complete": stage.GetRootLayer().customLayerData.get("blenderRig:complete", False),
        "conversion_diagnostics": list(stage.GetDefaultPrim().GetAttribute("blender:diagnostics").Get() or [])}
    if Path(reference["source"]).is_file():
        report["source_sha256"] = hashlib.sha256(Path(reference["source"]).read_bytes()).hexdigest()
    baseline = {}
    initial_channels = {}
    if args.incremental:
        for sample in reference["poses"]:
            for edit in sample["edits"]:
                attribute = resolve_edit_attribute(edit, controls, meshes)
                initial_channels[attribute.GetPath()] = (attribute, attribute.Get())
    for sample_index, sample in enumerate(reference["poses"]):
        if args.incremental:
            for attribute, value in initial_channels.values():
                attribute.Set(value)
        else:
            stage.GetSessionLayer().Clear()
        for edit in sample["edits"]:
            resolve_edit_attribute(edit, controls, meshes).Set(edit["value"])
        pose = rig.evaluate(-1)
        def read_points(prim):
            moved = pose.moved_property(str(prim.GetPath()) + ".points")
            return moved if moved is not None else prim.GetAttribute("points").Get()

        def read_frame(prim):
            frame = pose.joint_frame(str(prim.GetPath()), True)
            return frame.to_matrix4() if frame is not None else None

        result = compare_pose(sample, reference, args.reference, providers, meshes,
                              baseline, sample_index, read_points, read_frame,
                              pose.valid, pose.diagnostics,
                              args.report if args.write_measurements else None)
        failing = result["bones"]["failing"]
        report["passed"] &= result["passed"]
        report["poses"].append(result)
        args.report.write_text(json.dumps(report, indent=2))
        print(sample["name"], "PASS" if result["passed"] else "FAIL",
              "max mesh error", max((m.get("max_error", 0) for m in result["meshes"]), default=0),
              "failing bones", failing, flush=True)
    report["completed"] = True
    args.report.write_text(json.dumps(report, indent=2))
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
