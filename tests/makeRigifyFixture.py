"""Generate the bundled Rigify human rig.

    blender --background --factory-startup --python-exit-code 1 \
        --python tests/makeRigifyFixture.py -- build/blender-proof

Writes rigify-human.blend and rigify-human.json, the rig's UI collections and
their sidebar rows as Rigify itself reports them, then rigify-human.blendrig
the documented way: blender/extract.py in a second Blender that has not
enabled the add-on. Exits 77 where this Blender carries no Rigify.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

import bpy


def main():
    output = Path(sys.argv[sys.argv.index("--") + 1]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    try:
        bpy.ops.preferences.addon_enable(module="rigify")
        from rigify.metarigs import human
    except Exception as error:
        print("Rigify is unavailable: %s" % error)
        sys.stdout.flush()
        os._exit(77)
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    data = bpy.data.armatures.new("metarig")
    metarig = bpy.data.objects.new("metarig", data)
    bpy.context.scene.collection.objects.link(metarig)
    bpy.context.view_layer.objects.active = metarig
    metarig.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    human.create(metarig)
    bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.pose.rigify_generate()
    rig = data.rigify_target_rig
    rows = {c.name: c.rigify_ui_row for c in rig.data.collections_all if c.rigify_ui_row > 0}
    bpy.ops.wm.save_as_mainfile(filepath=str(output / "rigify-human.blend"), compress=True)
    with (output / "rigify-human.json").open("w") as stream:
        json.dump({"rig": rig.name, "rows": rows}, stream)
    print("Generated the Rigify human rig: %d UI collections, %d bones" % (len(rows), len(rig.data.bones)))
    sys.stdout.flush()
    subprocess.run([bpy.app.binary_path, "--background", "--factory-startup", "--disable-autoexec",
                    "--python-exit-code", "1", "--python", str(Path(__file__).resolve().parents[1] / "blender" / "extract.py"),
                    "--", str(output / "rigify-human.blend"), str(output / "rigify-human.blendrig")], check=True)


if __name__ == "__main__":
    main()
