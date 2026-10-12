"""Verify native picker classification independently of emitted USD controls."""
import importlib.util
import pathlib
import unittest

path = pathlib.Path(__file__).resolve().parents[1] / 'blender' / 'picker.py'
spec = importlib.util.spec_from_file_location('picker_inventory', path)
picker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(picker)


def record(name, shape='', collections=(), locked=False, properties=None):
    return {'name': name, 'shape': shape, 'collections': list(collections),
            'hidden': True, 'locks': {'translation': [locked]*3,
                'rotation': [True]*3, 'scale': [True]*3},
            'properties': properties or {}}


class InventoryTest(unittest.TestCase):
    def test_hidden_and_collection_controls_remain(self):
        # "Props" holds mostly shaped bones, so its unshaped member is a
        # control; "Rigging" holds mostly unshaped bones, so only its shaped
        # member is. No collection is recognised by name.
        inputs = [record('STR-Detail', 'shape', ('Rigging',)),
                  record('FK-Head', 'shape', ('Props',)),
                  record('Button', 'shape', ('Props',)),
                  record('P-Button', collections=('Props',)),
                  record('PRP-Settings', 'shape', locked=True,
                         properties={'Sharp': {'value': .5}}),
                  record('ORG-Mechanism', collections=('Rigging', 'Controls')),
                  record('MCH-Mechanism', collections=('Rigging',)),
                  record('ReadOnly', 'shape', locked=True)]
        self.assertEqual(picker.control_collections(inputs), {'Props'})
        controls, excluded = picker.classify(inputs)
        self.assertEqual([r['name'] for r in controls],
                         ['STR-Detail', 'FK-Head', 'Button', 'P-Button', 'PRP-Settings'])
        self.assertEqual([(r['name'], r['reason']) for r in excluded],
                         [('ORG-Mechanism', 'no custom shape or control collection'),
                          ('MCH-Mechanism', 'no custom shape or control collection'),
                          ('ReadOnly', 'no editable channels')])

    def test_collection_aliases_do_not_duplicate_inventory(self):
        controls, _ = picker.classify([record('FK-Hand', 'shape',
                                      ('Layer 8', 'FK Controls', 'Props'))])
        self.assertEqual(len(controls), 1)
        self.assertEqual(controls[0]['collections'], ['Layer 8', 'FK Controls', 'Props'])

    def test_collection_pages_do_not_depend_on_the_rig_name(self):
        class Collection:
            def __init__(self, name): self.name, self.is_visible_effectively = name, False

        class Shape:
            name = 'WGT'

        class PoseBone(dict):
            def __init__(self, name, collections, shape=True, locked=False, **properties):
                super().__init__(properties)
                self.name, self.custom_shape = name, Shape() if shape else None
                self.bone = type('Bone', (), {'collections': [Collection(c) for c in collections], 'hide': False})()
                self.lock_location = self.lock_rotation = self.lock_scale = [locked] * 3

            def id_properties_ui(self, key):
                return type('UI', (), {'as_dict': lambda self: {'description': key, 'min': 0, 'max': 1}})()

        bones = [PoseBone('FK-Arm.L', ['FK Controls'], ik_fk=1.0)] + \
                [PoseBone('STR-%02d' % i, ['Stretch Controls']) for i in range(50)] + \
                [PoseBone('ORG-Arm.L', ['Original Bones'], shape=False),
                 PoseBone('Properties_Character_Any', [], shape=False, locked=True, quality=2)]
        obj = type('Object', (), {'name': 'RIG-AnyCharacter', 'pose': type('Pose', (), {'bones': bones})()})()
        data = picker.collection_picker(obj, lambda o: 'object:' + o.name, lambda o, n: 'bone:' + n)
        # Plain source pages plus inventory; layout and page size are the
        # translator's decision, so a collection is one page however large.
        self.assertNotIn('layout', data)
        self.assertEqual((data['owner'], data['name']), ('object:RIG-AnyCharacter', 'RIG-AnyCharacter'))
        self.assertEqual([(p['name'], len(p['buttons'])) for p in data['pages']],
                         [('FK Controls', 1), ('Stretch Controls', 50), ('Source settings (unavailable)', 2)])
        self.assertEqual(data['pages'][1]['source'], {'collection': 'Stretch Controls'})
        self.assertEqual(data['pages'][0]['buttons'][0], {'label': 'FK-Arm.L', 'controls': ['bone:FK-Arm.L']})
        # The same bones classify the same under any collection names.
        for bone in bones:
            for collection in bone.bone.collections:
                collection.name = {'FK Controls': 'A', 'Stretch Controls': 'B', 'Original Bones': 'C'}[collection.name]
        renamed = picker.collection_picker(obj, lambda o: 'object:' + o.name, lambda o, n: 'bone:' + n)
        self.assertEqual([(p['name'], len(p['buttons'])) for p in renamed['pages']],
                         [('A', 1), ('B', 50), ('Source settings (unavailable)', 2)])
        self.assertEqual(renamed['inventory']['controls'], [dict(c, collections=[
            {'FK Controls': 'A', 'Stretch Controls': 'B'}[n] for n in c['collections']]) for c in data['inventory']['controls']])
        settings = data['pages'][-1]['buttons']
        self.assertEqual([b['label'] for b in settings], ['FK-Arm.L: ik_fk', 'Properties_Character_Any: quality'])
        self.assertTrue(all(not b['controls'] and b['source']['unsupported_reason'] for b in settings))
        self.assertEqual(data['inventory']['transformControls'], 51)
        self.assertEqual([e['name'] for e in data['inventory']['excluded']], ['ORG-Arm.L', 'Properties_Character_Any'])
        # A rig with neither controls nor settings gets no picker at all.
        empty = type('Object', (), {'name': 'RIG-Empty', 'pose': type('Pose', (), {'bones': [bones[-2]]})()})()
        self.assertIsNone(picker.collection_picker(empty, lambda o: o.name, lambda o, n: n))


if __name__ == '__main__':
    unittest.main()
