"""Create real Blender input and independent numerical reference poses."""
import json
import math
from pathlib import Path
import sys

import bpy
from mathutils import Euler, Matrix, Vector

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "blender"))
from extract import bone_id, object_id, snapshot


def main():
    output = Path(sys.argv[sys.argv.index("--") + 1]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    scene = bpy.context.scene
    scene.unit_settings.scale_length = 0.01
    scene.render.fps = 30

    def empty(name, location):
        obj = bpy.data.objects.new(name, None)
        scene.collection.objects.link(obj)
        obj.location = location
        return obj

    master = empty("Master", (3, -2, 1))
    master.rotation_euler.z = 0.35
    armature = bpy.data.armatures.new("Skeleton")
    rig = bpy.data.objects.new("Armature", armature)
    scene.collection.objects.link(rig)
    rig.parent = master
    rig.location = (0.5, 0.25, 0)
    bpy.context.view_layer.objects.active = rig
    rig.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    root = armature.edit_bones.new("Root")
    root.head, root.tail, root.roll = (0, 0, 0), (0, 1, 0), 0.25
    tip = armature.edit_bones.new("Tip.L")
    tip.head, tip.tail, tip.roll = (0, 1, 0), (0.4, 2, 0.2), -0.4
    tip.parent = root
    switching = armature.edit_bones.new("Parent switch")
    switching.head, switching.tail, switching.roll = (-0.4, 0.3, 0.2), (-0.4, 1.1, 0.2), -0.2
    switching.use_deform = False
    inheritance = []
    for inherit_rotation in (True, False):
        for mode in ("FULL", "NONE", "AVERAGE", "ALIGNED", "FIX_SHEAR", "NONE_LEGACY"):
            bone = armature.edit_bones.new("Inheritance " + mode + str(inherit_rotation))
            bone.head, bone.tail, bone.roll = (0.2, 0.5, 0.1), (0.35, 1.1, 0.4), 0.4
            bone.parent, bone.use_deform = root, False
            bone.inherit_scale, bone.use_inherit_rotation = mode, inherit_rotation
            inheritance.append(bone.name)
    for parent in (None, root):
        bone = armature.edit_bones.new("Nonlocal " + str(parent is not None))
        bone.head, bone.tail, bone.roll = (-0.3, 0.2, 0.1), (-0.4, 0.8, 0.5), -0.4
        bone.parent, bone.use_deform, bone.use_local_location = parent, False, False
        inheritance.append(bone.name)
    connected = armature.edit_bones.new("Connected")
    connected.head, connected.tail, connected.parent = (0, 1, 0), (0.1, 1.9, 0.2), root
    connected.use_connect, connected.use_deform = True, False
    inheritance.append(connected.name)
    stretch_probes = []
    for keep in ("PLANE_X", "PLANE_Z", "SWING_Y"):
        for volume in ("NO_VOLUME", "VOLUME_XZX", "VOLUME_X", "VOLUME_Z"):
            bone = armature.edit_bones.new("Stretch " + keep + volume)
            bone.head, bone.tail, bone.roll = (-0.3, 0.4, 0.2), (-0.2, 1.2, 0.4), 0.25
            bone.parent, bone.use_deform = root, False
            connected_stack = keep == "PLANE_X" and volume == "NO_VOLUME"
            if connected_stack:
                bone.head, bone.use_connect = (0, 1, 0), True
            stretch_probes.append((bone.name, keep, volume, connected_stack))
            inheritance.append(bone.name)
    track_probes = []
    for axis in ("TRACK_X", "TRACK_Y", "TRACK_Z", "TRACK_NEGATIVE_X", "TRACK_NEGATIVE_Y", "TRACK_NEGATIVE_Z"):
        bone = armature.edit_bones.new("Track " + axis)
        bone.head, bone.tail, bone.roll = (-0.1, 0.3, 0.2), (0.1, 1.1, 0.4), -0.3
        bone.parent, bone.use_deform = root, False
        track_probes.append((bone.name, axis))
        inheritance.append(bone.name)
    scale_probes = []
    for label, axes, uniform, offset, add in (
            ("masked", (True, False, True), False, False, False),
            ("all", (True, True, True), False, False, False),
            ("uniform", (True, True, True), True, False, False),
            ("uniform masked", (True, False, False), True, False, False),
            ("multiply", (True, True, True), False, True, False),
            ("add", (True, True, True), False, True, True)):
        bone = armature.edit_bones.new("Scale " + label)
        bone.head, bone.tail, bone.roll = (-0.2, 0.4, 0.1), (0.1, 1.0, 0.3), 0.3
        bone.parent, bone.use_deform = root, False
        scale_probes.append((bone.name, axes, uniform, offset, add))
        inheritance.append(bone.name)
    bone = armature.edit_bones.new("Shear cleanup")
    bone.head, bone.tail, bone.parent, bone.use_deform = (-0.2, 0.2, 0.3), (0.1, 1.0, 0.3), root, False
    inheritance.append(bone.name)
    for name in ("Half target", "Half parent", "Half stretch"):
        bone = armature.edit_bones.new(name)
        bone.head, bone.tail, bone.roll = (-0.2, 0.4, 0.1), (0.1, 1.0, 0.3), 0.3
        bone.parent, bone.use_deform = root, False
        inheritance.append(bone.name)
    for name in ("Partial stretch", "Partial track", "Partial armature", "Partial cleanup"):
        bone = armature.edit_bones.new(name)
        bone.head, bone.tail, bone.roll = (-0.2, 0.4, 0.1), (0.1, 1.0, 0.3), 0.3
        bone.parent, bone.use_deform = root, False
        inheritance.append(bone.name)
    parent = armature.edit_bones["Half parent"]
    parent.head, parent.tail = (0, 0, 0), (0, 0.5, 0)
    child = armature.edit_bones["Half stretch"]
    child.head, child.tail, child.parent, child.use_connect = (0, 0.5, 0), (0, 1, 0), parent, True
    space_probes = []
    for kind in ("COPY_LOCATION", "COPY_ROTATION", "COPY_SCALE", "COPY_TRANSFORMS"):
        for owner_space in ("WORLD", "POSE", "LOCAL", "CUSTOM"):
            for target_space in ("WORLD", "POSE", "LOCAL", "LOCAL_OWNER_ORIENT", "CUSTOM"):
                for influence in (1.0, 0.4):
                    bone = armature.edit_bones.new("Spaces " + kind + owner_space + target_space + str(influence))
                    bone.head, bone.tail, bone.roll = (-0.2, 0.4, 0.1), (0.1, 1.0, 0.3), 0.3
                    bone.parent, bone.use_deform = root, False
                    space_probes.append((bone.name, kind, owner_space, target_space, influence))
                    inheritance.append(bone.name)
    rotation_mixes = []
    for mix in ("BEFORE", "AFTER"):
        for space in ("WORLD", "LOCAL"):
            for influence in (1.0, 0.4):
                for enabled in (True, False):
                    bone = armature.edit_bones.new(f"Mix {mix}{space}{influence}{enabled}")
                    bone.head, bone.tail, bone.roll = (-0.2, 0.4, 0.1), (0.1, 1.0, 0.3), 0.3
                    bone.parent, bone.use_deform = root, False
                    inheritance.append(bone.name)
                    rotation_mixes.append((bone.name, mix, space, influence, enabled))
    transform_mixes=[]
    for mix in ("BEFORE_FULL","AFTER_FULL","BEFORE","AFTER","BEFORE_SPLIT","AFTER_SPLIT"):
        for space in ("WORLD","LOCAL"):
            for influence in (1.0,0.4):
                name=f"Transforms {mix} {space} {influence}"
                bone=armature.edit_bones.new(name)
                bone.head,bone.tail,bone.roll=(0.5,0.1,0.4),(0.6,0.8,0.7),0.25
                bone.parent,bone.use_deform=root,False
                inheritance.append(name);transform_mixes.append((name,mix,space,influence))
    armature_probes=[]
    for dual in (False,True):
        for current in (False,True):
            name=f"Armature blend {dual} {current}"
            bone=armature.edit_bones.new(name)
            bone.head,bone.tail,bone.roll=(0.5,0.1,0.4),(0.6,0.8,0.7),0.25
            bone.use_deform=False
            inheritance.append(name);armature_probes.append((name,dual,current))
    bpy.ops.object.mode_set(mode="OBJECT")
    armature["rig_id"] = "native_picker_fixture"
    for label, row, names in (("Main", 1, ("Root",)), ("Hand", 2, ("Tip.L",))):
        collection = armature.collections.new(label)
        collection["rigify_ui_row"] = row
        for name in names:
            collection.assign(armature.bones[name])
        collection.is_visible = label != "Hand"
    # A differently placed/scaled armature distinguishes POSE from WORLD,
    # including cross-space constraints and live armature object placement.
    source_data = bpy.data.armatures.new("Pose source")
    source_rig = bpy.data.objects.new("Pose source", source_data)
    scene.collection.objects.link(source_rig)
    source_rig.parent, source_rig.location = master, (-0.2, 0.9, 0.5)
    source_rig.rotation_euler, source_rig.scale = (0.4, -0.2, 0.1), (1.2, 0.7, 1.6)
    rig.select_set(False)
    source_rig.select_set(True)
    bpy.context.view_layer.objects.active = source_rig
    bpy.ops.object.mode_set(mode="EDIT")
    source_parent = source_data.edit_bones.new("Parent")
    source_parent.head, source_parent.tail, source_parent.roll = (-0.1, 0, 0), (0.1, 0.8, 0.2), -0.25
    source_bone = source_data.edit_bones.new("Target")
    source_bone.head, source_bone.tail, source_bone.roll = (0.1, 0.2, 0.3), (-0.3, 1.0, 0.5), 0.2
    source_bone.parent = source_parent
    bpy.ops.object.mode_set(mode="OBJECT")
    parent_pb = source_rig.pose.bones["Parent"]
    parent_pb.rotation_mode = "XYZ"
    parent_pb.location, parent_pb.rotation_euler, parent_pb.scale = (-0.1, 0.2, 0.1), (-0.1, 0.3, 0.2), (1.3, 0.8, 1.1)
    source_data["is_generated_cloudrig"] = True
    source_collection = source_data.collections.new("Controls")
    source_child = source_data.collections.new("Nested", parent=source_collection)
    source_child.assign(source_data.bones["Target"])
    source_child.is_visible = False
    cloud_panels = {
        "Settings": {"parent_id": "", "": {"Row": {"Target": {
            "owner_path": 'pose.bones["Target"]', "prop_name": '["switch"]',
            "children": {"True": {"": {"Child row": {"Nested target": {
                "owner_path": 'pose.bones["Parent"]', "prop_name": '["other"]'
            }}}}}
        }}}},
        "Groups": {"": {"Row": {"All": {
            "owner_path": 'data.collections_all["Controls"]', "prop_name": "is_visible"
        }}}}
    }
    def stored_pairs(value):
        return [[key, stored_pairs(child)] for key, child in value.items()] if isinstance(value, dict) else value
    source_data["ui_data"] = {"panels": stored_pairs(cloud_panels)}
    source_pb = source_rig.pose.bones["Target"]
    source_pb.rotation_mode = "XYZ"
    source_pb.location, source_pb.rotation_euler, source_pb.scale = (0.2, -0.1, 0.15), (0.25, 0.3, -0.2), (0.8, 1.2, 1.1)
    source_rig.select_set(False)
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    rig.pose.bones["Root"].rotation_mode = "XYZ"
    rig.pose.bones["Root"].rotation_euler = (0.2, -0.15, 0.3)
    driver = empty("Driver", (0.5, 1.25, 0.1))
    driver.parent = master
    stretch_target = empty("Stretch target", (1.3, 1.9, 0.8))
    stretch_target.parent = master
    stretch_target.scale = (1.7, 0.6, 1.3)
    con = rig.pose.bones["Tip.L"].constraints.new("COPY_LOCATION")
    con.target = driver
    con.owner_space = con.target_space = "WORLD"
    con = rig.pose.bones["Parent switch"].constraints.new("ARMATURE")
    target = con.targets.new()
    target.target, target.subtarget, target.weight = rig, "Root", 1
    for name,mix,space,influence in transform_mixes:
        pb=rig.pose.bones[name];pb.rotation_mode="XYZ"
        pb.location,pb.rotation_euler,pb.scale=(0.2,-0.15,0.1),(0.3,-0.2,0.4),(0.8,1.2,1.1)
        con=pb.constraints.new("COPY_TRANSFORMS")
        con.target,con.subtarget,con.mix_mode=source_rig,"Target",mix
        con.owner_space,con.target_space,con.influence=space,space,influence
    for name,dual,current in armature_probes:
        pb=rig.pose.bones[name];pb.location=(0.3,-0.2,0.1)
        con=pb.constraints.new("ARMATURE")
        con.use_deform_preserve_volume,con.use_current_location=dual,current
        for obj,bone,weight in ((rig,"Root",0.7),(source_rig,"Target",0.3)):
            target=con.targets.new();target.target,target.subtarget,target.weight=obj,bone,weight
        pb.constraints.new("LIMIT_ROTATION")
    rig.pose.bones["Parent switch"].location.x = 0.1
    con = rig.pose.bones["Connected"].constraints.new("COPY_TRANSFORMS")
    con.target, con.subtarget = rig, "Tip.L"
    for name, keep, volume, connected_stack in stretch_probes:
        if connected_stack:
            con = rig.pose.bones[name].constraints.new("COPY_TRANSFORMS")
            con.target, con.subtarget = rig, "Tip.L"
        con = rig.pose.bones[name].constraints.new("STRETCH_TO")
        con.target, con.keep_axis, con.volume, con.rest_length = stretch_target, keep, volume, 0.8
        con.use_bulge_min = con.use_bulge_max = True
        con.bulge_min, con.bulge_max, con.bulge_smooth = 0.7, 1.25, 0.6
    for name, axis in track_probes:
        con = rig.pose.bones[name].constraints.new("DAMPED_TRACK")
        con.target, con.track_axis = stretch_target, axis
    for name, axes, uniform, offset, add in scale_probes:
        con = rig.pose.bones[name].constraints.new("COPY_SCALE")
        con.target, con.power = stretch_target, 1.4
        con.use_x, con.use_y, con.use_z = axes
        con.use_make_uniform, con.use_offset, con.use_add = uniform, offset, add
    con = rig.pose.bones["Shear cleanup"].constraints.new("COPY_TRANSFORMS")
    con.target, con.subtarget = rig, "Tip.L"
    rig.pose.bones["Shear cleanup"].constraints.new("LIMIT_ROTATION")
    for name in ("Half target", "Half parent", "Half stretch"):
        con = rig.pose.bones[name].constraints.new("COPY_TRANSFORMS")
        con.target, con.subtarget = rig, "Half target" if name == "Half stretch" else "Root"
    con = rig.pose.bones["Half target"].constraints.new("COPY_LOCATION")
    con.target, con.influence = driver, 0.5
    con = rig.pose.bones["Half parent"].constraints.new("STRETCH_TO")
    con.target, con.subtarget, con.rest_length, con.volume, con.keep_axis = rig, "Half target", 0.5, "NO_VOLUME", "SWING_Y"
    con = rig.pose.bones["Half stretch"].constraints.new("STRETCH_TO")
    con.target, con.rest_length, con.volume, con.keep_axis = driver, 0.5, "NO_VOLUME", "SWING_Y"
    con = rig.pose.bones["Partial stretch"].constraints.new("STRETCH_TO")
    con.target, con.rest_length, con.volume, con.keep_axis = stretch_target, 0.8, "VOLUME_Z", "PLANE_X"
    con.influence = 0.4
    con = rig.pose.bones["Partial track"].constraints.new("DAMPED_TRACK")
    con.target, con.track_axis, con.influence = stretch_target, "TRACK_NEGATIVE_Z", 0.4
    con = rig.pose.bones["Partial armature"].constraints.new("ARMATURE")
    target = con.targets.new()
    target.target, target.subtarget, target.weight = rig, "Root", 1
    con.influence = 0.4
    con = rig.pose.bones["Partial cleanup"].constraints.new("COPY_TRANSFORMS")
    con.target, con.subtarget = rig, "Tip.L"
    con = rig.pose.bones["Partial cleanup"].constraints.new("LIMIT_ROTATION")
    con.influence = 0.4
    for name, kind, owner_space, target_space, influence in space_probes:
        con = rig.pose.bones[name].constraints.new(kind)
        con.target, con.subtarget = source_rig, "Target"
        con.owner_space, con.target_space = owner_space, target_space
        con.influence = influence
        if "CUSTOM" in (owner_space, target_space):
            con.space_object = rig if influence == 1 else master
            con.space_subtarget = "Root" if influence == 1 else ""
        if kind == "COPY_LOCATION":
            con.use_y, con.invert_z, con.use_offset, con.head_tail = False, True, True, 0.4
        elif kind == "COPY_SCALE":
            con.use_y, con.use_offset, con.power = False, True, 1.1
    for name, mix, space, influence, enabled in rotation_mixes:
        con = rig.pose.bones[name].constraints.new("COPY_ROTATION")
        con.target, con.subtarget = source_rig, "Target"
        con.owner_space, con.target_space = space, "WORLD" if space == "WORLD" else "LOCAL_OWNER_ORIENT"
        con.mix_mode, con.influence = mix, influence
        con.use_x = con.use_y = con.use_z = enabled
    for name in inheritance:
        pb = rig.pose.bones[name]
        pb.location = (0.1, -0.15, 0.2)
        pb.rotation_mode = "XYZ"
        pb.rotation_euler = (0.2, -0.1, 0.3)
        pb.scale = (0.9, 1.1, 1.2)

    data = bpy.data.meshes.new("Body")
    data.from_pydata([(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (0.5, 1.5, 0.25)],
                     [], [(0, 1, 2, 3), (3, 2, 4)])
    mesh = bpy.data.objects.new("Body", data)
    scene.collection.objects.link(mesh)
    mesh.parent = master
    mesh.location = (0.5, 0.25, 0)
    # This object placement differs from the armature in orientation, proving
    # the mesh-object / armature-object bind conversion rather than identity.
    mesh.rotation_euler.z = -0.12
    root_group, tip_group = mesh.vertex_groups.new(name="Root"), mesh.vertex_groups.new(name="Tip.L")
    root_group.add([0, 1], 1, "REPLACE")
    tip_group.add([2], 1, "REPLACE")
    root_group.add([3], 0.25, "REPLACE")
    tip_group.add([3], 0.75, "REPLACE")
    # Point 4 has no weight and must stay at its undeformed object placement.
    mod = mesh.modifiers.new("Skin", "ARMATURE")
    mod.object, mod.use_bone_envelopes = rig, False
    uv = data.uv_layers.new(name="UVMap")
    for loop in data.loops:
        co = data.vertices[loop.vertex_index].co
        uv.data[loop.index].uv = (co.x, co.y)
    editing_uv = data.uv_layers.new(name="EditingUV")
    for item in editing_uv.data:
        item.uv = (0.375, 0.625)
    data.uv_layers.active = editing_uv
    uv.active_render = True
    mat = bpy.data.materials.new("Blue")
    mat.diffuse_color = (0.1, 0.3, 0.8, 1)
    data.materials.append(mat)
    mat2 = bpy.data.materials.new("Red")
    mat2.diffuse_color = (0.8, 0.1, 0.1, 1)
    data.materials.append(mat2)
    data.polygons[1].material_index = 1

    test_meshes = [mesh]
    multi = bpy.data.objects.new("Masked chain", data.copy())
    scene.collection.objects.link(multi)
    multi.parent, multi.location = master, mesh.location
    multi.rotation_euler = mesh.rotation_euler
    for name in ("Root", "Tip.L"):
        group = multi.vertex_groups.new(name=name)
        for vertex in data.vertices:
            for membership in vertex.groups:
                if mesh.vertex_groups[membership.group].name == name:
                    group.add([vertex.index], membership.weight, "REPLACE")
    mask = multi.vertex_groups.new(name="Mask")
    for i, weight in enumerate((0, 0.2, 0.5, 0.8, 1)):
        mask.add([i], weight, "REPLACE")
    for invert in (True, False):
        skin = multi.modifiers.new("Armature", "ARMATURE")
        skin.object, skin.use_bone_envelopes = rig, False
        skin.vertex_group, skin.invert_vertex_group = "Mask", invert
        skin.use_multi_modifier = True
    test_meshes.append(multi)
    topology = bpy.data.objects.new("Mirror and mask", data.copy())
    scene.collection.objects.link(topology)
    topology.parent, topology.location = master, mesh.location
    group = topology.vertex_groups.new(name="Root")
    group.add(list(range(len(data.vertices))), 1, "REPLACE")
    group = topology.vertex_groups.new(name="Visible")
    group.add([0, 1, 2, 3], 1, "REPLACE")
    mirror = topology.modifiers.new("Mirror", "MIRROR")
    mirror.use_mirror_merge = False
    mirror.use_mirror_vertex_groups = False
    skin = topology.modifiers.new("Skin", "ARMATURE")
    skin.object, skin.use_bone_envelopes = rig, False
    mask_mod = topology.modifiers.new("Mask", "MASK")
    mask_mod.vertex_group = "Visible"
    test_meshes.append(topology)
    for relative in (False, True):
        # Independent bones are needed: relative-parenting is a bone flag.
        bone_name = "Tip.L" if relative else "Root"
        armature.bones[bone_name].use_relative_parent = relative
        child = bpy.data.objects.new("Relative parent" if relative else "Tail parent", data.copy())
        scene.collection.objects.link(child)
        child.parent, child.parent_type, child.parent_bone = rig, "BONE", bone_name
        child.location = (0.2, 0.1, -0.1)
        child.rotation_euler = (0.1, -0.2, 0.15)
        test_meshes.append(child)
    child = bpy.data.objects.new("Armature parent", data.copy())
    scene.collection.objects.link(child)
    child.parent, child.parent_type, child.parent_bone = rig, "BONE", "Parent switch"
    child.location = (0.1, -0.1, 0.15)
    test_meshes.append(child)
    for name in inheritance:
        child = bpy.data.objects.new("Probe " + name, data.copy())
        scene.collection.objects.link(child)
        child.parent, child.parent_type, child.parent_bone = rig, "BONE", name
        child.location = (0.1, 0.1, -0.1)
        test_meshes.append(child)

    shape_data = bpy.data.meshes.new("Guide")
    shape_data.from_pydata([(-0.2, 0, 0), (0.2, 0, 0), (0, 0, 0.2)], [(0, 1), (1, 2), (2, 0)], [])
    shape = bpy.data.objects.new("Guide", shape_data)
    rig.pose.bones["Root"].custom_shape = shape
    bpy.context.view_layer.update()
    source = output / "rig.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(source), compress=True)
    with (output / "rig.blendrig").open("w") as stream:
        json.dump(snapshot(), stream, allow_nan=False)

    def points(obj=mesh):
        bpy.context.view_layer.update()
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        return [list(evaluated.matrix_world @ vertex.co) for vertex in evaluated.data.vertices]

    def sample(name, edits):
        return {"name": name, "edits": edits, "points": points(),
                "meshes": [{"mesh": object_id(obj), "points": points(obj)} for obj in test_meshes]}

    saved_driver = driver.location.copy()
    saved_rotation = rig.pose.bones["Root"].rotation_euler.copy()
    poses = [sample("saved", [])]
    driver.location.x += 0.7
    poses.append(sample("control translation", [{"id": object_id(driver), "channel": "tx", "value": float(driver.location.x)}]))
    driver.location = saved_driver
    rig.pose.bones["Root"].rotation_euler.x += 0.4
    poses.append(sample("bone rotation", [{"id": bone_id(rig, "Root"), "channel": "rx",
                   "value": math.degrees(rig.pose.bones["Root"].rotation_euler.x)}]))
    rig.pose.bones["Root"].rotation_euler = saved_rotation
    rig.pose.bones["Root"].scale = (1.3, 0.8, 1.1)
    poses.append(sample("bone scale", [{"id": bone_id(rig, "Root"), "channel": axis, "value": val}
                   for axis, val in zip(("sx", "sy", "sz"), (1.3, 0.8, 1.1))]))
    rig.pose.bones["Root"].scale = (1, 1, 1)
    rig.pose.bones["Parent switch"].location.x = 0.35
    poses.append(sample("armature owner translation", [{"id": bone_id(rig, "Parent switch"), "channel": "tx", "value": 0.35}]))
    rig.pose.bones["Parent switch"].location.x = 0.1
    master.location.x += 0.8
    poses.append(sample("armature object translation", [{"id": object_id(master), "channel": "tx", "value": float(master.location.x)}]))
    master.location.x -= 0.8
    poses.append(sample("reset", []))
    # Deliberately collapsed stretches are unsupported by invertible RigExec
    # frames. Verify the diagnosed native guard remains finite and recovers;
    # this is a runtime guard test, not a Blender-equivalence measurement.
    head = rig.matrix_world @ rig.pose.bones["Stretch PLANE_XVOLUME_XZX"].head
    zero_target = master.matrix_world.inverted() @ head
    guards = [{"id": object_id(stretch_target), "channel": ch, "value": float(val)}
              for ch, val in zip(("tx", "ty", "tz"), zero_target)]
    with (output / "reference.json").open("w") as stream:
        json.dump({"mesh": object_id(mesh), "poses": poses, "tolerance": 0.00002,
                   "native_guard_edits": guards}, stream, allow_nan=False)
    # Separate preserve-volume fixture: opposing joint rotations distinguish
    # DQ from LBS, and the second masked modifier must read cached input.
    mod.use_deform_preserve_volume = True
    multi.modifiers[-1].use_deform_preserve_volume = True
    rig.pose.bones["Tip.L"].rotation_mode = "XYZ"
    rig.pose.bones["Tip.L"].rotation_euler.z = -1.4
    rig.pose.bones["Root"].rotation_euler.x = 1.1
    bpy.context.view_layer.update()
    bpy.ops.wm.save_as_mainfile(filepath=str(output / "dual.blend"), compress=True)
    dual_poses = [sample("opposing rotations", [])]
    rig.pose.bones["Root"].rotation_euler.x = 1.5
    dual_poses.append(sample("DQ bone rotation", [{"id": bone_id(rig, "Root"), "channel": "rx", "value": math.degrees(1.5)}]))
    rig.pose.bones["Root"].rotation_euler.x = 1.1
    dual_poses.append(sample("DQ reset", []))
    with (output / "dual-reference.json").open("w") as stream:
        json.dump({"mesh": object_id(mesh), "poses": dual_poses, "tolerance": 0.00002}, stream, allow_nan=False)
    text = bpy.data.texts.new("autoexec.py")
    text.use_module = True
    text.write("from pathlib import Path\nPath(" + repr(str(output / "script-executed")) + ").write_text('unexpected execution')\n")
    bpy.ops.wm.save_as_mainfile(filepath=str(output / "unsupported.blend"), compress=True)
    bpy.data.texts.remove(text)
    mod.use_deform_preserve_volume = False
    multi.modifiers[-1].use_deform_preserve_volume = False
    for pb in rig.pose.bones:
        for con in pb.constraints:
            if con.type == "STRETCH_TO":
                con.mute = True
    bpy.ops.wm.save_as_mainfile(filepath=str(output / "strict.blend"), compress=True)
    print("Created compressed .blend fixtures and independent Blender reference poses")


if __name__ == "__main__":
    main()
