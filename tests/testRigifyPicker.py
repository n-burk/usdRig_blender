"""Check the picker of a rig the bundled Rigify generates against its geometry.

    python tests/testRigifyPicker.py build/blender-proof

Reads rigify-human.blendrig and rigify-human.json written by CTest's
rigifyFixture, in the host USD/RigExec Python environment. The role table
names generated controls; this ties those names to where the generator puts
them: a slot drawn further out, or lower, on the biped panel must select a
control that is further out, or lower, on the rig. Exits 77 without a fixture.
"""
import json
from pathlib import Path
import sys

from pxr import Sdf, Usd

# Roles ordered from the centre line outwards, and from top to bottom.
OUTWARD = (('Shldr', 'UpArm', 'LoArm', 'Hand'), ('ArmBend0', 'ArmBend1', 'ArmBend2', 'ArmBend3', 'ArmBend4'),
           ('InBrow', 'Brow', 'MidBrow'), ('InLid', 'Eye', 'OutLid'), ('UpLip1', 'OutLip'), ('CheekInner', 'Cheek'))
DOWNWARD = (('M_Head', 'M_Neck'), ('M_SpineBend3', 'M_SpineBend2', 'M_SpineBend1'), ('L_UpLeg', 'L_LoLeg', 'L_Foot'),
            ('L_LegBend0', 'L_LegBend1', 'L_LegBend2', 'L_LegBend3', 'L_LegBend4'), ('L_UpLid', 'L_LoLid'),
            ('M_UpLipMid', 'M_LoLipMid'), ('L_UpLip1', 'L_LoLip1'), ('M_NoseBridge', 'M_Nose'),
            ('teeth_upper_def', 'teeth_lower_def'))


def multiply(a, b):
    # Snapshot matrices are flat and multiply row vectors.
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4)) for r in range(4) for c in range(4)]


def main():
    fixture = Path(sys.argv[1])
    if not (fixture / 'rigify-human.blendrig').exists():
        print('No Rigify fixture: skipped')
        sys.exit(77)
    generated = json.loads((fixture / 'rigify-human.json').read_text())
    snapshot = json.loads((fixture / 'rigify-human.blendrig').read_text())
    # Extraction, in a Blender without the add-on: one page per UI collection, in sidebar row order.
    source = next(p for p in snapshot['pickers'] if p['name'] == generated['rig'])
    assert source['source_kind'] == 'Rigify control collections' and source['source_ui'], source['source_kind']
    pages = [(p['source']['collection'], p['source']['row']) for p in source['pages']]
    assert sorted(pages) == sorted(generated['rows'].items()), pages
    assert [row for _, row in pages] == sorted(generated['rows'].values()), pages
    nodes = {n['id']: n for n in snapshot['nodes']}
    frames = {}

    def rest(ident):
        if ident not in frames:
            node = nodes[ident]
            frames[ident] = multiply(node['rest'], rest(node['parent'])) if node.get('parent') in nodes else list(node['rest'])
        return frames[ident]
    template = Sdf.Layer.FindOrOpen(str(Path(__file__).resolve().parents[1] / 'resources' / 'biped_picker.usda'))
    stage = Usd.Stage.Open(str(fixture / 'rigify-human.blendrig'))
    picker = stage.GetPrimAtPath('/Rig/Pickers').GetChild(generated['rig'])
    assert picker and picker.GetCustomDataByKey('blender:sourceKind') == 'Rigify control collections'
    offered = {c for page in source['pages'] for b in page['buttons'] for c in b['controls']}
    assert picker.GetCustomDataByKey('blender:controlCount') == len(offered)
    assert not picker.GetCustomDataByKey('blender:unavailable')
    # Overflow panels carry collection names, never a bare "(Tweak)" title.
    labels = [p.GetAttribute('ui:label').Get() for p in picker.GetChildren() if p.GetName().startswith('page_')]
    assert labels and all(label.split(' (more)')[0].split(' 1/')[0].split(' 2/')[0] in generated['rows'] for label in labels), labels
    roles = {}
    for panel in picker.GetChildren():
        for button in panel.GetChildren():
            spec = template.GetPrimAtPath('/Template/%s/%s' % (panel.GetName(), button.GetName()))
            keys = list(spec.customData.get('blender', {}).get('roles', [])) if spec else []
            targets = button.GetRelationship('rigExec:picker:controls').GetTargets()
            if len(keys) != 1 or len(targets) != 1:
                continue
            ident = stage.GetPrimAtPath(targets[0]).GetCustomDataByKey('blender:sourceId')
            assert ident in offered, ident
            position = button.GetAttribute('ui:position').Get() or (0, 0)
            size = button.GetAttribute('ui:size').Get() or (10, 10)
            world = rest(ident)
            # A role may own a slot on both panels; its control is the same.
            entry = roles.setdefault(keys[0], {'name': json.loads(ident)[-1], 'world': world[12:15], 'slots': {}})
            assert entry['name'] == json.loads(ident)[-1], keys[0]
            # Of several slots on one panel, the one named after the role is the control's own.
            if button.GetName() == 'b_' + keys[0] or panel.GetName() not in entry['slots']:
                entry['slots'][panel.GetName()] = (position[0] + size[0] / 2, position[1] + size[1] / 2)
    assert picker.GetCustomDataByKey('blender:bipedCount') == len(roles) and len(roles) >= 100, len(roles)
    # Left is +X in Blender, and each side selects the mirrored control.
    for role, entry in roles.items():
        if role[:2] in ('L_', 'R_'):
            assert (entry['world'][0] > 0) == (role[0] == 'L'), (role, entry['name'], entry['world'])
            peer = roles.get({'L': 'R_', 'R': 'L_'}[role[0]] + role[2:])
            mirrored = entry['name'].replace('.' + role[0], '.' + {'L': 'R', 'R': 'L'}[role[0]])
            assert peer and peer['name'] == mirrored, (role, entry['name'])

    def shared_panel(chain):
        panels = set.intersection(*(set(roles[r]['slots']) for r in chain))
        assert panels, chain
        return sorted(panels)[-1]
    for chain in OUTWARD:
        left, right = ['L_' + r for r in chain], ['R_' + r for r in chain]
        assert all(r in roles for r in left + right), chain
        panel = shared_panel(left + right)
        drawn = [roles[l]['slots'][panel][0] - roles[r]['slots'][panel][0] for l, r in zip(left, right)]
        placed = [roles[l]['world'][0] - roles[r]['world'][0] for l, r in zip(left, right)]
        assert drawn == sorted(drawn) and placed == sorted(placed) and len(set(placed)) == len(placed), (chain, drawn, placed)
    for chain in DOWNWARD:
        assert all(r in roles for r in chain), chain
        panel = shared_panel(chain)
        drawn = [roles[r]['slots'][panel][1] for r in chain]
        placed = [-roles[r]['world'][2] for r in chain]
        assert drawn == sorted(drawn) and placed == sorted(placed) and len(set(placed)) == len(placed), (chain, drawn, placed)
    print('Rigify human: %d UI collections, %d controls, %d biped roles consistent with the generated geometry' % (
        len(pages), len(offered), len(roles)))


if __name__ == '__main__':
    main()
