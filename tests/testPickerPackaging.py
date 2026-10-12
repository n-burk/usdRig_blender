"""Check Rigify/CloudRig page extraction, the biped picker mapping and USDZ preservation.

CTest runs this as pickerPackaging after blenderFixture. By hand, in the host
USD/RigExec Python environment with the plugin registered:
    python tests/testPickerPackaging.py build/blender-proof build/picker-proof
"""
import json
import base64
from pathlib import Path
import sys

from pxr import Ar, Sdf, Usd, UsdShade

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from package import package, picker_exposure, picker_records, udim_records


def template_roles():
    """Single-role slots of the packaged biped template: (panel, button) -> role."""
    layer = Sdf.Layer.FindOrOpen(str(Path(__file__).resolve().parents[1] / 'resources' / 'biped_picker.usda'))
    roles = {}
    for panel in layer.GetPrimAtPath('/Template').nameChildren:
        for button in panel.nameChildren:
            data = button.customData.get('blender', {})
            # Role keys only: the template names no source bone.
            assert set(data.keys()) <= {'roles'}, button.path
            names = list(data.get('roles', []))
            if len(names) == 1:
                roles[(panel.name, button.name)] = names[0]
    return roles


def template_clickable():
    """Clickable share of every role slot when all slots of a template panel are live."""
    layer = Sdf.Layer.FindOrOpen(str(Path(__file__).resolve().parents[1] / 'resources' / 'biped_picker.usda'))
    shares = {}
    for panel in layer.GetPrimAtPath('/Template').nameChildren:
        slots = [b for b in panel.nameChildren if b.customData.get('blender', {}).get('roles')]

        def value(button, name, fallback):
            # Fallbacks of the RigExecPickerButton schema.
            attribute = button.attributes.get(name)
            return fallback if attribute is None or attribute.default is None else attribute.default
        for button in slots:
            assert not value(button, 'ui:alternate', False) and value(button, 'ui:shape', 'rectangle') != 'slider', button.path
        boxes = [(*value(b, 'ui:position', (0, 0)), *value(b, 'ui:size', (10, 10)),
                  value(b, 'ui:depth', 0.0), value(b, 'ui:shape', 'rectangle')) for b in slots]
        shares.update({(panel.name, b.name): share for b, share in zip(slots, picker_exposure(boxes))})
    return shares


def main():
    fixture, output = map(Path, sys.argv[1:])
    # The stock picker gives a click to the topmost bounding box. The native
    # layout stacks each IK slot on its FK slot behind a mode dial: the IK foot
    # one unit off the FK foot, the knee pole inside the FK thigh. Without the
    # dial the lower button is dead, so the rule must report it.
    foot, pole = picker_exposure([(216, 402, 27, 27, 0.0331, 'rectangle'), (217, 401, 27, 27, 0.0334, 'rectangle')]), \
        picker_exposure([(221, 331, 17, 10, 0.0332, 'circle'), (217, 293, 27, 50, 0.0336, 'rectangle')])
    assert foot[0] < 0.08 and foot[1] == 1 and pole == [0, 1], (foot, pole)
    assert picker_exposure([(0, 0, 10, 10, 0, 'rectangle'), (0, 0, 10, 10, 0, 'rectangle')]) == [0, 1]
    assert picker_exposure([(0, 0, 10, 10, 0, 'widgetControl'), (0, 0, 10, 10, 1, 'rectangle')]) == [1, 0]
    # Worst case for the packaged template: every role slot live at once.
    shares = template_clickable()
    assert len(shares) == 326 and min(shares.values()) >= 0.25, sorted(shares.items(), key=lambda item: item[1])[:4]
    for side in 'LR':
        for fk, ik in (('Hand', 'ArmIK'), ('Foot', 'LegIK'), ('UpLeg', 'LegPV')):
            assert min(shares[('Body', 'b_%s_%s' % (side, role))] for role in (fk, ik)) >= 0.5, (side, fk, ik)
    output.mkdir(parents=True, exist_ok=True)
    snapshot = json.loads((fixture / 'rig.blendrig').read_text())
    render_mesh = next(m for m in snapshot['meshes'] if m.get('uv_active') == 'EditingUV')
    assert render_mesh['uv_render'] == 'UVMap'
    assert render_mesh['uv'] == render_mesh['uv_sets']['UVMap']
    assert render_mesh['uv'] != render_mesh['uv_sets']['EditingUV']
    source = snapshot['pickers']
    assert len(source) == 2
    # Extraction emits plain source pages; the translator alone decides layout.
    assert all('layout' not in p for p in source)
    rigify = next(p for p in source if p['source_kind'] == 'Rigify control collections')
    assert [p['name'] for p in rigify['pages']] == ['Main', 'Hand']
    assert rigify['pages'][1]['source']['visible'] is False
    # The fixture stores the Hand row the way Blender 5 stores an add-on's
    # bpy.props value, as a system property, and the Main row as a custom property.
    assert [p['source']['row'] for p in rigify['pages']] == [1, 2]
    # A generated rig that stores no UI rows, read from real Blender objects:
    # the collection whose bones mostly carry a shape gives the page, its
    # unshaped member is a control, and a numeric property is a setting.
    fallback = json.loads((fixture / 'collections-picker.json').read_text())
    assert len(fallback) == 1 and fallback[0]['source_kind'] == 'Control collections' and not fallback[0]['source_ui']
    assert [(p['name'], [b['label'] for b in p['buttons']]) for p in fallback[0]['pages']] == [
        ('Controls', ['Lever', 'Slide', 'Unshaped']), ('Source settings (unavailable)', ['Lever: amount'])]
    assert all(len(b['controls']) == 1 for b in fallback[0]['pages'][0]['buttons'])
    assert not fallback[0]['pages'][1]['buttons'][0]['controls']
    assert [(e['name'], e['reason']) for e in fallback[0]['inventory']['excluded']] == [
        ('Locked', 'no editable channels'), ('Mechanism', 'no custom shape or control collection')]
    cloud = next(p for p in source if p['source_kind'] == 'CloudRig sidebar panels')
    assert [p['name'] for p in cloud['pages']] == ['Settings', 'Groups', 'Controls: Controls', 'Controls: Nested']
    assert [b['label'] for b in cloud['pages'][0]['buttons']] == ['Target', 'Nested target']
    # The "All" binding names a collection of two bones, one in a nested
    # child: a source button that selects a group.
    group = cloud['pages'][1]['buttons'][0]['controls']
    assert len(group) == 2 and group == [b['controls'][0] for b in cloud['pages'][2]['buttons']]
    assert cloud['pages'][3]['buttons'][0]['controls'] == [group[1]]
    stage = Usd.Stage.Open(str(fixture / 'rig.blendrig'))
    records = picker_records(stage)
    diagnostics = stage.GetDefaultPrim().GetAttribute('blender:diagnostics').Get() or []
    native_switch_targets = {
        str(target)
        for prim in stage.Traverse() if prim.GetTypeName() == 'RigExecSpaceSwitch'
        for target in prim.GetRelationship('rigExec:target').GetTargets()
    }
    roles = template_roles()
    by_kind = {p['source_kind']: p for p in records}
    assert set(by_kind) == {p['source_kind'] for p in source}
    # Snapshot JSON is not copied onto picker prims.
    for prim in Usd.PrimRange(stage.GetPrimAtPath('/Rig/Pickers')):
        data = prim.GetCustomData()
        assert 'source' not in data.get('blender', {}) and 'controlInventory' not in data.get('blender', {}), prim.GetPath()
    for original in source:
        picker = by_kind[original['source_kind']]
        assert picker['label'] == original['name'] and picker['rig'] == ['/Rig']
        # Selection coverage: exactly the controls the source pages offer.
        # Every fixture target is editable, so no source button is refused.
        expected = {c for page in original['pages'] for b in page['buttons'] for c in b['controls']}
        assert not any('picker target is read-only' in m and c in m for m in diagnostics for c in expected)
        assert picker['control_count'] == len(expected)
        assert not picker['unavailable'] and not picker['unsupported_reason']
        single, groups = {}, []
        for page in picker['pages']:
            name = page['path'].rsplit('/', 1)[-1]
            template = name in ('Body', 'Facial')
            assert template or name.startswith('page_'), name
            for button in page['buttons']:
                prim = stage.GetPrimAtPath(button['path'])
                if not button['targets']:
                    # Only template artwork has no selection; never a dead source button.
                    assert template and not button['label'], button['path']
                    continue
                assert button['clickable'] >= 0.25, 'covered live button: ' + button['path']
                assert not prim.GetCustomDataByKey('blender:unavailable')
                for path in button['targets']:
                    target = stage.GetPrimAtPath(path)
                    assert target and target.GetTypeName() == 'RigExecControl', path
                    ident = target.GetCustomDataByKey('blender:sourceId')
                    assert ident in expected, path
                    spaces = target.GetRelationship('rigExec:channelSpaces').GetTargets()
                    assert path in native_switch_targets or len(spaces) == 2
                    assert all(target.GetAttribute('avars:' + channel)
                               for channel in ('tx', 'ty', 'tz', 'rx', 'ry', 'rz', 'sx', 'sy', 'sz'))
                    posed = target.GetAttribute('posed:space')
                    assert spaces or not posed or not posed.HasAuthoredConnections(), path
                if not template and len(button['targets']) > 1:
                    # A source group button: its whole source set, labelled as in the source.
                    groups.append((page['label'], button['label'], [
                        stage.GetPrimAtPath(t).GetCustomDataByKey('blender:sourceId') for t in button['targets']]))
                if len(button['targets']) == 1:
                    ident = stage.GetPrimAtPath(button['targets'][0]).GetCustomDataByKey('blender:sourceId')
                    # Overflow de-duplication: a control with a biped slot has no
                    # overflow entry, and no control has two overflow entries.
                    assert template or ident not in single, ident
                    single.setdefault(ident, (name, button['path'].rsplit('/', 1)[-1], page['label'], button['label']))
        assert set(single) == expected
        assert picker['group_count'] == len(groups)
        placed = {json.loads(i)[-1]: (roles[(v[0], v[1])], v[0]) for i, v in single.items() if (v[0], v[1]) in roles}
        overflow = {json.loads(i)[-1]: v[2] for i, v in single.items() if v[0].startswith('page_')}
        assert all(v[3] == json.loads(i)[-1] for i, v in single.items() if v[0].startswith('page_'))
        if original is rigify:
            # Rigify "Root" fills the native Main slot; the hidden Hand
            # collection keeps its control on an overflow page of that name.
            assert placed == {'Root': ('Main', 'Body')}, placed
            assert overflow == {'Tip.L': 'Hand'}, overflow
            assert not groups
            assert [p['label'] for p in picker['pages']] == ['Body', 'Hand']
            assert [p['order'] for p in picker['pages']] == [0, 2]
        else:
            # No generator names: no template panel is authored. Each control
            # sits once on the first collection page that lists it alone, so
            # the Settings page, whose bindings only name them, has no panel.
            # The collection binding keeps its group button on its own page.
            assert not placed
            assert overflow == {'Target': 'Controls: Controls', 'Parent': 'Controls: Controls'}, overflow
            assert [p['label'] for p in picker['pages']] == ['Groups', 'Controls: Controls']
            assert groups == [('Groups', 'All', group)], groups
            for label in ('Target', 'Nested target', 'All'):
                assert 'picker binding is selection-only; settings and operators are not translated: ' + label in diagnostics
    assert sum(len(p['pages']) for p in records) == 4
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
    assert report['verified'] and report['picker_pages'] == 4
    assert report['picker_controls'] == 4
    assert report['authored_time_samples'] == 0
    assert set(report['udim_tiles']['/Rig/UdimPackagingProbe.inputs:file']) == {'1001', '1002'}
    packaged = Usd.Stage.Open(str(output / 'fixture.usdz'))
    assert picker_records(packaged) == report['pickers']
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
    print('Biped slots, de-duplicated overflow pages and every source control target verified in USDZ')


if __name__ == '__main__':
    main()
