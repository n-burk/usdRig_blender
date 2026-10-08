"""Capture independent Blender results for production-rig parity testing.

blender --background --factory-startup --disable-autoexec --python-exit-code 1
        --python tests/captureRigReference.py -- source.blend output-directory
"""
import argparse
import json
import math
from pathlib import Path
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "blender"))
from extract import bone_id, object_id, matrix


def render_objects():
    result = set()
    def visit(collection):
        if collection.hide_render:
            return
        result.update(collection.objects)
        for child in collection.children:
            visit(child)
    visit(bpy.context.scene.collection)
    return [obj for obj in sorted(result, key=object_id) if not obj.hide_render
            and obj.type in {"MESH", "CURVE", "SURFACE", "FONT", "CURVES"}
            and not any(token in obj.name.lower() for token in ("wgt", "widget"))]


def mesh_arrays(obj, depsgraph):
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
    if mesh is None:
        raise ValueError("no evaluated mesh for " + obj.name)
    try:
        points = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
        mesh.vertices.foreach_get("co", points)
        points = points.reshape(-1, 3).astype(np.float64)
        world = np.array(evaluated.matrix_world, dtype=np.float64)
        points = points @ world[:3, :3].T + world[:3, 3]
        counts = np.empty(len(mesh.polygons), dtype=np.int32)
        mesh.polygons.foreach_get("loop_total", counts)
        indices = np.empty(len(mesh.loops), dtype=np.int32)
        mesh.loops.foreach_get("vertex_index", indices)
        material_indices = np.empty(len(mesh.polygons), dtype=np.int32)
        mesh.polygons.foreach_get("material_index", material_indices)
        uv = np.empty(0, dtype=np.float32)
        render_uv = next((layer for layer in mesh.uv_layers if layer.active_render), mesh.uv_layers.active)
        if render_uv:
            uv = np.empty(len(mesh.loops) * 2, dtype=np.float32)
            render_uv.data.foreach_get("uv", uv)
        normals = np.empty(len(mesh.corner_normals) * 3, dtype=np.float32)
        mesh.corner_normals.foreach_get("vector", normals)
        normals = normals.reshape(-1, 3).astype(np.float64)
        normal_matrix = np.linalg.inv(world[:3, :3])
        normals = normals @ normal_matrix
        lengths = np.linalg.norm(normals, axis=1)
        normals /= np.maximum(lengths[:, None], 1e-30)
        return {"points": points, "counts": counts, "indices": indices, "normals": normals,
                "uv": uv.reshape(-1, 2), "material_indices": material_indices}
    finally:
        evaluated.to_mesh_clear()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("output")
    parser.add_argument("--include-mesh", action="append", default=[],
                        help="Also measure a named hidden dependency mesh")
    parser.add_argument("--jaw-sweep", action="store_true",
                        help="Capture additional jaw angles and a combined root/head/jaw edit")
    parser.add_argument("--shape-key", nargs=2, action="append", default=[], metavar=("MESH", "KEY"),
                        help="Capture the named shape channel at 0, 0.5, and 1")
    parser.add_argument("--control", nargs=2, action="append", default=[], metavar=("NAME", "OPERATION"),
                        help="Also capture a named control using translate, translate_fine, or rotate")
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    source, output = args.source, args.output
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.open_mainfile(filepath=str(Path(source).resolve()), load_ui=False, use_scripts=False)
    scene = bpy.context.scene
    bpy.context.view_layer.update()
    objects = render_objects()
    for name in args.include_mesh:
        obj = bpy.data.objects.get(name)
        if obj is None or obj.type != "MESH":
            parser.error("requested dependency mesh is missing: " + name)
        if obj not in objects:
            objects.append(obj)
    armatures = sorted([obj for obj in scene.objects if obj.type == "ARMATURE"], key=object_id)
    # The user-facing rig has the most custom shape controls; meta rigs are
    # generally hidden and are used for construction rather than posing.
    rig = max(armatures, key=lambda obj: (obj.visible_get(), sum(bool(pb.custom_shape) for pb in obj.pose.bones)))
    controls = {pb.name: pb for pb in rig.pose.bones if pb.custom_shape}
    for name,operation in args.control:
        if name not in controls or operation not in {"translate","translate_fine","rotate"}:
            parser.error("requested control or operation is unsupported: " + name + "/" + operation)
    preferred = [
        ("global translation", ("root", "Root"), "translate"),
        ("left hand IK", ("hand_ik.L", "IK-Wrist.L"), "translate"),
        ("right hand IK", ("hand_ik.R", "IK-Wrist.R"), "translate"),
        ("left arm FK rotation", ("upper_arm_fk.L", "FK-UpperArm.L"), "rotate"),
        ("right arm FK rotation", ("upper_arm_fk.R", "FK-UpperArm.R"), "rotate"),
        ("left foot IK", ("foot_ik.L", "IK-Foot.L"), "translate"),
        ("right foot IK", ("foot_ik.R", "IK-Foot.R"), "translate"),
        ("head rotation", ("head", "ROOT-Head"), "rotate"),
        ("jaw rotation", ("jaw_master", "Jaw", "jaw"), "rotate"),
        ("torso rotation", ("torso", "TORSO-Spine", "IK-CTR-Spine"), "rotate"),
    ]
    preferred.extend(("requested control " + name,(name,),operation) for name,operation in args.control)
    if "Tail-SPIK03" in controls:
        preferred.append(("tail tip translation", ("Tail-SPIK03",), "translate"))
    if "Eye.L" in controls:
        preferred.append(("left eye rotation", ("Eye.L",), "rotate"))
        preferred.append(("left eye target translation", ("Eye.L",), "translate_fine"))
    if "Eyelid_Master.T.L" in controls:
        preferred.append(("left upper eyelid rotation", ("Eyelid_Master.T.L",), "rotate"))
        preferred.append(("left upper eyelid translation", ("Eyelid_Master.T.L",), "translate_fine"))
    # Exercise both lattice Hook deformation and movement of its parent frame.
    # These optional controls are present on Ellie; other rigs retain their
    # existing capture set.
    for name in ("LTC-FannyPack_Bag", "LTC-FannyPack_Bag_Top"):
        if name in controls:
            preferred.append(("bag lattice translation", (name,), "translate_fine"))
            parent = "ROOT-" + name
            if parent in controls:
                preferred.append(("bag lattice parent rotation", (parent,), "rotate"))
    state = {pb.name: pb.matrix_basis.copy() for pb in rig.pose.bones}
    shape_channels = []
    for mesh_name, key_name in args.shape_key:
        obj = bpy.data.objects.get(mesh_name)
        keys = obj.data.shape_keys if obj and obj.type == "MESH" else None
        key = keys.key_blocks.get(key_name) if keys else None
        if key is None:
            parser.error("requested shape key is missing: " + mesh_name + "/" + key_name)
        shape_channels.append((obj, key, key.value))
        if obj not in objects:
            objects.append(obj)
    driver_inventory = []
    for obj in scene.objects:
        for owner in (obj, obj.data, getattr(obj.data, "shape_keys", None)):
            ad = getattr(owner, "animation_data", None)
            if ad:
                for curve in ad.drivers:
                    driver_inventory.append({"owner": owner.name, "path": curve.data_path,
                        "index": curve.array_index, "expression": curve.driver.expression,
                        "valid": curve.driver.is_valid, "simple": curve.driver.is_simple_expression})
    metadata = {"source": str(Path(source).resolve()), "blender_version": bpy.app.version_string,
        "script_autoexec": False, "drivers": driver_inventory,
        "frame": scene.frame_current, "rig": rig.name, "objects": [], "poses": [],
        "skipped_controls": [], "geometry_errors": [], "tolerance": 0.00002}
    extent = 1.0
    for obj in objects:
        bounds = [obj.matrix_world @ Vector(point) for point in obj.bound_box]
        extent = max(extent, *(max(p[a] for p in bounds) - min(p[a] for p in bounds) for a in range(3)))
        metadata["objects"].append({"id": object_id(obj), "name": obj.name, "type": obj.type,
            "base_vertices": len(obj.data.vertices) if obj.type == "MESH" else None,
            "modifiers": [{"name": mod.name, "type": mod.type, "enabled": mod.show_viewport}
                          for mod in obj.modifiers],
            "materials": [slot.material.name_full if slot.material else "" for slot in obj.material_slots]})

    def capture(label, edits):
        index = len(metadata["poses"])
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        pose = {"name": label, "sample": index + 1, "edits": edits, "meshes": [], "bones": {}}
        for obj in objects:
            try:
                arrays = mesh_arrays(obj, depsgraph)
            except Exception as error:
                metadata["geometry_errors"].append({"object": obj.name, "error": str(error)})
                continue
            name = f"pose-{index:02d}-mesh-{len(pose['meshes']):03d}.npz"
            np.savez_compressed(output / name, **arrays)
            pose["meshes"].append({"id": object_id(obj), "name": obj.name, "file": name,
                "vertices": len(arrays["points"]), "faces": len(arrays["counts"])})
        for obj in armatures:
            evaluated = obj.evaluated_get(depsgraph)
            for pb in evaluated.pose.bones:
                pose["bones"][bone_id(obj, pb.name)] = matrix(evaluated.matrix_world @ pb.matrix)
        metadata["poses"].append(pose)
        (output / "reference.json").write_text(json.dumps(metadata, indent=2))
        print(label, "meshes", len(pose["meshes"]), "vertices", sum(m["vertices"] for m in pose["meshes"]), flush=True)

    def reset():
        for _, key, value in shape_channels:
            key.value = value
        for pb in rig.pose.bones:
            pb.matrix_basis = state[pb.name]
        bpy.context.view_layer.update()

    def control_edits(names):
        edits = []
        for name in names:
            t, q, scale = controls[name].matrix_basis.decompose()
            values = list(t) + [math.degrees(v) for v in q.to_euler("XYZ")] + list(scale)
            edits.extend({"id": bone_id(rig, name), "channel": ch, "value": float(value)}
                         for ch, value in zip(("tx", "ty", "tz", "rx", "ry", "rz", "sx", "sy", "sz"), values))
        return edits

    capture("saved pose", [])
    for label, candidates, operation in preferred:
        chosen = next((name for name in candidates if name in controls), None)
        if chosen is None:
            metadata["skipped_controls"].append(label)
            continue
        reset()
        pb = controls[chosen]
        if operation.startswith("translate"):
            offset = extent * (0.01 if operation == "translate_fine" else
                               0.08 if chosen != "root" else 0.12)
            pb.location.x += offset
        else:
            t, q, s = pb.matrix_basis.decompose()
            from mathutils import Quaternion
            q = q @ Quaternion(Vector((1, 0, 0)), math.radians(20))
            pb.matrix_basis = Matrix.LocRotScale(t, q, s)
        capture(label + ": " + chosen, control_edits([chosen]))
    if args.jaw_sweep:
        jaw = next((n for n in ("jaw_master", "Jaw", "jaw") if n in controls), None)
        if jaw is None:
            parser.error("jaw sweep requested but no jaw control was found")
        from mathutils import Quaternion
        for angle in (-10, 10, 30, 40):
            reset()
            pb = controls[jaw]
            t, q, scale = pb.matrix_basis.decompose()
            pb.matrix_basis = Matrix.LocRotScale(t, q @ Quaternion(Vector((1, 0, 0)), math.radians(angle)), scale)
            capture("jaw sweep %g degrees" % angle, control_edits([jaw]))
        reset()
        changed = [jaw]
        t, q, scale = controls[jaw].matrix_basis.decompose()
        controls[jaw].matrix_basis = Matrix.LocRotScale(t, q @ Quaternion(Vector((1, 0, 0)), math.radians(30)), scale)
        head = next((n for n in ("head", "ROOT-Head") if n in controls), None)
        if head:
            changed.append(head)
            t, q, scale = controls[head].matrix_basis.decompose()
            controls[head].matrix_basis = Matrix.LocRotScale(t, q @ Quaternion(Vector((0, 0, 1)), math.radians(15)), scale)
        root = next((n for n in ("root", "Root") if n in controls), None)
        if root:
            changed.append(root)
            controls[root].location.x += extent * 0.08
        capture("combined root head jaw", control_edits(changed))
    for obj, key, _ in shape_channels:
        for value in (0.0, 0.5, 1.0):
            reset()
            key.value = value
            capture("shape %s/%s = %g" % (obj.name, key.name, value),
                    [{"id": object_id(obj), "shape_key": key.name, "value": value}])
    reset()
    capture("reset", [])
    metadata["invalid_drivers"] = sum(not d["valid"] for d in driver_inventory)
    (output / "reference.json").write_text(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
