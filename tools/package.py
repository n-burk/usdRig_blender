#!/usr/bin/env python3
"""Package native USD and its asset dependencies as a verified USDZ.

Uses the general OpenUSD packager so native RigExec schemas and picker pages
remain intact. Requires the same USD/plugin environment as tools/convert.py.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import zipfile

from pxr import Ar, Sdf, Usd, UsdShade, UsdUtils


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def udim_records(stage, require_internal=False):
    """Resolve and hash concrete UDIM tiles; a pattern alone is not an asset."""
    records = {}
    resolver = Ar.GetResolver()
    for prim in stage.Traverse():
        for attr in prim.GetAttributes():
            value = attr.Get()
            if not isinstance(value, Sdf.AssetPath) or not UsdShade.UdimUtils.IsUdimIdentifier(value.path):
                continue
            layers = attr.GetPropertyStack(Usd.TimeCode.Default())
            layer = layers[0].layer if layers else stage.GetRootLayer()
            tiles = UsdShade.UdimUtils.ResolveUdimTilePaths(value.path, layer)
            if not tiles:
                raise ValueError('UDIM pattern has no resolved tiles: ' + str(attr.GetPath()))
            tile_hashes = {}
            for path, tile in tiles:
                if require_internal and not Ar.IsPackageRelativePath(path):
                    raise ValueError('UDIM tile remains outside the package: ' + path)
                resolved = resolver.Resolve(path)
                asset = resolver.OpenAsset(resolved) if resolved else None
                if not asset:
                    raise ValueError('unreadable UDIM tile: ' + path)
                tile_hashes[tile] = hashlib.sha256(asset.GetBuffer()).hexdigest()
            records[str(attr.GetPath())] = tile_hashes
    return records


def picker_exposure(boxes):
    """Share of each button rectangle that a click reaches in the stock picker.

    boxes are (x, y, width, height, depth, shape) of one panel's selecting
    buttons in prim order. The picker hit-tests bounding boxes and gives the
    click to the topmost button: widget controls first, then the larger
    ui:depth, then the later prim. A covered button cannot be clicked, however
    visible its outline is.
    """
    xs = sorted({v for b in boxes for v in (b[0], b[0] + b[2])})
    ys = sorted({v for b in boxes for v in (b[1], b[1] + b[3])})
    order = sorted(range(len(boxes)), key=lambda i: (boxes[i][5] == 'widgetControl', boxes[i][4], i))
    area = [0.0] * len(boxes)
    for x0, x1 in zip(xs, xs[1:]):
        for y0, y1 in zip(ys, ys[1:]):
            x, y = (x0 + x1) / 2, (y0 + y1) / 2
            hit = [i for i in order if boxes[i][0] <= x <= boxes[i][0] + boxes[i][2]
                   and boxes[i][1] <= y <= boxes[i][1] + boxes[i][3]]
            if hit:
                area[hit[-1]] += (x1 - x0) * (y1 - y0)
    return [a / (b[2] * b[3]) if b[2] * b[3] else 0.0 for a, b in zip(area, boxes)]


def picker_records(stage):
    records = []
    for picker in stage.Traverse():
        if picker.GetTypeName() != 'RigExecPicker':
            continue
        pages = []
        for page in picker.GetChildren():
            if page.GetTypeName() != 'RigExecPickerPanel':
                continue
            buttons, boxes = [], []
            for button in page.GetChildren():
                if button.GetTypeName() != 'RigExecPickerButton':
                    continue
                targets = button.GetRelationship('rigExec:picker:controls').GetTargets()
                if any(not stage.GetPrimAtPath(p) or stage.GetPrimAtPath(p).GetTypeName()
                       not in ('RigExecControl', 'RigExecJoint') for p in targets):
                    raise ValueError('unresolved picker target: ' + str(button.GetPath()))
                mirrors = button.GetRelationship('rigExec:picker:mirror').GetTargets()
                if any(not p.HasPrefix(picker.GetPath()) or not stage.GetPrimAtPath(p) for p in mirrors):
                    raise ValueError('unresolved picker mirror: ' + str(button.GetPath()))
                buttons.append({'path': str(button.GetPath()),
                    'label': button.GetAttribute('ui:text').Get(), 'targets': list(map(str, targets)),
                    'mirror': list(map(str, mirrors)), 'clickable': None})
                # The stock picker draws neither a mode-less alternate nor a slider.
                if targets and not button.GetAttribute('ui:alternate').Get() and button.GetAttribute('ui:shape').Get() != 'slider':
                    position, size = button.GetAttribute('ui:position').Get(), button.GetAttribute('ui:size').Get()
                    boxes.append((position[0], position[1], size[0], size[1],
                                  button.GetAttribute('ui:depth').Get(), button.GetAttribute('ui:shape').Get(), buttons[-1]))
                elif targets:
                    buttons[-1]['clickable'] = 0.0
            for box, share in zip(boxes, picker_exposure(boxes)):
                box[-1]['clickable'] = round(share, 6)
            pages.append({'path': str(page.GetPath()), 'label': page.GetAttribute('ui:label').Get(),
                          'order': page.GetAttribute('ui:order').Get(), 'buttons': buttons})
        # Source settings that are not selections are compact picker metadata.
        records.append({'path': str(picker.GetPath()), 'label': picker.GetAttribute('ui:label').Get(),
                        'rig': list(map(str, picker.GetRelationship('rigExec:picker:rig').GetTargets())),
                        'source_kind': picker.GetCustomDataByKey('blender:sourceKind'),
                        'control_count': picker.GetCustomDataByKey('blender:controlCount'),
                        'group_count': picker.GetCustomDataByKey('blender:groupCount'),
                        'unavailable': list(picker.GetCustomDataByKey('blender:unavailable') or []),
                        'unsupported_reason': list(picker.GetCustomDataByKey('blender:unsupportedReason') or []),
                        'pages': pages})
    return records


def package(source, output):
    source, output = Path(source).resolve(), Path(output).resolve()
    if not source.is_file() or source.suffix.lower() not in {'.usd', '.usda', '.usdc'}:
        raise ValueError('source must be an existing USD layer')
    if source == output or output.suffix.lower() != '.usdz':
        raise ValueError('output must be a distinct .usdz file')
    output.parent.mkdir(parents=True, exist_ok=True)
    source_hash = digest(source)
    context = Ar.GetResolver().CreateDefaultContextForAsset(str(source))
    with Ar.ResolverContextBinder(context):
        layers, assets, unresolved = UsdUtils.ComputeAllDependencies(Sdf.AssetPath(str(source)))
        if unresolved:
            raise ValueError('unresolved package dependencies: ' + ', '.join(unresolved))
        stage = Usd.Stage.Open(str(source))
        expected_udims = udim_records(stage)
        expected_pickers = picker_records(stage)
        expected_prims = {str(p.GetPath()): str(p.GetTypeName()) for p in stage.Traverse()}
        expected_connections = {str(a.GetPath()): list(map(str, a.GetConnections()))
                                for p in stage.Traverse() for a in p.GetAttributes() if a.HasAuthoredConnections()}
        with tempfile.TemporaryDirectory(prefix='.usdRig-package-', dir=output.parent) as folder:
            temporary = Path(folder) / output.name
            if not UsdUtils.CreateNewUsdzPackage(Sdf.AssetPath(str(source)), str(temporary),
                                               output.stem + '.usdc', False):
                raise RuntimeError('USDZ package creation failed')
            with zipfile.ZipFile(temporary) as archive:
                if archive.testzip() is not None:
                    raise ValueError('USDZ ZIP CRC verification failed')
                entries = archive.infolist()
                with temporary.open('rb') as stream:
                    for entry in entries:
                        if entry.compress_type != zipfile.ZIP_STORED:
                            raise ValueError('USDZ entries must be uncompressed')
                        stream.seek(entry.header_offset)
                        header = stream.read(30)
                        name_length, extra_length = struct.unpack_from('<HH', header, 26)
                        if (entry.header_offset + 30 + name_length + extra_length) % 64:
                            raise ValueError('USDZ entry is not aligned to 64 bytes')
                if entries[0].filename != output.stem + '.usdc':
                    raise ValueError('unexpected root layer in USDZ')
                names = [entry.filename for entry in entries]
            packaged = Usd.Stage.Open(str(temporary))
            if not packaged or picker_records(packaged) != expected_pickers:
                raise ValueError('picker pages changed during packaging')
            if {str(p.GetPath()): str(p.GetTypeName()) for p in packaged.Traverse()} != expected_prims:
                raise ValueError('native prim types or paths changed during packaging')
            if {str(a.GetPath()): list(map(str, a.GetConnections())) for p in packaged.Traverse()
                for a in p.GetAttributes() if a.HasAuthoredConnections()} != expected_connections:
                raise ValueError('native computation connections changed during packaging')
            _, packaged_assets, missing = UsdUtils.ComputeAllDependencies(Sdf.AssetPath(str(temporary)))
            if missing:
                raise ValueError('packaged dependencies do not resolve: ' + ', '.join(missing))
            if any(not Ar.IsPackageRelativePath(asset) for asset in packaged_assets):
                raise ValueError('package retains an external asset dependency')
            if udim_records(packaged, require_internal=True) != expected_udims:
                raise ValueError('UDIM tiles or their bytes changed during packaging')
            if digest(source) != source_hash:
                raise ValueError('source USD changed during packaging')
            os.replace(temporary, output)
    report = {'source': str(source), 'source_sha256': source_hash, 'output': str(output),
              'sha256': digest(output), 'bytes': output.stat().st_size, 'entries': names,
              'native_prims': len(expected_prims), 'native_connections': len(expected_connections),
              'picker_pages': sum(len(p['pages']) for p in expected_pickers),
              'picker_buttons': sum(len(page['buttons']) for p in expected_pickers for page in p['pages']),
              'picker_controls': len({t for p in expected_pickers for page in p['pages']
                                      for button in page['buttons'] for t in button['targets']}),
              'pickers': expected_pickers, 'source_unchanged': True, 'verified': True,
              'udim_tiles': expected_udims,
              'authored_time_samples': sum(a.GetNumTimeSamples() for p in stage.Traverse() for a in p.GetAttributes())}
    output.with_suffix('.package.json').write_text(json.dumps(report, indent=2))
    print(output)
    print(f"Verified {report['picker_pages']} picker pages, {report['picker_buttons']} buttons, "
          f"{report['picker_controls']} selectable controls, {len(names)} package entries")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    package(args.source, args.output)


if __name__ == '__main__':
    main()
