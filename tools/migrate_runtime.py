"""Inert legacy USD type-name migration into the shared usdRig schemas.

No rig is evaluated. USDZ entry names, order, composition, and asset bytes are
preserved; every USD layer, including variant specs and nested packages, is
visited. Input files are never modified. USDC/USD roots are supported directly.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import tempfile
import zipfile

from pxr import Ar, Sdf, UsdUtils

TYPE_NAMES = {
    'RigExecBlenderBoneFrame': 'RigExecBoneFrame',
    'RigExecBlenderConstraintFrame': 'RigExecConstraintFrame',
    'RigExecBlenderCopyTransforms': 'RigExecCopyFrame',
    'RigExecBlenderMappedFrame': 'RigExecMappedFrame',
    'RigExecBlenderSkinInfluence': 'RigExecSkinInfluence',
    'RigExecBlenderArmatureParent': 'RigExecArmatureParent',
    'RigExecBlenderArmatureMover': 'RigExecLayeredSkinMover',
}
USD_EXTENSIONS = {'.usd', '.usda', '.usdc'}


def digest(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prim_specs(layer):
    specs = []
    def visit(path):
        obj = layer.GetObjectAtPath(path)
        if isinstance(obj, Sdf.PrimSpec):
            specs.append(obj)
    layer.Traverse(Sdf.Path.absoluteRootPath, visit)
    return specs


def rename_layer(layer, name, records):
    changes = [(prim, prim.typeName, TYPE_NAMES[prim.typeName])
               for prim in prim_specs(layer) if prim.typeName in TYPE_NAMES]
    before = layer.ExportToString()
    for prim, old, new in changes:
        prim.typeName = new
    # Reversing only the renamed fields must reproduce the complete authored
    # layer, proving attributes, relationships, variants, and metadata intact.
    for prim, old, new in changes:
        prim.typeName = old
    if layer.ExportToString() != before:
        raise ValueError('migration changed authored fields beyond type names: ' + name)
    for prim, old, new in changes:
        prim.typeName = new
        records.append({'layer': name, 'path': str(prim.path), 'old': old, 'new': new})
    return bool(changes)


def export_layer(layer, destination, binary):
    args = {'format': 'usdc' if binary else 'usda'} if destination.suffix == '.usd' else {}
    if not layer.Export(str(destination), args=args):
        raise RuntimeError('cannot export layer: ' + str(destination))


def safe_entry(name):
    path = PurePosixPath(name)
    if not name or not path.parts or path.is_absolute() or '..' in path.parts or '\\' in name or ':' in path.parts[0] or str(path) != name:
        raise ValueError('unsafe USDZ entry path: ' + name)
    return path


def migrate_package(source, destination, records, label=''):
    with tempfile.TemporaryDirectory(prefix='usdRig-package-migration-', dir=destination.parent) as directory:
        folder = Path(directory)
        files = []
        hashes = {}
        with zipfile.ZipFile(source) as archive:
            if archive.testzip():
                raise ValueError('damaged USDZ package: ' + str(source))
            seen = set()
            for entry in archive.infolist():
                if entry.is_dir():
                    raise ValueError('explicit USDZ directory entries are unsupported: ' + entry.filename)
                relative = safe_entry(entry.filename)
                if relative in seen:
                    raise ValueError('duplicate USDZ entry: ' + entry.filename)
                seen.add(relative)
                if (entry.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError('USDZ symlink entry is unsupported: ' + entry.filename)
                target = folder.joinpath(*relative.parts)
                if target.exists():
                    raise ValueError('colliding USDZ entry path: ' + entry.filename)
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(entry) as incoming, target.open('wb') as outgoing:
                    shutil.copyfileobj(incoming, outgoing)
                files.append((entry.filename, target))
                hashes[entry.filename] = digest(target)
        if not files or files[0][1].suffix.lower() not in USD_EXTENSIONS:
            raise ValueError('USDZ first entry must be its USD root layer')
        for name, path in files:
            nested_label = label + name
            if path.suffix.lower() == '.usdz':
                # Keep intermediate packages outside the extracted tree: an
                # authored sibling can have any otherwise-valid file name.
                with tempfile.TemporaryDirectory(prefix='usdRig-nested-migration-', dir=destination.parent) as nested_folder:
                    nested = Path(nested_folder) / 'result.usdz'
                    migrate_package(path, nested, records, nested_label + '[')
                    os.replace(nested, path)
            elif path.suffix.lower() in USD_EXTENSIONS:
                layer = Sdf.Layer.FindOrOpen(str(path))
                if not layer:
                    raise ValueError('unreadable USD layer: ' + nested_label)
                binary = path.read_bytes()[:8] == b'PXR-USDC'
                if rename_layer(layer, nested_label, records):
                    export_layer(layer, path, binary)
            elif digest(path) != hashes[name]:
                raise ValueError('asset bytes changed: ' + nested_label)
        with Sdf.ZipFileWriter.CreateNew(str(destination)) as writer:
            for name, path in files:
                if writer.AddFile(str(path), name) != name:
                    raise RuntimeError('cannot preserve USDZ entry: ' + name)
        with zipfile.ZipFile(destination) as archive:
            if archive.testzip() or archive.namelist() != [name for name, path in files]:
                raise ValueError('repacked USDZ entries differ')
            for name, path in files:
                if path.suffix.lower() not in USD_EXTENSIONS | {'.usdz'}:
                    if hashlib.sha256(archive.read(name)).hexdigest() != hashes[name]:
                        raise ValueError('repacked asset bytes differ: ' + name)


def migrate_root(source, destination, records, output_parent):
    layer = Sdf.Layer.FindOrOpen(str(source))
    if not layer:
        raise ValueError('unreadable USD root layer')
    # External layers are kept as references, rather than flattening away
    # variants. A legacy dependency must be migrated separately or packaged.
    layers, assets, unresolved = UsdUtils.ComputeAllDependencies(Sdf.AssetPath(str(source)))
    if unresolved:
        raise ValueError('unresolved source dependencies: ' + ', '.join(unresolved))
    for dependency in layers:
        if dependency.identifier != layer.identifier and any(p.typeName in TYPE_NAMES for p in prim_specs(dependency)):
            raise ValueError('external layer requires migration; migrate its USDZ or layer first: ' + dependency.identifier)
    binary = source.read_bytes()[:8] == b'PXR-USDC'
    rename_layer(layer, source.name, records)
    if source.parent != output_parent:
        # Reanchor asset-valued fields when moving a root to another folder.
        def anchor(path):
            return Sdf.ComputeAssetPathRelativeToLayer(layer, path) if path else path
        UsdUtils.ModifyAssetPaths(layer, anchor)
    export_layer(layer, destination, binary)


def migrate(source, output):
    source, output = Path(source).resolve(), Path(output).resolve()
    if not source.is_file() or source.suffix.lower() not in USD_EXTENSIONS | {'.usdz'}:
        raise ValueError('source must be an existing USD/USDC/USDZ file')
    if source == output:
        raise ValueError('output must differ from source; preserve the input or a backup')
    if output.suffix.lower() != source.suffix.lower():
        raise ValueError('output must retain the source file extension')
    output.parent.mkdir(parents=True, exist_ok=True)
    source_hash = digest(source)
    records = []
    with tempfile.TemporaryDirectory(prefix='usdRig-runtime-migration-', dir=output.parent) as folder:
        temporary = Path(folder) / output.name
        if source.suffix.lower() == '.usdz':
            migrate_package(source, temporary, records)
        else:
            migrate_root(source, temporary, records, output.parent)
        if digest(source) != source_hash:
            raise ValueError('source changed during migration')
        os.replace(temporary, output)
    report = {'source': str(source), 'source_sha256': source_hash, 'output': str(output),
              'sha256': digest(output), 'renamed_prims': len(records), 'changes': records,
              'source_unchanged': True, 'evaluated': False, 'verified': True}
    output.with_suffix('.migration.json').write_text(json.dumps(report, indent=2) + '\n')
    print('RUNTIME_TYPES_MIGRATED', len(records), output, flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    migrate(args.source, args.output)
