"""Inert migration preserves packaged composition, variants, and asset bytes."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from pxr import Sdf

module_path = Path(__file__).resolve().parents[1] / 'tools' / 'migrate_runtime.py'
spec = importlib.util.spec_from_file_location('migrate_runtime', module_path)
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class MigrationTest(unittest.TestCase):
    def test_root_usdc_changes_only_types(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            source, output = folder / 'old.usdc', folder / 'new.usdc'
            layer = Sdf.Layer.CreateNew(str(source))
            p = Sdf.PrimSpec(layer, 'Bone', Sdf.SpecifierDef, 'RigExecBlenderBoneFrame')
            attr = Sdf.AttributeSpec(p, 'inputs:inheritScale', Sdf.ValueTypeNames.Token)
            attr.default = 'FIX_SHEAR'
            layer.SetTimeSample(attr.path, 1, 'FULL')
            layer.Save()
            before = source.read_bytes()
            report = migration.migrate(source, output)
            converted = Sdf.Layer.FindOrOpen(str(output))
            self.assertEqual(converted.GetPrimAtPath('/Bone').typeName, 'RigExecBoneFrame')
            self.assertEqual(converted.GetAttributeAtPath('/Bone.inputs:inheritScale').default, 'FIX_SHEAR')
            self.assertEqual(converted.QueryTimeSample('/Bone.inputs:inheritScale', 1), 'FULL')
            self.assertEqual(source.read_bytes(), before)
            self.assertEqual(report['renamed_prims'], 1)

    def test_packaged_layers_variants_nested_assets(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder / 'sub').mkdir()
            root = Sdf.Layer.CreateNew(str(folder / 'root.usda'))
            prim = Sdf.PrimSpec(root, 'Rig', Sdf.SpecifierDef, 'Scope')
            variants = Sdf.VariantSetSpec(prim, 'detail')
            variant = Sdf.VariantSpec(variants, 'hidden')
            Sdf.PrimSpec(variant.primSpec, 'Frame', Sdf.SpecifierDef, 'RigExecBlenderConstraintFrame')
            root.subLayerPaths = ['sub/body.usdc']
            root.Save()
            child = Sdf.Layer.CreateNew(str(folder / 'sub/body.usdc'))
            for index, name in enumerate(migration.TYPE_NAMES):
                p = Sdf.PrimSpec(child, 'Node' + str(index), Sdf.SpecifierDef, name)
                a = Sdf.AttributeSpec(p, 'source:kept', Sdf.ValueTypeNames.String)
                a.default = 'unchanged ' + name
            child.Save()
            nested = Sdf.Layer.CreateNew(str(folder / 'nested.usda'))
            Sdf.PrimSpec(nested, 'Parent', Sdf.SpecifierDef, 'RigExecBlenderArmatureParent')
            nested.Save()
            with Sdf.ZipFileWriter.CreateNew(str(folder / 'nested.usdz')) as writer:
                writer.AddFile(str(folder / 'nested.usda'), 'nested.usda')
            pixel = b'pixel-data-must-remain-byte-identical\x00\xff'
            (folder / 'texture.png').write_bytes(pixel)
            source, output = folder / 'old.usdz', folder / 'new.usdz'
            entries = ['root.usda', 'sub/body.usdc', 'nested.usdz', 'texture.png']
            with Sdf.ZipFileWriter.CreateNew(str(source)) as writer:
                for name in entries:
                    writer.AddFile(str(folder / name), name)
            before = hashlib.sha256(source.read_bytes()).hexdigest()
            report = migration.migrate(source, output)
            self.assertEqual(report['renamed_prims'], 9)
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), before)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.namelist(), entries)
                self.assertEqual(archive.read('texture.png'), pixel)
                self.assertNotIn(b'def RigExecBlenderConstraintFrame', archive.read('root.usda'))
                for entry in archive.infolist():
                    with output.open('rb') as stream:
                        stream.seek(entry.header_offset)
                        header = stream.read(30)
                        lengths = struct.unpack_from('<HH', header, 26)
                        self.assertEqual((entry.header_offset + 30 + sum(lengths)) % 64, 0)
            converted = Sdf.Layer.FindOrOpen(str(output) + '[sub/body.usdc]')
            for index, (old, new) in enumerate(migration.TYPE_NAMES.items()):
                self.assertEqual(converted.GetPrimAtPath('/Node' + str(index)).typeName, new)
                self.assertEqual(converted.GetAttributeAtPath('/Node' + str(index) + '.source:kept').default, 'unchanged ' + old)
            changed_nested = Sdf.Layer.FindOrOpen(str(output) + '[nested.usdz[nested.usda]]')
            self.assertEqual(changed_nested.GetPrimAtPath('/Parent').typeName, 'RigExecArmatureParent')

    def test_unsafe_package_keeps_existing_output(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            source, output = folder / 'bad.usdz', folder / 'target.usdz'
            with zipfile.ZipFile(source, 'w') as archive:
                archive.writestr('../escape.usda', '#usda 1.0\n')
            output.write_bytes(b'prior output')
            with self.assertRaisesRegex(ValueError, 'unsafe'):
                migration.migrate(source, output)
            self.assertEqual(output.read_bytes(), b'prior output')

    def test_nested_migration_preserves_similarly_named_sibling(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder / 'root.usda').write_text('#usda 1.0\ndef Scope "Root" {}\n')
            entries = ['root.usda', 'child.usdz', 'child-migrated.usdz']
            for index, name in enumerate(entries[1:]):
                layer = folder / ('nested' + str(index) + '.usda')
                layer.write_text('#usda 1.0\ndef RigExecBlenderBoneFrame "Bone' + str(index) + '" {}\n')
                with Sdf.ZipFileWriter.CreateNew(str(folder / name)) as writer:
                    writer.AddFile(str(layer), layer.name)
            source, output = folder / 'old.usdz', folder / 'new.usdz'
            with Sdf.ZipFileWriter.CreateNew(str(source)) as writer:
                for name in entries:
                    writer.AddFile(str(folder / name), name)
            report = migration.migrate(source, output)
            self.assertEqual(report['renamed_prims'], 2)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.namelist(), entries)
            for index, name in enumerate(entries[1:]):
                layer = Sdf.Layer.FindOrOpen(str(output) + '[' + name + '[nested' + str(index) + '.usda]]')
                self.assertEqual(layer.GetPrimAtPath('/Bone' + str(index)).typeName, 'RigExecBoneFrame')

    def test_ambiguous_entry_paths_preserve_existing_output(self):
        for name in ['a//b.usda', './a.usda', 'a/']:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as folder:
                folder = Path(folder)
                source, output = folder / 'bad.usdz', folder / 'target.usdz'
                with zipfile.ZipFile(source, 'w') as archive:
                    archive.writestr('root.usda', '#usda 1.0\n')
                    archive.writestr(name, '#usda 1.0\n')
                output.write_bytes(b'prior output')
                with self.assertRaises(ValueError):
                    migration.migrate(source, output)
                self.assertEqual(output.read_bytes(), b'prior output')


if __name__ == '__main__':
    unittest.main()
