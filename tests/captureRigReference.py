"""Capture independent Blender results for production-rig parity testing.

blender --background --factory-startup --disable-autoexec --python-exit-code 1
        --python tests/captureRigReference.py -- source.blend output-directory
"""
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
    source, output = sys.argv[sys.argv.index("--") + 1:]
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.open_mainfile(filepath=str(Path(source).resolve()), load_ui=False, use_scripts=False)
    scene = bpy.context.scene
    bpy.context.view_layer.update()
    objects = render_objects()
    armatures = sorted([obj for obj in scene.objects if obj.type == "ARMATURE"], key=object_id)
    # The user-facing rig has the most custom shape controls; meta rigs are
    # generally hidden and are used for construction rather than posing.
    rig = max(armatures, key=lambda obj: (obj.visible_get(), sum(bool(pb.custom_shape) for pb in obj.pose.bones)))
    controls = {pb.name: pb for pb in rig.pose.bones if pb.custom_shape}
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
    if "Tail-SPIK03" in controls:
        preferred.append(("tail tip translation", ("Tail-SPIK03",), "translate"))
    if "Eye.L" in controls:
        preferred.append(("left eye rotation", ("Eye.L",), "rotate"))
        preferred.append(("left eye target translation", ("Eye.L",), "translate_fine"))
    if "Eyelid_Master.T.L" in controls:
        preferred.append(("left upper eyelid rotation", ("Eyelid_Master.T.L",), "rotate"))
        preferred.append(("left upper eyelid translation", ("Eyelid_Master.T.L",), "translate_fine"))
    state = {pb.name: pb.matrix_basis.copy() for pb in rig.pose.bones}
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
        for pb in rig.pose.bones:
            pb.matrix_basis = state[pb.name]
        bpy.context.view_layer.update()

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
        t, q, s = pb.matrix_basis.decompose()
        xyz = list(t) + [math.degrees(v) for v in q.to_euler("XYZ")] + list(s)
        channels = ("tx", "ty", "tz", "rx", "ry", "rz", "sx", "sy", "sz")
        capture(label + ": " + chosen, [{"id": bone_id(rig, chosen), "channel": ch, "value": float(val)}
                                      for ch, val in zip(channels, xyz)])
    reset()
    capture("reset", [])
    metadata["invalid_drivers"] = sum(not d["valid"] for d in driver_inventory)
    (output / "reference.json").write_text(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
