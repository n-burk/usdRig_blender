"""Check the native pickers of any converted rig.

Usage: checkPickerCoverage.py stage.usdc [--inventory source-inventory.json]

Every picker of the stage is checked against the picker contract: selectable
native controls, one single-target button per control, de-duplicated overflow,
whole-set group buttons, clickable buttons under the stock hit-test rule,
symmetric mirror links, settings as metadata. Nothing is keyed to a rig name.

The optional inventory is an independent capture of one armature's pose bones
({"armature": name, "bones": [{"name", "shape", "collections", "locks",
"properties"}]}), taken in Blender and not from USD. The controls it implies
under blender/picker.py's classification must be exactly what that armature's
picker selects.
"""
import argparse
import importlib.util
import json
import pathlib
import sys
from pxr import Usd, Sdf

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from package import picker_records


def template_slots():
    """(panel, button) -> role keys of the packaged biped template."""
    template = Sdf.Layer.FindOrOpen(str(ROOT / 'resources' / 'biped_picker.usda'))
    slots = {}
    for panel in template.GetPrimAtPath('/Template').nameChildren:
        for button in panel.nameChildren:
            data = button.customData.get('blender', {})
            # The template names roles, never source bones.
            assert set(data.keys()) <= {'roles'}, button.path
            slots[(panel.name, button.name)] = list(data.get('roles', []))
    return slots


def inventory_controls(path):
    """Armature name and control names of an independent source capture."""
    spec = importlib.util.spec_from_file_location('picker_inventory', ROOT / 'blender' / 'picker.py')
    picker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(picker)
    source = json.load(open(path))
    records = []
    for bone in source['bones']:
        properties = {key: value for key, value in bone.get('properties', {}).items()
                      if key not in picker.SYSTEM_PROPERTIES and not key.startswith('$')
                      and isinstance(value.get('value') if isinstance(value, dict) else value, (bool, int, float))}
        records.append({'name': bone['name'], 'shape': bone['shape'], 'collections': bone['collections'],
                        'locks': bone['locks'], 'properties': properties})
    controls, _ = picker.classify(records)
    return source['armature'], {record['name'] for record in controls}


def check(stage, picker, slots, record):
    """Check one picker; return its summary and the selected source ids."""
    assert picker.GetTypeName() == 'RigExecPicker'
    # Source JSON and inventories are not copied onto picker prims.
    assert picker.GetCustomDataByKey('blender:controlInventory') is None
    assert picker.GetCustomDataByKey('blender:source') is None
    clickable = {b['path']: b['clickable'] for page in record['pages'] for b in page['buttons']}
    coverage, library, placed, groups, mirrors = {}, [], {}, 0, 0
    for panel in picker.GetChildren():
        assert panel.GetTypeName() == 'RigExecPickerPanel'
        overflow = panel.GetName().startswith('page_')
        assert overflow or panel.GetName() in ('Body', 'Facial'), panel.GetPath()
        if overflow:
            # At most 48 entries in three columns.
            assert panel.GetAttribute('ui:size').Get()[1] <= 480 and len(panel.GetChildren()) <= 48, panel.GetPath()
        live = 0
        for button in panel.GetChildren():
            assert button.GetCustomDataByKey('blender:source') is None
            assert not button.GetCustomDataByKey('blender:unavailable')
            # The stock picker hides mode-less alternates and never draws sliders.
            assert not button.GetAttribute('ui:alternate').Get() and button.GetAttribute('ui:shape').Get() != 'slider'
            assert not button.GetRelationship('rigExec:picker:attribute').GetTargets()
            assert not button.GetAttribute('rigExec:picker:command').Get()
            targets = button.GetRelationship('rigExec:picker:controls').GetTargets()
            for peer in button.GetRelationship('rigExec:picker:mirror').GetTargets():
                mirrors += 1
                assert peer.HasPrefix(picker.GetPath()) and stage.GetPrimAtPath(peer)
                other = stage.GetPrimAtPath(peer)
                assert other.GetTypeName() == 'RigExecPickerButton'
                # Links are authored both ways and stay inside one panel. A selecting
                # slot pairs with a different selection: the same roles on the other
                # side, or for centre slots another centre role (upper and lower lip).
                assert other.GetRelationship('rigExec:picker:mirror').GetTargets() == [button.GetPath()], button.GetPath()
                assert other.GetParent() == panel
                selection = other.GetRelationship('rigExec:picker:controls').GetTargets()
                assert bool(selection) == bool(targets) and (not targets or selection != targets), button.GetPath()
                mine, theirs = (set(slots[(panel.GetName(), b.GetName())]) for b in (button, other))
                flipped = {{'L_': 'R_', 'R_': 'L_'}.get(r[:2], r[:2]) + r[2:] for r in mine}
                assert theirs == flipped if flipped != mine else not mine or theirs != mine, (button.GetPath(), sorted(mine), sorted(theirs))
            # Every selecting button keeps a clickable share under the stock hit test.
            assert not targets or clickable[str(button.GetPath())] >= 0.25, button.GetPath()
            names = []
            for target in targets:
                control = stage.GetPrimAtPath(target)
                assert control.GetTypeName() == 'RigExecControl', target
                ident = control.GetCustomDataByKey('blender:sourceId')
                assert ident, target
                for channel in ('tx', 'ty', 'tz', 'rx', 'ry', 'rz', 'sx', 'sy', 'sz'):
                    attr = control.GetAttribute('avars:' + channel)
                    assert attr and not attr.GetConnections()
                coverage[ident] = json.loads(ident)[-1] if ident.startswith('[') else ident
                names.append(ident)
            live += bool(targets)
            if overflow:
                # No dead buttons: an entry selects one control under its own
                # name, or is a source group button with its whole set.
                assert names, button.GetPath()
                if len(names) == 1:
                    assert button.GetAttribute('ui:text').Get() == coverage[names[0]], button.GetPath()
                    library.append(names[0])
                else:
                    assert len(set(names)) == len(names)
                    groups += 1
            else:
                roles = slots[(panel.GetName(), button.GetName())]
                # Unresolved slots are dropped; only pure artwork has no targets.
                assert bool(names) == bool(roles), button.GetPath()
                if len(roles) == 1:
                    # A role may own several slots (eyes on Body and Face) but one control.
                    assert len(names) == 1 and placed.setdefault(roles[0], names[0]) == names[0]
                else:
                    assert not names or 2 <= len(names) <= len(roles), button.GetPath()
        assert live, 'every authored panel has a live button'
    # Each control has exactly one single-target button: its biped slot or one overflow entry.
    assert len(library) == len(set(library)) and len(set(placed.values())) == len(placed)
    assert not set(library) & set(placed.values())
    assert set(library) | set(placed.values()) == set(coverage)
    assert picker.GetCustomDataByKey('blender:controlCount') == len(coverage)
    assert picker.GetCustomDataByKey('blender:bipedCount') == len(placed)
    assert picker.GetCustomDataByKey('blender:overflowCount') == len(library)
    assert picker.GetCustomDataByKey('blender:groupCount') == groups
    # Settings, operators and read-only selections are metadata with their reasons.
    unavailable = list(picker.GetCustomDataByKey('blender:unavailable') or [])
    reasons = list(picker.GetCustomDataByKey('blender:unsupportedReason') or [])
    assert len(unavailable) == len(reasons) and all(reasons)
    # Where both halves of a natively stacked FK/IK pair are live, each stays clickable.
    body = picker.GetChild('Body')
    for side in 'LR':
        for fk, ik in (('Hand', 'ArmIK'), ('Foot', 'LegIK'), ('UpLeg', 'LegPV')):
            pair = [body.GetChild('b_%s_%s' % (side, role)) for role in (fk, ik)] if body else []
            if all(pair):
                assert all(clickable[str(slot.GetPath())] >= 0.5 for slot in pair), (side, fk, ik)
    print('PICKER_COVERAGE_OK %s: %d controls, %d biped slots, %d overflow entries, %d group buttons, '
          '%d unavailable entries as metadata, %d mirror links' % (
              picker.GetPath(), len(coverage), len(placed), len(library), groups, len(unavailable), mirrors))
    return coverage


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('stage')
    parser.add_argument('--inventory', help='independent source capture of one armature')
    args = parser.parse_args()
    stage = Usd.Stage.Open(args.stage)
    assert stage
    slots = template_slots()
    records = {record['path']: record for record in picker_records(stage)}
    assert records, 'stage has no picker'
    selected = {}
    for path, record in records.items():
        for ident, name in check(stage, stage.GetPrimAtPath(path), slots, record).items():
            parts = json.loads(ident) if ident.startswith('[') else []
            if len(parts) == 4 and parts[0] == 'bone':
                # Bone ids are ["bone", library, armature, bone].
                selected.setdefault(parts[2], set()).add(name)
    if args.inventory:
        armature, expected = inventory_controls(args.inventory)
        assert armature in selected, 'no picker selects bones of ' + armature
        missing, extra = expected - selected[armature], selected[armature] - expected
        assert not missing and not extra, (sorted(missing)[:8], sorted(extra)[:8])
        print('PICKER_INVENTORY_OK %s: %d source controls' % (armature, len(expected)))


if __name__ == '__main__':
    main()
