"""Capture one armature's pose bones for tests/checkPickerCoverage.py --inventory.

    blender --background --factory-startup --disable-autoexec --python-exit-code 1 \
        --python tests/capturePickerInventory.py -- character.blend capture.json [armature]

Reads Blender data directly and shares no code with blender/extract.py, so the
capture is an independent statement of which bones carry shapes, collections,
unlocked channels and numeric properties. Without an armature name the file
must hold exactly one armature object with pose bones.
"""
import json
import sys

import bpy


def main():
    arguments = sys.argv[sys.argv.index('--') + 1:]
    if len(arguments) not in (2, 3):
        raise SystemExit(__doc__)
    bpy.ops.wm.open_mainfile(filepath=arguments[0], use_scripts=False)
    rigs = [o for o in bpy.data.objects if o.type == 'ARMATURE' and o.pose and
            (len(arguments) == 2 or o.name == arguments[2])]
    if len(rigs) != 1:
        raise SystemExit('expected one armature, found: ' + ', '.join(sorted(o.name for o in rigs)))
    bones = []
    for bone in rigs[0].pose.bones:
        properties = {}
        for key in bone.keys():
            value = bone[key]
            if isinstance(value, (bool, int, float)):
                properties[key] = {'value': value}
        bones.append({'name': bone.name, 'shape': bone.custom_shape.name if bone.custom_shape else '',
                      'collections': [c.name for c in bone.bone.collections],
                      'locks': {'translation': list(bone.lock_location), 'rotation': list(bone.lock_rotation),
                                'scale': list(bone.lock_scale)},
                      'properties': properties})
    with open(arguments[1], 'w') as output:
        json.dump({'armature': rigs[0].name, 'bones': bones}, output, indent=1)
    print('captured %d pose bones of %s' % (len(bones), rigs[0].name))


if __name__ == '__main__':
    main()
