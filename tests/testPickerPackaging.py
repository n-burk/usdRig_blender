"""Check complete Rigify/CloudRig page extraction and USDZ preservation.

Run after CTest's blenderFixture in the host USD/RigExec Python environment:
    python tests/testPickerPackaging.py build/blender-proof build/picker-proof
"""
import json
import base64
from pathlib import Path
import sys

from pxr import Ar, Sdf, Usd, UsdShade

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from package import package, picker_records, udim_records


def main():
    fixture, output = map(Path, sys.argv[1:])
    output.mkdir(parents=True, exist_ok=True)
    snapshot = json.loads((fixture / 'rig.blendrig').read_text())
    render_mesh = next(m for m in snapshot['meshes'] if m.get('uv_active') == 'EditingUV')
    assert render_mesh['uv_render'] == 'UVMap'
    assert render_mesh['uv'] == render_mesh['uv_sets']['UVMap']
    assert render_mesh['uv'] != render_mesh['uv_sets']['EditingUV']
    source = snapshot['pickers']
    assert len(source) == 2
    rigify = next(p for p in source if p['source_kind'] == 'Rigify control collections')
    assert [p['name'] for p in rigify['pages']] == ['Main', 'Hand']
    assert rigify['pages'][1]['source']['visible'] is False
    cloud = next(p for p in source if p['source_kind'] == 'CloudRig sidebar panels')
    assert [p['name'] for p in cloud['pages']] == ['Settings', 'Groups', 'Controls: Controls', 'Controls: Nested']
    assert [b['label'] for b in cloud['pages'][0]['buttons']] == ['Target', 'Nested target']
    assert cloud['pages'][1]['buttons'][0]['controls'] == cloud['pages'][3]['buttons'][0]['controls']
    stage = Usd.Stage.Open(str(fixture / 'rig.blendrig'))
    records = picker_records(stage)
    diagnostics = stage.GetDefaultPrim().GetAttribute('blender:diagnostics').Get() or []
    native_switch_targets = {
        str(target)
        for prim in stage.Traverse() if prim.GetTypeName() == 'RigExecSpaceSwitch'
        for target in prim.GetRelationship('rigExec:target').GetTargets()
    }
    assert sum(len(p['pages']) for p in records) == 6
    expected = {p['owner']: p for p in source}
    for picker in records:
        original = json.loads(picker['source'])
        assert original == expected[original['owner']]
        assert len(picker['pages']) == len(original['pages'])
        for page, original_page in zip(picker['pages'], original['pages']):
            assert page['label'] == original_page['name']
            assert json.loads(page['source']) == original_page
            assert len(page['buttons']) == len(original_page['buttons'])
            for button, original_button in zip(page['buttons'], original_page['buttons']):
                actual_ids = []
                for path in button['targets']:
                    target = stage.GetPrimAtPath(path)
                    assert target, path
                    if target.GetCustomDataByKey('blender:sourceId'):
                        assert target.GetTypeName() == 'RigExecControl'
                        actual_id = target.GetCustomDataByKey('blender:sourceId')
                        assert path in native_switch_targets
                        assert all(target.GetAttribute('avars:' + channel)
                                   for channel in ('tx', 'ty', 'tz', 'rx', 'ry', 'rz', 'sx', 'sy', 'sz'))
                        posed = target.GetAttribute('posed:space')
                        assert not posed or not posed.HasAuthoredConnections(), path
                    else:
                        actual_id = target.GetCustomDataByKey('blender:id')
                    actual_ids.append(actual_id)
                expected_ids = original_button['controls']
                if actual_ids:
                    assert actual_ids == expected_ids
                    expected_label = original_button['label']
                    if (original_button.get('source') or {}).get('binding'):
                        expected_label = 'Select ' + expected_label
                    assert button['label'] == expected_label
                elif expected_ids:
                    assert button['label'] == original_button['label'] + ' (unavailable)'
                    assert any(
                        'picker target is read-only in standard usdRig interaction: ' + ident in message
                        for ident in expected_ids for message in diagnostics), expected_ids
                else:
                    assert not actual_ids
                    if (original_button.get('source') or {}).get('binding'):
                        assert button['label'] == original_button['label'] + ' (unavailable)'
    native = output / 'fixture.usdc'
    image = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=')
    (output / 'picker.png').write_bytes(image)
    for tile in ('1001', '1002'):
        (output / ('skin.' + tile + '.png')).write_bytes(image)
    texture = UsdShade.Shader.Define(stage, '/Rig/UdimPackagingProbe')
    texture.CreateIdAttr('UsdUVTexture')
    texture.CreateInput('file', Sdf.ValueTypeNames.Asset).Set(
        Sdf.AssetPath(str((output / 'skin.<UDIM>.png').resolve())))
    stage.GetPrimAtPath(records[0]['pages'][0]['path']).GetAttribute('ui:backgroundImage').Set(Sdf.AssetPath(str((output / 'picker.png').resolve())))
    assert stage.GetRootLayer().Export(str(native))
    report = package(native, output / 'fixture.usdz')
    assert report['verified'] and report['picker_pages'] == 6
    assert report['authored_time_samples'] == 0
    assert set(report['udim_tiles']['/Rig/UdimPackagingProbe.inputs:file']) == {'1001', '1002'}
    packaged = Usd.Stage.Open(str(output / 'fixture.usdz'))
    asset = packaged.GetPrimAtPath(records[0]['pages'][0]['path']).GetAttribute('ui:backgroundImage').Get()
    assert asset.resolvedPath and '[' in asset.resolvedPath
    assert Ar.GetResolver().OpenAsset(Ar.ResolvedPath(asset.resolvedPath)).GetBuffer() == image
    assert udim_records(packaged, require_internal=True) == report['udim_tiles']
    texture.GetInput('file').Set(Sdf.AssetPath(str((output / 'missing.<UDIM>.png').resolve())))
    try:
        udim_records(stage)
    except ValueError as error:
        assert 'no resolved tiles' in str(error)
    else:
        raise AssertionError('An empty UDIM tile set was accepted')
    print('All source pages, hidden groups, nested bindings and available native targets verified in USDZ')


if __name__ == '__main__':
    main()
