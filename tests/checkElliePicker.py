"""Compare the native picker against an independently captured Blender inventory.

Usage: checkElliePicker.py stage.usdc source-inventory.json
Source inventory is captured from RIG-Ellie pose bones, not USD picker buttons.
"""
import argparse
import json
from pxr import Usd, Sdf

p = argparse.ArgumentParser()
p.add_argument('stage')
p.add_argument('source_inventory')
a = p.parse_args()
source = json.load(open(a.source_inventory))
# This independent source capture includes all pose bones and their original
# shapes/collection memberships. The source asset is expected to have755shapes.
assert source['armature'] == 'RIG-Ellie'
assert len(source['bones']) == 1887
shaped = {b['name'] for b in source['bones'] if b['shape']}
assert len(shaped) == 755
extras = {'MSTR-H-Head_Bottom', 'P-CTR-CheekPuff.L', 'P-CTR-CheekPuff.R',
          'P-Button1', 'P-Button2', 'P-Button3',
          'P-Pin1', 'P-Pin2', 'P-Pin3', 'P-Pin4', 'P-Pin5'}
assert not extras & shaped
expected = shaped | extras
assert len(expected) == 766
# Validate reusable template membership before conversion, including absent
# control regressions such as a generated heel helper with no editable UI.
template = Sdf.Layer.FindOrOpen(str(__import__('pathlib').Path(__file__).resolve().parents[1] / 'resources' / 'biped_picker.usda'))
for panel in template.GetPrimAtPath('/Template').nameChildren:
    for button in panel.nameChildren:
        assert set(button.customData.get('blender', {}).get('controlNames', [])) <= expected
stage = Usd.Stage.Open(a.stage)
assert stage
picker = stage.GetPrimAtPath('/Rig/Pickers/RIG_Ellie')
assert picker and picker.GetTypeName() == 'RigExecPicker'
inventory = json.loads(picker.GetCustomDataByKey('blender:controlInventory'))
assert {r['name'] for r in inventory['controls']} == expected
assert len(inventory['controls']) == 766
coverage, settings, mirrors = set(), 0, 0
library = []
for panel in picker.GetChildren():
    assert panel.GetTypeName() == 'RigExecPickerPanel'
    if panel.GetName().startswith('page_'):
        size = panel.GetAttribute('ui:size').Get()
        assert size[1] <= 480
    for button in panel.GetChildren():
        assert not button.GetRelationship('rigExec:picker:attribute').GetTargets()
        assert not button.GetAttribute('rigExec:picker:command').Get()
        for peer in button.GetRelationship('rigExec:picker:mirror').GetTargets():
            mirrors += 1
            assert peer.HasPrefix(picker.GetPath()) and stage.GetPrimAtPath(peer)
            assert stage.GetPrimAtPath(peer).GetTypeName() == 'RigExecPickerButton'
        targets = button.GetRelationship('rigExec:picker:controls').GetTargets()
        for target in targets:
            control = stage.GetPrimAtPath(target)
            assert control.GetTypeName() == 'RigExecControl'
            sid = json.loads(control.GetCustomDataByKey('blender:sourceId'))
            assert sid[:3] == ['bone', '', 'RIG-Ellie']
            assert sid[3] in expected
            for channel in ('tx','ty','tz','rx','ry','rz','sx','sy','sz'):
                attr = control.GetAttribute('avars:' + channel)
                assert attr and not attr.GetConnections()
            coverage.add(sid[3])
            if panel.GetName().startswith('page_'):
                library.append(sid[3])
        if not targets and panel.GetAttribute('ui:label').Get().startswith('Source settings'):
            assert button.GetCustomDataByKey('blender:unavailable')
            assert button.GetCustomDataByKey('blender:unsupportedReason')
            settings += 1
assert coverage == expected
assert len(library) == len(set(library)) == 766
assert settings == 115
assert mirrors == 290
assert stage.GetPrimAtPath(picker.GetPath().AppendChild('Body'))
assert stage.GetPrimAtPath(picker.GetPath().AppendChild('Facial'))
print('ELLIE_NATIVE_PICKER_COVERAGE_OK:766 controls,755 custom shapes,115 explicit unavailable settings')
