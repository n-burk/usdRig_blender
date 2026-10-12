"""Write tests/fixtures/leg.blendrig: Rigify-convention legs as a hand-built snapshot.

    python tests/makeLegFixture.py tests/fixtures/leg.blendrig
    python tests/makeLegFixture.py tests/fixtures/leg.blendrig --check

Both sides carry the bones limbs.leg generates for a thigh, shin and foot: an
FK chain, an IK chain solved onto a foot_ik control, the ORG chain that copies
both, tweaks and a DEF chain, plus a skinned quad. No Blender is involved; the
snapshot only has to reach the connected lower-limb calibration.
"""
import json
import math
import sys

I = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def r(value):
    return round(value, 6) + 0.0


def RX(degrees, t=(0, 0, 0)):
    c, s = r(math.cos(math.radians(degrees))), r(math.sin(math.radians(degrees)))
    return [1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, t[0], t[1], t[2], 1]


def T(x, y, z):
    return RX(0, (x, y, z))


def node(ident, parent, kind='joint', rest=I, **extra):
    data = {"id": ident, "name": ident.split('/')[-1], "kind": kind, "parent": parent, "rest": rest,
            "pose": [0, 0, 0, 0, 0, 0, 1, 1, 1], "length": 1, "visible": True}
    if kind == 'joint':
        data.update({"armature": "Rig", "deform": False, "inherit_scale": "FULL", "inherit_rotation": True,
                     "local_location": True, "connected": False, "ik_stretch": 0})
    data.update(extra)
    return data


def constraint(owner, kind, source, name, **extra):
    data = {"owner": owner, "name": name, "type": kind, "enabled": True, "influence": 1, "source": source,
            "owner_space": "WORLD", "target_space": "WORLD"}
    data.update(extra)
    return data


def copy(owner, source, name='Copy Transforms'):
    return constraint(owner, "COPY_TRANSFORMS", source, name, mix_mode="REPLACE", head_tail=0,
                      remove_target_shear=False, use_bbone_shape=False)


def build():
    nodes = [node("Rig", "", kind='control', object_type="ARMATURE"), node("Body", "", kind='control', object_type="MESH"),
             node("Rig/spine", "Rig", deform=True)]
    constraints = []
    bend = 20
    up, knee, ankle = RX(bend - 90, (0, 0, 2)), RX(-2 * bend, (0, 1, 0)), RX(bend - 90, (0, 1, 0))
    for side, x in (("L", 1), ("R", -1)):
        def b(name):
            return "Rig/" + name + "." + side
        hip = list(up)
        hip[12] = x
        nodes += [node(b("thigh_fk"), "Rig/spine", rest=hip), node(b("shin_fk"), b("thigh_fk"), rest=knee, connected=True),
                  node(b("foot_fk"), b("shin_fk"), rest=ankle, connected=True),
                  node(b("MCH-thigh_ik"), "Rig/spine", rest=hip, ik_stretch=0.1),
                  node(b("MCH-shin_ik"), b("MCH-thigh_ik"), rest=knee, ik_stretch=0.1),
                  node(b("foot_ik"), "Rig", rest=T(x, 0, r(2 - 2 * math.cos(math.radians(bend))))),
                  node(b("MCH-thigh_ik_target"), b("foot_ik"), rest=RX(-90)),
                  node(b("ORG-thigh"), "Rig/spine", rest=hip), node(b("ORG-shin"), b("ORG-thigh"), rest=knee, connected=True),
                  node(b("ORG-foot"), b("ORG-shin"), rest=ankle, connected=True),
                  node(b("shin_tweak"), b("ORG-shin"), length=0.25), node(b("foot_tweak"), b("ORG-foot"), length=0.25),
                  node(b("DEF-thigh"), "Rig/spine", rest=hip, deform=True),
                  node(b("DEF-shin"), b("DEF-thigh"), rest=knee, connected=True, deform=True)]
        constraints += [
            constraint(b("MCH-shin_ik"), "IK", b("MCH-thigh_ik_target"), "IK", chain_count=2, use_tail=True,
                       use_stretch=True, use_location=True, use_rotation=False, pole_angle=0),
            copy(b("ORG-thigh"), b("thigh_fk")), copy(b("ORG-thigh"), b("MCH-thigh_ik"), 'Copy Transforms.001'),
            copy(b("ORG-shin"), b("shin_fk")), copy(b("ORG-shin"), b("MCH-shin_ik"), 'Copy Transforms.001'),
            copy(b("ORG-foot"), b("foot_fk")), copy(b("ORG-foot"), b("MCH-thigh_ik_target"), 'Copy Transforms.001'),
            copy(b("DEF-thigh"), b("ORG-thigh")), copy(b("DEF-shin"), b("shin_tweak")),
            constraint(b("DEF-shin"), "STRETCH_TO", b("foot_tweak"), "Stretch To", head_tail=0, rest_length=1, bulge=1,
                       volume="VOLUME_XZX", keep_axis="SWING_Y", use_bbone_shape=False, use_bulge_min=False,
                       use_bulge_max=False, bulge_min=1, bulge_max=1, bulge_smooth=0)]
    mesh = {"id": "Body", "points": [[-1, 0, 2], [1, 0, 2], [1, 0, 3], [-1, 0, 3]], "counts": [4], "indices": [0, 1, 2, 3],
            "uv": [[0, 0], [1, 0], [1, 1], [0, 1]], "smooth": [False], "materials": ["Skin"], "material_indices": [0],
            "skin": {"influences": ["Rig/spine", "Rig/DEF-shin.L"],
                     "weights": [[[0, 1]], [[0, 1]], [[0, 1]], [[0, 0.5], [1, 0.5]]], "method": "classicLinear"}}
    scene = {"format": "usdBlenderRig", "version": 1, "source": "", "meters_per_unit": 1, "fps": 24, "start": 1, "end": 2}
    return ('{' + ', '.join('"%s": %s' % (key, json.dumps(value)) for key, value in scene.items()) + ',\n "nodes": [\n' +
            ',\n'.join('  ' + json.dumps(n) for n in nodes) + '\n ],\n "constraints": [\n' +
            ',\n'.join('  ' + json.dumps(c) for c in constraints) + '\n ],\n' +
            ' "meshes": [\n  ' + json.dumps(mesh) + '\n ],\n "materials": [{"id": "Skin", "color": [0.8, 0.6, 0.5, 1], '
            '"roughness": 0.5, "metallic": 0, "texture": ""}],\n "dependencies": [], "diagnostics": []}\n')


def main():
    arguments = [a for a in sys.argv[1:] if a != '--check']
    if len(arguments) != 1:
        raise SystemExit(__doc__)
    text = build()
    if '--check' in sys.argv[1:]:
        if open(arguments[0]).read() != text:
            raise SystemExit(arguments[0] + ' differs from the generated fixture')
        print(arguments[0] + ' matches the generated fixture')
    else:
        open(arguments[0], 'w').write(text)


if __name__ == '__main__':
    main()
