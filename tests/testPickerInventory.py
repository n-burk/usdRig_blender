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
        inputs = [record('STR-Detail', 'shape', ('Stretch Helpers',)),
                  record('FK-Head', 'shape', ('FK Controls',)),
                  record('P-Button', collections=('Clothes Main',)),
                  record('PRP-Settings', 'shape', locked=True,
                         properties={'Sharp': {'value': .5}}),
                  record('ORG-Mechanism', collections=('Mechanism Bones',)),
                  record('ReadOnly', 'shape', locked=True)]
        controls, excluded = picker.classify(inputs)
        self.assertEqual([r['name'] for r in controls],
                         ['STR-Detail', 'FK-Head', 'P-Button', 'PRP-Settings'])
        self.assertEqual([r['reason'] for r in excluded],
                         ['no custom shape or explicit control collection',
                          'no editable channels'])

    def test_collection_aliases_do_not_duplicate_inventory(self):
        controls, _ = picker.classify([record('FK-Hand', 'shape',
                                      ('Layer 8', 'FK Controls', 'Clothes Main'))])
        self.assertEqual(len(controls), 1)
        self.assertEqual(controls[0]['collections'], ['Layer 8', 'FK Controls', 'Clothes Main'])


if __name__ == '__main__':
    unittest.main()
