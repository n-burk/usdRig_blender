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
    transform_probes=[]
    for from_kind,mix,extrapolate,influence,constrained in (
            ("ROTATION","ADD",False,1.,False),
            ("ROTATION","REPLACE",False,.4,False),
            ("ROTATION","ADD",True,1.,False),
            ("ROTATION","ADD",False,.6,True),
            ("LOCATION","ADD",False,1.,False),
            ("SCALE","REPLACE",True,.4,False)):
        name=f"Transform {from_kind} {mix} {extrapolate} {influence} {constrained}"
        bone=armature.edit_bones.new(name)
        bone.head,bone.tail,bone.roll=(-.15,.3,.25),(.1,.9,.35),.2
        bone.parent,bone.use_deform=root,False
        inheritance.append(name);transform_probes.append((name,from_kind,mix,extrapolate,influence,constrained))
    bone=armature.edit_bones.new("Transform constrained source")
    bone.head,bone.tail,bone.roll=(.1,.4,-.1),(.2,1.1,.2),-.15
    bone.parent,bone.use_deform=root,False
    inheritance.append(bone.name)
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
    constrained_source=rig.pose.bones["Transform constrained source"]
    con=constrained_source.constraints.new("COPY_ROTATION")
    con.target,con.subtarget,con.owner_space,con.target_space=source_rig,"Target","LOCAL","LOCAL"
    for name,from_kind,mix,extrapolate,influence,constrained in transform_probes:
        pb=rig.pose.bones[name]
        con=pb.constraints.new("ARMATURE")
        target=con.targets.new();target.target,target.subtarget,target.weight=rig,"Tip.L",1.
        con.use_deform_preserve_volume=True
        con=pb.constraints.new("TRANSFORM")
        con.target,con.subtarget=rig,"Transform constrained source" if constrained else "Root"
        con.owner_space,con.target_space="LOCAL","LOCAL"
        con.map_from,con.map_to,con.mix_mode=from_kind,"LOCATION",mix
        con.use_motion_extrapolate,con.influence=extrapolate,influence
        con.map_to_x_from,con.map_to_y_from,con.map_to_z_from="Y","Z","X"
        suffix="_rot" if from_kind=="ROTATION" else "_scale" if from_kind=="SCALE" else ""
        minimum=(0,0,0) if from_kind=="ROTATION" else (.5,.5,.5) if from_kind=="SCALE" else (-.2,-.2,-.2)
        maximum=(math.radians(60),.6,0) if from_kind=="ROTATION" else (1.5,1.5,1.5) if from_kind=="SCALE" else (.3,.3,.3)
        for i,axis in enumerate("xyz"):
            setattr(con,"from_min_"+axis+suffix,minimum[i]);setattr(con,"from_max_"+axis+suffix,maximum[i])
            setattr(con,"to_min_"+axis,(-.01,.01,0)[i]);setattr(con,"to_max_"+axis,(.03,-.02,.12)[i])
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
    shape_objects = []
    for skinned in (True, False):
        shaped = bpy.data.objects.new("Shape skin helper" if skinned else "Shape owner follow", data.copy())
        scene.collection.objects.link(shaped)
        shaped.parent, shaped.location = master, (-.3,.2,.4)
        shaped.rotation_euler, shaped.scale = (.21,-.14,.3),(1.3,.7,1.1)
        for name in ("Root","Tip.L"):
            group = shaped.vertex_groups.new(name=name)
            for vertex in data.vertices:
                for membership in vertex.groups:
                    if mesh.vertex_groups[membership.group].name == name:
                        group.add([vertex.index],membership.weight,"REPLACE")
        group = shaped.vertex_groups.new(name="Shape mask")
        for i,w in enumerate((0,.25,.75,1,.5)):group.add([i],w,"REPLACE")
        basis = shaped.shape_key_add(name="Basis")
        primary = shaped.shape_key_add(name="Primary")
        primary.slider_min, primary.slider_max, primary.value = -1,2,.7
        for i,p in enumerate(primary.data):p.co += Vector((.03*(i+1),-.025*i,.12+.04*i))
        relative = shaped.shape_key_add(name="Relative masked")
        relative.relative_key, relative.vertex_group, relative.value = primary,"Shape mask",.4
        for i,p in enumerate(relative.data):p.co = primary.data[i].co + Vector((-.08,.015*i,.05))
        muted = shaped.shape_key_add(name="Muted")
        muted.value, muted.mute = .8,True
        for p in muted.data:p.co += Vector((.4,-.3,.2))
        limited = shaped.shape_key_add(name="Positive range")
        limited.slider_min,limited.slider_max,limited.value=.5,1.0,.6
        for p in limited.data:p.co += Vector((-.02,.01,-.015))
        if skinned:
            shaped.modifiers.new("Skin","ARMATURE").object = rig
        shape_objects.append(shaped)
        test_meshes.append(shaped)
    bpy.context.view_layer.update()
    evaluated = shape_objects[0].evaluated_get(bpy.context.evaluated_depsgraph_get())
    triangle = [evaluated.matrix_world @ evaluated.data.vertices[i].co for i in (3,2,4)]
    normal = (triangle[1]-triangle[0]).cross(triangle[2]-triangle[0]).normalized()
    sink_data = bpy.data.meshes.new("Shape driven shrink attachment")
    sink_data.from_pydata([triangle[0]*a+triangle[1]*b+triangle[2]*(1-a-b)+normal*.08
                           for a,b in ((.2,.2),(.3,.2),(.2,.3))],[],[(0,1,2)])
    sink = bpy.data.objects.new("Shape driven shrink attachment",sink_data)
    scene.collection.objects.link(sink)
    shrink = sink.modifiers.new("Shape helper shrink","SHRINKWRAP")
    shrink.target,shrink.wrap_method,shrink.wrap_mode,shrink.offset=shape_objects[0],"NEAREST_SURFACEPOINT","ON_SURFACE",.02
    test_meshes.append(sink)
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

    # Hidden dependency cages still need their topology modifiers evaluated.
    hidden_data = bpy.data.meshes.new("Hidden mirrored cage")
    hidden_data.from_pydata([(0,0,0),(1,0,0),(1,1,0),(0,1,0)], [], [(0,1,2,3)])
    hidden_cage = bpy.data.objects.new("Hidden mirrored cage", hidden_data)
    scene.collection.objects.link(hidden_cage)
    hidden_cage.parent = master
    hidden_cage.modifiers.new("Mirror", "MIRROR")
    hidden_group = hidden_cage.vertex_groups.new(name="Root")
    hidden_group.add([0,1,2,3], 1, "REPLACE")
    hidden_cage.modifiers.new("Skin", "ARMATURE").object = rig

    # Saved Surface Deform bindings exercise nonidentity object spaces,
    # polygon offsets, sparse masks, and a driver that itself has a skin chain.
    bpy.context.view_layer.update()
    for sparse, cage in ((False, mesh), (True, mesh), (False, hidden_cage)):
        cage_eval = cage.evaluated_get(bpy.context.evaluated_depsgraph_get())
        cage_points = [cage_eval.matrix_world @ v.co for v in cage_eval.data.vertices]
        normal = (cage_points[1] - cage_points[0]).cross(cage_points[2] - cage_points[0]).normalized()
        attachment_data = bpy.data.meshes.new("Surface attachment")
        attachment = bpy.data.objects.new("Sparse surface" if sparse else "Bound surface", attachment_data)
        scene.collection.objects.link(attachment)
        attachment.parent = master
        attachment.location = (0.2, -0.3, 0.4)
        attachment.rotation_euler = (0.1, 0.2, -0.3)
        attachment.scale = (0.9, 1.2, 1.1)
        bpy.context.view_layer.update()
        inverse = attachment.matrix_world.inverted()
        positions = [inverse @ (cage_points[0]*a + cage_points[1]*b + cage_points[2]*(1-a-b) + normal*0.12)
                     for a, b in ((0.2, 0.2), (0.3, 0.4), (0.5, 0.2))]
        attachment_data.from_pydata(positions, [], [(0, 1, 2)])
        surface = attachment.modifiers.new("Bound cage", "SURFACE_DEFORM")
        surface.target = cage
        if sparse:
            group = attachment.vertex_groups.new(name="Mask")
            group.add([0], 1, "REPLACE")
            group.add([1], 0.4, "REPLACE")
            surface.vertex_group = group.name
            surface.use_sparse_bind = True
            surface.strength = 0.7
        bpy.context.view_layer.objects.active = attachment
        bpy.ops.object.surfacedeform_bind(modifier=surface.name)
        assert surface.is_bound
        test_meshes.append(attachment)

    # Ordered deformation dependency: skin -> saved DeltaMush -> bound surface
    # -> two regular-grid lattices. Cage owner and Hook target are distinct bones.
    grid_data = bpy.data.meshes.new("Corrective cage")
    grid_data.from_pydata([(x*.34+.02*y*y, y*.31, .08*math.sin(x*1.1)*math.cos(y))
                          for y in range(4) for x in range(4)], [],
                         [(4*y+x,4*y+x+1,4*y+x+5,4*y+x+4) for y in range(3) for x in range(3)])
    grid = bpy.data.objects.new("Corrective cage", grid_data)
    scene.collection.objects.link(grid)
    grid.parent = master
    grid.location, grid.rotation_euler, grid.scale = (0.1,-0.2,.3), (.12,-.23,.17), (.87,1.3,.72)
    for name in ("Root", "Tip.L"):
        grid.vertex_groups.new(name=name)
    for i in range(16):
        w = (i//4)/3
        grid.vertex_groups["Root"].add([i], 1-w, "REPLACE")
        grid.vertex_groups["Tip.L"].add([i], w, "REPLACE")
    group = grid.vertex_groups.new(name="Smoothing")
    for i in range(16): group.add([i], (i%4)/3, "REPLACE")
    grid.modifiers.new("Skin", "ARMATURE").object = rig
    rig.pose.bones["Tip.L"].rotation_mode = "XYZ"
    corrective = grid.modifiers.new("DeltaMush", "CORRECTIVE_SMOOTH")
    corrective.rest_source = "BIND"
    corrective.smooth_type = "LENGTH_WEIGHTED"
    corrective.factor, corrective.iterations, corrective.scale = .47, 4, 1.2
    corrective.vertex_group = group.name
    bpy.context.view_layer.objects.active = grid
    bpy.ops.object.correctivesmooth_bind(modifier=corrective.name)
    bpy.context.view_layer.update()
    assert corrective.is_bind
    test_meshes.append(grid)
    cages = []
    for number, parent_bone, hook_bone in ((0,"Root","Tip.L"),(1,"Tip.L","Root")):
        lattice = bpy.data.lattices.new("Hook cage data"+str(number))
        lattice.points_u, lattice.points_v, lattice.points_w = 4, 3, 4
        lattice.interpolation_type_u = "KEY_BSPLINE" if number == 0 else "KEY_CARDINAL"
        lattice.interpolation_type_v = "KEY_LINEAR"
        lattice.interpolation_type_w = "KEY_CATMULL_ROM"
        cage = bpy.data.objects.new("Hook cage"+str(number), lattice)
        scene.collection.objects.link(cage)
        cage.parent, cage.parent_type, cage.parent_bone = rig, "BONE", parent_bone
        cage.matrix_parent_inverse = Matrix.Translation((.17,-.25,.12))
        cage.location, cage.rotation_euler, cage.scale = (.3,.1,-.2), (.17,-.2,.23), (1.4,.85,1.1)
        for p in lattice.points:
            a = p.co
            p.co_deform = (a.x+.07*math.sin(a.y*2), a.y+.05*a.x*a.x, a.z+.08*math.sin(a.x*2+a.y))
        group = cage.vertex_groups.new(name="Hook")
        for i in range(len(lattice.points)): group.add([i], (0,.25,.75,1)[i%4], "REPLACE")
        hook = cage.modifiers.new("Weighted Hook", "HOOK")
        hook.object, hook.subtarget, hook.vertex_group = rig, hook_bone, group.name
        hook.strength, hook.falloff_radius = .61, 0
        bpy.context.view_layer.update()
        target = rig.matrix_world @ rig.pose.bones[hook_bone].matrix
        hook.matrix_inverse = target.inverted() @ cage.matrix_world @ Matrix.Translation((.02,-.01,.03))
        if number == 1: hook.vertex_indices_set(list(range(0,len(lattice.points),2)))
        cages.append(cage)
        test_meshes.append(cage)
    # A skinned mesh followed by a lattice whose object has an Armature
    # constraint must not apply shared root/head movement twice. Exercise
    # object-origin DQ binding, partial influence and multiple target frames.
    object_frame_probes = [master]
    sibling = empty("Unconstrained object sibling",(.2,-.1,.3))
    sibling.parent = master
    sibling.rotation_euler,sibling.scale = (.2,-.3,.1),(1.1,.85,1.3)
    object_frame_probes.append(sibling)
    for dual, target_count in ((False,1),(True,1),(False,2),(True,2)):
        label = "Object armature cage " + str(dual) + str(target_count)
        lattice = bpy.data.lattices.new(label)
        lattice.points_u = lattice.points_v = lattice.points_w = 3
        cage = bpy.data.objects.new(label,lattice)
        scene.collection.objects.link(cage)
        cage.parent = master
        cage.location, cage.rotation_euler, cage.scale = (.2,-.1,.3), (.2,-.3,.1), (1.1,.85,1.3)
        constraint = cage.constraints.new("ARMATURE")
        constraint.use_deform_preserve_volume = dual
        constraint.use_current_location = False  # Objects still use incoming origin.
        constraint.influence = 1 if target_count == 1 else .65
        for bone_name,weight in (("Root",1),) if target_count == 1 else (("Root",.35),("Tip.L",.65)):
            target = constraint.targets.new()
            target.target,target.subtarget,target.weight = rig,bone_name,weight
        test_meshes.append(cage)
        object_frame_probes.append(cage)
        data = bpy.data.meshes.new(label + " mesh")
        data.from_pydata([(.1,.1,.2),(.3,.1,.2),(.1,.3,.2)],[],[(0,1,2)])
        probe = bpy.data.objects.new(label + " mesh",data)
        scene.collection.objects.link(probe)
        probe.parent = master
        probe.vertex_groups.new(name="Root").add([0,1,2],1,"REPLACE")
        probe.modifiers.new("Skin","ARMATURE").object = rig
        probe.modifiers.new("Constrained cage","LATTICE").object = cage
        test_meshes.append(probe)
    bpy.context.view_layer.update()
    evaluated = grid.evaluated_get(bpy.context.evaluated_depsgraph_get())
    polygon = evaluated.data.polygons[4]
    positions = [evaluated.matrix_world @ evaluated.data.vertices[i].co for i in polygon.vertices]
    normal = (positions[1]-positions[0]).cross(positions[2]-positions[0]).normalized()
    attachment_data = bpy.data.meshes.new("Stacked attachment")
    attachment = bpy.data.objects.new("Stacked attachment", attachment_data)
    scene.collection.objects.link(attachment)
    attachment.parent = master
    attachment.location, attachment.rotation_euler, attachment.scale = (.2,-.3,.4), (.1,.2,-.3), (.9,1.2,1.1)
    bpy.context.view_layer.update()
    inverse = attachment.matrix_world.inverted()
    attachment_data.from_pydata([inverse @ (positions[0]*a+positions[1]*b+positions[2]*(1-a-b)+normal*.06)
                                for a,b in ((.2,.2),(.3,.4),(.5,.2))], [], [(0,1,2)])
    surface = attachment.modifiers.new("Bound corrected cage", "SURFACE_DEFORM")
    surface.target = grid
    bpy.context.view_layer.objects.active = attachment
    bpy.ops.object.surfacedeform_bind(modifier=surface.name)
    assert surface.is_bound
    for number,cage in enumerate(cages):
        group = attachment.vertex_groups.new(name="Lattice mask"+str(number))
        group.add([0], 1, "REPLACE"); group.add([1], .4, "REPLACE")
        if number == 0:
            extra = attachment.vertex_groups.new(name="Additional lattice mask")
            for i,w in enumerate((.2,.35,.1)): extra.add([i],w,"REPLACE")
            mix = attachment.modifiers.new("Consumed mask mix", "VERTEX_WEIGHT_MIX")
            mix.vertex_group_a, mix.vertex_group_b = group.name, extra.name
            mix.mix_mode, mix.mix_set, mix.mask_constant = "ADD", "ALL", .7
        lattice = attachment.modifiers.new("Lattice"+str(number), "LATTICE")
        lattice.object, lattice.vertex_group = cage, group.name
        lattice.invert_vertex_group, lattice.strength = number == 1, .79
    test_meshes.append(attachment)
    # Four nearest-surface policies, with both sides of a transformed closed target.
    bpy.ops.mesh.primitive_cube_add(size=1, location=(.3,-.2,.4))
    shrink_target = bpy.context.object
    shrink_target.name = "Shrink target"
    shrink_target.parent = master
    shrink_target.rotation_euler, shrink_target.scale = (.2,-.17,.3), (1.2,.73,1.4)
    test_meshes.append(shrink_target)
    for number,mode in enumerate(("ON_SURFACE","INSIDE","OUTSIDE","OUTSIDE_SURFACE")):
        data = bpy.data.meshes.new("Shrink probe"+str(number))
        data.from_pydata([(-.6,.01,.08),(.36,.1,.15),(.02,.01,.07),(.1,.02,.8)], [], [(0,1,2),(1,2,3)])
        probe = bpy.data.objects.new("Shrink probe"+str(number), data)
        scene.collection.objects.link(probe)
        probe.parent = master
        probe.location, probe.rotation_euler, probe.scale = (.17,-.1,.26), (.12,.2,-.19), (.93,1.14,.82)
        group = probe.vertex_groups.new(name="Shrink mask")
        for i,w in enumerate((0,.3,.7,1)): group.add([i], w, "REPLACE")
        shrink = probe.modifiers.new("Shrinkwrap", "SHRINKWRAP")
        shrink.target, shrink.wrap_method, shrink.wrap_mode = shrink_target, "NEAREST_SURFACEPOINT", mode
        shrink.vertex_group, shrink.invert_vertex_group, shrink.offset = group.name, number%2 == 1, .03
        test_meshes.append(probe)

    hidden_cage.hide_viewport = True

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
        if obj.type == "LATTICE":
            return [list(evaluated.matrix_world @ point.co_deform) for point in evaluated.data.points]
        return [list(evaluated.matrix_world @ vertex.co) for vertex in evaluated.data.vertices]

    def sample(name, edits):
        return {"name": name, "edits": edits, "points": points(),
                "meshes": [{"mesh": object_id(obj), "points": points(obj)} for obj in test_meshes],
                "frames": [{"id":object_id(obj),"points":[list(obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).matrix_world @ Vector(p))
                    for p in ((0,0,0),(1,0,0),(0,1,0),(0,0,1))]} for obj in object_frame_probes]}

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
    tip_pose = rig.pose.bones["Tip.L"]
    saved_tip_location, saved_tip_rotation = tip_pose.location.copy(), tip_pose.rotation_euler.copy()
    tip_pose.location.x += .15
    tip_pose.rotation_euler.z += .19
    hook_edits = [{"id": bone_id(rig,"Tip.L"), "channel": "tx", "value": float(tip_pose.location.x)},
                  {"id": bone_id(rig,"Tip.L"), "channel": "rz", "value": math.degrees(tip_pose.rotation_euler.z)}]
    poses.append(sample("hook target motion", hook_edits))
    rig.pose.bones["Root"].rotation_euler.x += .17
    poses.append(sample("combined hook and cage parent motion", hook_edits +
                 [{"id": bone_id(rig,"Root"), "channel": "rx", "value": math.degrees(rig.pose.bones["Root"].rotation_euler.x)}]))
    tip_pose.location, tip_pose.rotation_euler = saved_tip_location, saved_tip_rotation
    rig.pose.bones["Root"].rotation_euler = saved_rotation
    saved_source_rotation=source_pb.rotation_euler.copy()
    for angle in (-10.,0.,15.,40.,80.):
        rig.pose.bones["Root"].rotation_euler.x=math.radians(angle)
        source_pb.rotation_euler.x=math.radians(angle)
        poses.append(sample("transform mapping source "+str(angle),[
            {"id":bone_id(rig,"Root"),"channel":"rx","value":angle},
            {"id":bone_id(source_rig,"Target"),"channel":"rx","value":angle}]))
    rig.pose.bones["Root"].rotation_euler=saved_rotation
    source_pb.rotation_euler=saved_source_rotation
    for value in (-1.0,-.35,.25,1.0,1.6):
        edits=[]
        for shaped in shape_objects:
            shaped.data.shape_keys.key_blocks["Primary"].value=value
            edits.append({"id":object_id(shaped),"shape_key":"Primary","value":value})
            shaped.data.shape_keys.key_blocks["Muted"].value=.2
            edits.append({"id":object_id(shaped),"shape_key":"Muted","value":.2})
        poses.append(sample("signed shape weight "+str(value),edits))
    for shaped in shape_objects:
        shaped.data.shape_keys.key_blocks["Primary"].value=.7
        shaped.data.shape_keys.key_blocks["Muted"].value=.8
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
