"""Extract a versioned, inert rig snapshot with Blender's own file reader.

blender --background --factory-startup --disable-autoexec --python-exit-code 1
        --python extract.py -- character.blend character.blendrig
"""
import json
import ast
import math
import os
import re
import sys

import bpy
from mathutils import Euler, Matrix, Vector
from mathutils.bvhtree import BVHTree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from materials import Textures, extract as extract_material


def matrix(value):
    # USD multiplies row vectors. Transpose Blender's column-vector matrices.
    return [float(value[c][r]) for r in range(4) for c in range(4)]


def object_id(obj):
    library = bpy.path.abspath(obj.library.filepath) if obj.library else ""
    return json.dumps(["object", library, obj.name], ensure_ascii=False, separators=(",", ":"))


def bone_id(obj, name):
    library = bpy.path.abspath(obj.library.filepath) if obj.library else ""
    return json.dumps(["bone", library, obj.name, name], ensure_ascii=False, separators=(",", ":"))


def material_id(mat):
    library = bpy.path.abspath(mat.library.filepath) if mat.library else ""
    return json.dumps(["material", library, mat.name], ensure_ascii=False, separators=(",", ":"))


def inert(value):
    """Read ID properties without registering or executing source UI scripts."""
    if hasattr(value, "to_dict"):
        return inert(value.to_dict())
    if hasattr(value, "to_list"):
        return inert(value.to_list())
    if isinstance(value, dict):
        return {str(k): inert(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [inert(v) for v in value]
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    return str(value)


def picker_data(obj):
    """Turn Rigify collections and CloudRig's stored panels into native pages.

    Source sidebars are adapted to selection buttons. Settings/operator
    bindings remain inert metadata; this does not translate their behavior.
    """
    if obj.type != "ARMATURE":
        return []
    collections = list(getattr(obj.data, "collections_all", []))

    def members(collection):
        names = {bone.name for bone in collection.bones}
        for child in collection.children:
            names.update(members(child))
        return names

    by_name = {c.name: c for c in collections}
    pages = []
    if obj.data.get("rig_id"):
        for collection in sorted(collections, key=lambda c: c.get("rigify_ui_row", 0)):
            row = collection.get("rigify_ui_row", 0)
            if row <= 0:
                continue
            pages.append({"name": collection.get("rigify_ui_title") or collection.name,
                "source": {"collection": collection.name, "row": int(row),
                           "visible": collection.is_visible},
                "buttons": [{"label": name, "controls": [bone_id(obj, name)]}
                            for name in sorted(members(collection))]})
        kind = "Rigify control collections"
    elif obj.data.get("is_generated_cloudrig"):
        kind = "CloudRig sidebar panels"

        def pairs(value):
            if isinstance(value, dict):
                return {k: pairs(v) for k, v in value.items()}
            if isinstance(value, list) and all(isinstance(v, (tuple, list)) and
                    len(v) == 2 and isinstance(v[0], str) for v in value):
                return {k: pairs(v) for k, v in value}
            return value

        source = inert(obj.data.get("ui_data", {}))
        panels = pairs(source.get("panels", []))
        for label, panel in panels.items():
            buttons = []

            def visit(value, trail=()):
                if not isinstance(value, dict):
                    return
                if "owner_path" in value:
                    path = value["owner_path"]
                    match = re.fullmatch(r'(pose\.bones|data\.collections_all)\[("(?:[^"\\]|\\.)*"|\x27(?:[^\x27\\]|\\.)*\x27)\]', path)
                    targets = []
                    if match:
                        name = ast.literal_eval(match[2])
                        if match[1] == "pose.bones" and name in obj.pose.bones:
                            targets = [bone_id(obj, name)]
                        elif match[1] == "data.collections_all" and name in by_name:
                            targets = [bone_id(obj, n) for n in sorted(members(by_name[name]))]
                    buttons.append({"label": trail[-1] if trail else label,
                        "controls": targets, "source": {"ui_path": list(trail), "binding": value}})
                for key, child in value.items():
                    if isinstance(child, dict):
                        visit(child, trail + (key,))

            visit(panel)
            pages.append({"name": label or "General", "source": panel, "buttons": buttons})
        # Keep every collection, including hidden and nested groups. These are
        # additional selection pages; source sidebar panels stay in order above.
        for collection in collections:
            pages.append({"name": "Controls: " + collection.name,
                "source": {"collection": collection.name,
                           "parent": collection.parent.name if collection.parent else "",
                           "visible": collection.is_visible},
                "buttons": [{"label": name, "controls": [bone_id(obj, name)]}
                            for name in sorted(members(collection))]})
    else:
        return []
    if not pages:
        return []
    text = obj.get("rig_ui") or obj.data.get("cloudrig_ui")
    return [{"owner": object_id(obj), "name": obj.name, "source_kind": kind,
             "source_ui": text.as_string() if isinstance(text, bpy.types.Text) else "",
             "pages": pages}]


def bind_mesh(obj):
    """Materialize constant topology operations on undeformed source data.

    Mirror must precede deformation. A nonsmooth vertex-group mask only
    deletes elements, so it commutes with per-vertex skinning. No pose,
    shape key, lattice or corrective deformation is evaluated here.
    """
    selected = set()
    prefix = True
    for index, mod in enumerate(obj.modifiers):
        if not mod.show_viewport:
            continue
        if mod.type == "MIRROR" and prefix:
            selected.add(index)
        elif mod.type == "MASK" and mod.mode == "VERTEX_GROUP" and not mod.use_smooth:
            selected.add(index)
        else:
            prefix = False
    if not selected:
        return obj.data, set()
    temporary = obj.copy()
    raw = obj.data.copy()
    temporary.data = raw
    temporary.animation_data_clear()
    temporary.shape_key_clear()
    bpy.context.scene.collection.objects.link(temporary)
    try:
        for index, mod in reversed(list(enumerate(temporary.modifiers))):
            if index not in selected:
                temporary.modifiers.remove(mod)
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        mesh = bpy.data.meshes.new_from_object(temporary.evaluated_get(depsgraph),
                                              preserve_all_data_layers=True, depsgraph=depsgraph)
        return mesh, {obj.modifiers[i].name for i in selected}
    finally:
        bpy.data.objects.remove(temporary, do_unlink=True)
        bpy.data.meshes.remove(raw)


def surface_deform_skin(obj, mesh, modifier):
    """Transfer a bound cage's bone weights to a Surface Deform mesh.

    This preserves the cage's rigid/root motion and gives the teeth their
    deforming bone hierarchy. Nearest cage triangles approximate Blender's
    Surface Deform bind weights for local, nonrigid cage motion.
    """
    target = modifier.target
    if not modifier.is_bound or not target or target.type != "MESH" or modifier.strength != 1:
        return None
    active = [m for m in target.modifiers if m.show_viewport]
    if len(active) != 1 or active[0].type != "ARMATURE":
        return None
    armature = active[0]
    if not armature.object or armature.use_bone_envelopes or not armature.use_vertex_groups or armature.vertex_group:
        return None
    cage = target.data
    groups = {group.index: group.name for group in target.vertex_groups}
    weighted = {groups[g.group] for v in cage.vertices for g in v.groups if g.weight > 0 and g.group in groups}
    palette = [bone for bone in armature.object.data.bones if bone.use_deform and bone.name in weighted]
    if not palette or not cage.polygons:
        return None
    indices = {bone.name: i for i, bone in enumerate(palette)}
    cage_weights = []
    for vertex in cage.vertices:
        row = {indices[groups[g.group]]: float(g.weight) for g in vertex.groups
               if g.group in groups and groups[g.group] in indices and g.weight > 0}
        total = sum(row.values())
        if total <= 1e-8:
            return None
        cage_weights.append({i: weight / total for i, weight in row.items()})
    positions = [target.matrix_world @ vertex.co for vertex in cage.vertices]
    triangles = []
    for polygon in cage.polygons:
        corners = list(polygon.vertices)
        triangles.extend((corners[0], corners[i], corners[i + 1])
                         for i in range(1, len(corners) - 1))
    if not triangles:
        return None
    bvh = BVHTree.FromPolygons(positions, triangles, all_triangles=True)
    rows = []
    for vertex in mesh.vertices:
        nearest, _, triangle_index, _ = bvh.find_nearest(obj.matrix_world @ vertex.co)
        if nearest is None:
            return None
        a, b, c = (positions[index] for index in triangles[triangle_index])
        v0, v1, v2 = b - a, c - a, nearest - a
        d00, d01, d11 = v0.dot(v0), v0.dot(v1), v1.dot(v1)
        d20, d21 = v2.dot(v0), v2.dot(v1)
        denominator = d00 * d11 - d01 * d01
        if abs(denominator) < 1e-14:
            return None
        wb = (d11 * d20 - d01 * d21) / denominator
        wc = (d00 * d21 - d01 * d20) / denominator
        weights = {}
        for index, barycentric in zip(triangles[triangle_index], (1 - wb - wc, wb, wc)):
            for bone_index, weight in cage_weights[index].items():
                weights[bone_index] = weights.get(bone_index, 0.0) + barycentric * weight
        total = sum(max(0.0, weight) for weight in weights.values())
        if total <= 1e-8:
            return None
        rows.append([[index, max(0.0, weight) / total] for index, weight in sorted(weights.items())
                     if weight > 1e-8])
    return {"influences": [bone_id(armature.object, bone.name) for bone in palette],
            "weights": rows, "method": "classicLinear", "mask": [], "use_base_input": False,
            "source": {"type": "SURFACE_DEFORM", "target": object_id(target),
                       "approximation": "nearest cage triangle bone weights"}}


def snapshot():
    scene = bpy.context.scene
    bpy.context.view_layer.update()
    diagnostics = []
    dependencies = set()
    nodes = []
    constraints = []
    meshes = []
    materials = []
    pickers = []
    objects = sorted(scene.objects, key=object_id)
    for obj in objects:
        pickers.extend(picker_data(obj))
    object_ids = {object_id(obj) for obj in objects}

    def warn(text):
        diagnostics.append(text)

    def animation(data, label):
        anim = getattr(data, "animation_data", None)
        if anim and (anim.action or anim.drivers or anim.nla_tracks):
            warn("animation/drivers retained at the saved frame; curves and expressions are not translated: " + label)

    def constraint(owner, con):
        target = getattr(con, "target", None)
        subtarget = getattr(con, "subtarget", "")
        item = {"owner": owner, "name": con.name, "type": con.type,
                "enabled": not con.mute, "influence": float(con.influence),
                "source": bone_id(target, subtarget) if target and subtarget else object_id(target) if target else "",
                "owner_space": getattr(con, "owner_space", "WORLD"),
                "target_space": getattr(con, "target_space", "WORLD")}
        space_object = getattr(con, "space_object", None)
        space_bone = getattr(con, "space_subtarget", "")
        if item["owner_space"] == "CUSTOM" or item["target_space"] == "CUSTOM":
            item["custom_space"] = (bone_id(space_object, space_bone) if space_object and
                space_object.type == "ARMATURE" and space_bone else object_id(space_object) if space_object else "")
            if space_object and space_bone and space_object.type != "ARMATURE":
                item["custom_space_unsupported"] = True
                warn("custom constraint vertex-group space is not translated: " + owner + "/" + con.name)
        if con.type == "ARMATURE":
            item["targets"] = [{"source": bone_id(t.target, t.subtarget) if t.target and t.subtarget else "",
                                "weight": float(t.weight)} for t in con.targets]
            active = [t for t in item["targets"] if t["weight"] > 0]
            if len(active) == 1:
                item["source"] = active[0]["source"]
            for key in ("use_deform_preserve_volume", "use_bone_envelopes", "use_current_location"):
                item[key] = getattr(con, key)
        for key in ("use_x", "use_y", "use_z", "invert_x", "invert_y", "invert_z",
                    "use_offset", "mix_mode", "head_tail", "chain_count", "use_tail",
                    "use_stretch", "use_location", "use_rotation", "pole_angle",
                    "use_make_uniform", "use_add", "power", "euler_order", "remove_target_shear",
                    "rest_length", "bulge", "volume", "keep_axis", "track_axis", "use_bbone_shape",
                    "use_bulge_min", "use_bulge_max", "bulge_min", "bulge_max", "bulge_smooth",
                    "use_limit_x", "use_limit_y", "use_limit_z",
                    "y_scale_mode", "xz_scale_mode", "use_curve_radius",
                    "use_even_divisions", "use_chain_offset"):
            if hasattr(con, key):
                item[key] = getattr(con, key)
        pole = getattr(con, "pole_target", None)
        pole_bone = getattr(con, "pole_subtarget", "")
        if pole:
            item["pole"] = bone_id(pole, pole_bone) if pole_bone else object_id(pole)
        constraints.append(item)

    textures = Textures(dependencies)
    shape_objects = {p.custom_shape for o in objects if o.type == "ARMATURE" for p in o.pose.bones if p.custom_shape}

    guide_extents = {}

    def guide_width(pb):
        armature = pb.id_data
        key = object_id(armature)
        if key not in guide_extents:
            bones = [b for b in armature.data.bones if b.use_deform] or list(armature.data.bones)
            points = [p for b in bones for p in (b.head_local, b.tail_local)]
            guide_extents[key] = max((max(p[a] for p in points) - min(p[a] for p in points)
                                     for a in range(3)), default=pb.bone.length)
        # Blender widths are screen pixels; native guide widths are scene units.
        # A thin diameter relative to the character makes Hydra curves pickable
        # without scaling the width by a large root-control shape.
        pixels = float(getattr(pb, "custom_shape_wire_width", 1))
        return max(guide_extents[key], 1e-6) * 0.001 * max(pixels, 0.5)

    def guide(pb):
        shape = pb.custom_shape
        if not shape:
            return {}
        scale = Vector(pb.custom_shape_scale_xyz)
        if pb.use_custom_shape_bone_size:
            scale *= pb.bone.length
        transform = Matrix.LocRotScale(Vector(pb.custom_shape_translation),
                    Euler(pb.custom_shape_rotation_euler, "XYZ").to_quaternion(), scale)
        lines = []
        if shape.type == "MESH":
            lines = [[transform @ shape.data.vertices[i].co for i in edge.vertices]
                     for edge in shape.data.edges]
            if shape.modifiers:
                warn("custom shape modifiers are not evaluated: " + shape.name)
        elif shape.type == "CURVE" and all(s.type == "POLY" for s in shape.data.splines):
            for spline in shape.data.splines:
                line = [transform @ p.co.to_3d() for p in spline.points]
                if spline.use_cyclic_u and line:
                    line.append(line[0])
                lines.append(line)
        else:
            warn("custom shape requires mesh edges or POLY curves: " + shape.name)
        lines = [line for line in lines if len(line) >= 2]
        color=pb.color if pb.color.palette!="DEFAULT" else pb.bone.color
        rgb=list(color.custom.normal) if color.palette=="CUSTOM" else [1.0,0.85,0.2]
        if color.palette.startswith("THEME"):
            rgb=list(bpy.context.preferences.themes[0].bone_color_sets[int(color.palette[5:])-1].normal)
        return {"guide_points": [list(p) for line in lines for p in line],
                "guide_counts": [len(line) for line in lines],
                "guide_source": bone_id(pb.id_data, pb.custom_shape_transform.name) if pb.custom_shape_transform else "",
                "guide_wire_width": guide_width(pb), "source_wire_width_pixels": float(getattr(pb, "custom_shape_wire_width", 1)),
                "guide_color": rgb}

    for obj in objects:
        oid = object_id(obj)
        animation(obj, obj.name)
        animation(obj.data, obj.name + " data")
        if obj.instance_type != "NONE":
            warn("object/collection instances are not translated: " + obj.name)
        if obj.library:
            dependencies.add(bpy.path.abspath(obj.library.filepath))
        parent = object_id(obj.parent) if obj.parent else ""
        local = obj.matrix_parent_inverse.copy()
        t, q, s = obj.matrix_basis.decompose()
        pose = list(t) + [math.degrees(v) for v in q.to_euler("XYZ")] + list(s)
        object_basis = True
        if obj.parent and obj.parent_type == "BONE" and obj.parent.type == "ARMATURE" and obj.parent_bone in obj.parent.data.bones:
            bone = obj.parent.data.bones[obj.parent_bone]
            parent = bone_id(obj.parent, bone.name)
            # Ordinary bone parenting uses its tail; relative parenting uses
            # the rest-to-pose map instead of the absolute bone frame.
            offset = bone.matrix_local.inverted() if bone.use_relative_parent else Matrix.Translation((0, bone.length, 0))
            local = offset @ local
        elif obj.parent and obj.parent_type != "OBJECT":
            warn("vertex/invalid bone parenting saved as independent world placement: " + obj.name)
            local, parent = obj.matrix_world.copy(), ""
            pose, object_basis = [0, 0, 0, 0, 0, 0, 1, 1, 1], False
        if obj.parent and object_id(obj.parent) not in object_ids:
            warn("parent outside active scene saved as independent world placement: " + obj.name)
            local, parent = obj.matrix_world.copy(), ""
            pose, object_basis = [0, 0, 0, 0, 0, 0, 1, 1, 1], False
        node = {"id": oid, "name": obj.name, "kind": "control", "parent": parent,
                "rest": matrix(local), "pose": pose, "pose_is_object_basis": object_basis,
                "visible": obj.visible_get(), "render_visible": not obj.hide_render, "object_type": obj.type}
        if obj.type == "CURVE" and len(obj.data.splines) == 1:
            spline = obj.data.splines[0]
            hooks = [mod for mod in obj.modifiers if mod.show_viewport and mod.type == "HOOK"]
            if spline.type == "BEZIER" and len(spline.bezier_points) == 3 and len(hooks) == 3:
                ordered = sorted(hooks, key=lambda mod: min(mod.vertex_indices, default=-1))
                if all(mod.object and mod.object.type == "ARMATURE" and mod.subtarget in mod.object.data.bones
                       and sorted(mod.vertex_indices) == list(range(3 * i, 3 * i + 3))
                       for i, mod in enumerate(ordered)):
                    node["spline_hooks"] = [bone_id(mod.object, mod.subtarget) for mod in ordered]
                    node["spline_radii"] = [float(point.radius) for point in spline.bezier_points]
        nodes.append(node)
        for con in obj.constraints:
            constraint(oid, con)
        if obj.type == "ARMATURE":
            if obj.data.pose_position != "POSE":
                warn("armature displayed in REST mode; native rig exports its saved pose channels: " + obj.name)
            for bone in obj.data.bones:
                pb = obj.pose.bones[bone.name]
                local = bone.parent.matrix_local.inverted() @ bone.matrix_local if bone.parent else bone.matrix_local
                t, q, s = pb.matrix_basis.decompose()
                # Preserve source channels where they already use XYZ. Matrix
                # decomposition adds roundoff to scale/Eulers, amplified by
                # nearly antiparallel tracking constraints.
                t, s = pb.location.copy(), pb.scale.copy()
                r = pb.rotation_euler.copy() if pb.rotation_mode == "XYZ" else q.to_euler("XYZ")
                node = {"id": bone_id(obj, bone.name), "name": bone.name, "kind": "joint",
                        "parent": bone_id(obj, bone.parent.name) if bone.parent else oid,
                        "rest": matrix(local), "pose": list(t) + [math.degrees(v) for v in r] + list(s),
                        "length": float(bone.length), "deform": bone.use_deform,
                        "visible": obj.visible_get() and not bone.hide and (not bone.collections or any(c.is_visible_effectively for c in bone.collections))}
                node["bbone_segments"] = bone.bbone_segments
                node["ik_stretch"] = float(pb.ik_stretch)
                node["armature"] = oid
                node.update(inherit_scale=bone.inherit_scale, inherit_rotation=bone.use_inherit_rotation,
                            local_location=bone.use_local_location, connected=bone.use_connect)
                node.update(guide(pb))
                nodes.append(node)
                if bone.bbone_segments != 1:
                    warn("B-Bone segment deformation is not translated: " + obj.name + "/" + bone.name)
                for con in pb.constraints:
                    constraint(node["id"], con)
        elif obj.type not in {"MESH", "EMPTY"}:
            warn("object data is not translated: " + obj.name + " (" + obj.type + ")")

        if obj.type != "MESH" or obj in shape_objects or (not obj.visible_get() and not obj.data.polygons):
            continue
        mesh = obj.data
        if mesh.shape_keys:
            warn("shape keys are not translated: " + obj.name)
        mesh, prepared = bind_mesh(obj)
        active_modifiers = [mod for mod in obj.modifiers if mod.show_viewport]
        armatures = [mod for mod in active_modifiers if mod.type == "ARMATURE" and mod.object]
        surface_skin = None
        if not armatures:
            surfaces = [mod for mod in active_modifiers if mod.type == "SURFACE_DEFORM"]
            if len(surfaces) == 1 and active_modifiers[0] == surfaces[0]:
                surface_skin = surface_deform_skin(obj, mesh, surfaces[0])
        for mod in active_modifiers:
            if mod.name in prepared:
                # Parameters stay in sourceData; topology is the bind mesh.
                continue
            if mod.type == "SUBSURF" and mod.levels == 0:
                continue
            if mod.type == "SURFACE_DEFORM" and surface_skin is not None:
                warn("Surface Deform uses approximate cage bone weights: " + obj.name + "/" + mod.name)
            elif mod.type != "ARMATURE":
                warn("modifier is not translated: " + obj.name + "/" + mod.name + " (" + mod.type + ")")
            elif not mod.object:
                warn("armature modifier has no object: " + obj.name + "/" + mod.name)
        skins = []
        for mod in armatures:
            if mod.use_bone_envelopes or not mod.use_vertex_groups:
                warn("armature envelopes or disabled vertex groups are not translated: " + obj.name + "/" + mod.name)
                continue
            else:
                group_names = {group.index: group.name for group in obj.vertex_groups}
                weighted_names = {group_names[g.group] for vertex in mesh.vertices for g in vertex.groups if g.weight > 0}
                palette = [bone for bone in mod.object.data.bones if bone.use_deform and bone.name in weighted_names]
                if not palette:
                    continue
                indices = {bone.name: i for i, bone in enumerate(palette)}
                rows = []
                for vertex in mesh.vertices:
                    row = [[indices[group_names[g.group]], float(g.weight)] for g in vertex.groups
                           if group_names[g.group] in indices and g.weight > 0]
                    total = sum(w for _, w in row)
                    rows.append([[i, w / total] for i, w in row] if total > 0.0001 else [])
                skin = {"influences": [bone_id(mod.object, bone.name) for bone in palette], "weights": rows,
                        "method": "dualQuaternion" if mod.use_deform_preserve_volume else "classicLinear"}
                mask = obj.vertex_groups.get(mod.vertex_group)
                if mask:
                    skin["mask"] = []
                    for vertex in mesh.vertices:
                        value = next((float(g.weight) for g in vertex.groups if g.group == mask.index), 0)
                        skin["mask"].append(1 - value if mod.invert_vertex_group else value)
                else:
                    # Blender ignores an absent mask group, including inversion.
                    skin["mask"] = []
                previous = active_modifiers.index(mod) - 1
                skin["use_base_input"] = bool(mod.use_multi_modifier and previous >= 0 and active_modifiers[previous].type == "ARMATURE")
                if skin["use_base_input"] and any(m.type != "ARMATURE" for m in active_modifiers[:previous]):
                    warn("multi-armature cache after other modifiers is not translated: " + obj.name + "/" + mod.name)
                skins.append(skin)
                if mod.use_deform_preserve_volume:
                    warn("preserve-volume skinning mapped to native DQ; Blender scale and bend parity is not established: " + obj.name)
        if surface_skin is not None:
            skins.append(surface_skin)
        uv = []
        render_uv = next((layer for layer in mesh.uv_layers if layer.active_render), mesh.uv_layers.active)
        if render_uv:
            uv = [list(v.uv) for v in render_uv.data]
        uv_sets = {layer.name: [list(v.uv) for v in layer.data] for layer in mesh.uv_layers}
        if mesh.has_custom_normals:
            warn("custom split normals are not translated: " + obj.name)
        meshes.append({"id": oid, "points": [list(v.co) for v in mesh.vertices],
                       "counts": [len(p.vertices) for p in mesh.polygons],
                       "indices": [i for p in mesh.polygons for i in p.vertices], "uv": uv, "uv_sets": uv_sets,
                       "uv_active": mesh.uv_layers.active.name if mesh.uv_layers.active else "",
                       "uv_render": render_uv.name if render_uv else "",
                       "smooth": [p.use_smooth for p in mesh.polygons],
                       "materials": [material_id(slot.material) if slot.material else "" for slot in obj.material_slots],
                       "material_indices": [p.material_index for p in mesh.polygons],
                       "bind_topology": [{"name": mod.name, "type": mod.type} for mod in active_modifiers if mod.name in prepared],
                       "skin": skins[0] if len(skins) == 1 else None, "skin_stack": skins})
        if mesh != obj.data:
            bpy.data.meshes.remove(mesh)

    for mat in sorted({slot.material for obj in objects if obj.type == "MESH"
                       for slot in obj.material_slots if slot.material}, key=lambda m: m.name_full):
        item = {"id": material_id(mat), "name": mat.name_full}
        animation(mat, mat.name_full)
        if mat.use_nodes:
            animation(mat.node_tree, mat.name_full + " nodes")
        item.update(extract_material(mat, textures, warn))
        materials.append(item)
    for library in bpy.data.libraries:
        dependencies.add(bpy.path.abspath(library.filepath))
    for path in dependencies:
        if not os.path.isfile(path):
            warn("unresolved asset dependency: " + path)
    animation(scene, "scene")
    if bpy.data.texts:
        warn("embedded text blocks are not executed or translated")
    return {"format": "usdBlenderRig", "version": 1, "blender_version": bpy.app.version_string,
            "source": bpy.data.filepath, "meters_per_unit": float(scene.unit_settings.scale_length),
            "fps": float(scene.render.fps / scene.render.fps_base), "frame": scene.frame_current,
            "start": scene.frame_start, "end": scene.frame_end, "nodes": nodes,
            "constraints": constraints, "meshes": meshes, "materials": materials,
            "pickers": pickers,
            "dependencies": sorted(dependencies), "diagnostics": sorted(set(diagnostics))}


def main():
    args = sys.argv[sys.argv.index("--") + 1:]
    if len(args) != 2:
        raise ValueError("expected input.blend output.blendrig")
    source, output = map(os.path.abspath, args)
    bpy.ops.wm.open_mainfile(filepath=source, load_ui=False, use_scripts=False)
    with open(output, "w", encoding="utf-8") as stream:
        json.dump(snapshot(), stream, ensure_ascii=False, allow_nan=False, separators=(",", ":"))


if __name__ == "__main__":
    main()
