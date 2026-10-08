"""Read-only stock usdRig environment regression for converted control frames.

Source only usdRig/bin/_env.sh; this intentionally does not register any
converter plugin or use the Blender viewer wrapper. Edits live in an anonymous
session layer, and the source asset hash must remain unchanged.
"""
import argparse
import ctypes
import hashlib
import json
import os
import sys
from pathlib import Path

import numpy as np
from pxr import Plug, Tf, Usd, UsdGeom
import rigexec


def loaded_libraries():
    if sys.platform != 'darwin':
        return []
    process = ctypes.CDLL(None)
    process._dyld_image_count.restype = ctypes.c_uint32
    process._dyld_get_image_name.argtypes = [ctypes.c_uint32]
    process._dyld_get_image_name.restype = ctypes.c_char_p
    return [process._dyld_get_image_name(i).decode()
            for i in range(process._dyld_image_count())
            if process._dyld_get_image_name(i) and
            any(n in process._dyld_get_image_name(i).decode()
                for n in ('rigExec', 'usdBlenderRig'))]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('stage', type=Path)
    p.add_argument('report', type=Path)
    p.add_argument('--rig', default='/Rig')
    p.add_argument('--root')
    p.add_argument('--require-core-runtime', action='store_true',
                   help='Require the converter plugin and library to be absent during playback')
    p.add_argument('--reject-legacy-types', action='store_true',
                   help='Require a newly converted asset to use generic shared schema names')
    args = p.parse_args()
    source = args.stage.resolve()
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    stage = Usd.Stage.Open(str(source))
    assert stage
    stage.SetEditTarget(stage.GetSessionLayer())
    rig = rigexec.Rig(stage, args.rig)
    rig.compile()
    saved = rig.evaluate(-1)
    report = {'stage': str(source), 'rig': args.rig, 'completed': False,
              'stock_environment': {'pluginpath': os.environ.get('PXR_PLUGINPATH_NAME'),
                                    'pythonpath': os.environ.get('PYTHONPATH')},
              'saved_valid': bool(saved.valid), 'saved_diagnostics': list(saved.diagnostics)}
    controls = [p for p in stage.Traverse() if p.GetTypeName() == 'RigExecControl']
    frames = []
    for prim in controls:
        frame = saved.control_frame(str(prim.GetPath()))
        matrix = np.asarray(frame.to_matrix4(), dtype=np.float64).reshape(4, 4) if frame else None
        frames.append({'path': str(prim.GetPath()), 'available': frame is not None,
                       'origin': matrix[3, :3].tolist() if frame else None})
    report['controls'] = frames
    report['control_count'] = len(controls)
    report['frames_available'] = sum(f['available'] for f in frames)
    report['controls_at_origin'] = int(sum(f['available'] and np.linalg.norm(f['origin']) <= 1e-6 for f in frames))
    report['controls_away_from_origin'] = int(sum(f['available'] and np.linalg.norm(f['origin']) > 1e-6 for f in frames))
    meshes = [p for p in stage.Traverse() if p.IsA(UsdGeom.Mesh)]

    def points(pose):
        result = {}
        for mesh in meshes:
            value = pose.moved_property(str(mesh.GetPath()) + '.points')
            if value is not None:
                array = np.asarray(value, dtype=np.float64)
                if array.ndim == 2 and array.shape[1] == 3 and len(array):
                    result[str(mesh.GetPath())] = array.copy()
        return result

    original = points(saved)
    if args.root:
        root = stage.GetPrimAtPath(args.root)
    else:
        candidates = []
        for prim in controls:
            ident = prim.GetCustomDataByKey('blender:sourceId')
            if not ident:
                continue
            parts = json.loads(ident)
            if parts[0] == 'bone' and parts[-1].lower() == 'root' and parts[2].upper().startswith('RIG'):
                candidates.append(prim)
        assert len(candidates) == 1, [str(c.GetPath()) for c in candidates]
        root = candidates[0]
    assert root and root.GetTypeName() == 'RigExecControl'
    tx = root.GetAttribute('avars:tx')
    assert tx and not tx.HasAuthoredConnections()
    initial = tx.Get()
    delta = 0.05
    assert tx.Set(initial + delta)
    edited = rig.evaluate(-1)
    moved = points(edited)
    movements = [{'path': path, 'vertices': len(array),
                  'max_displacement': float(np.linalg.norm(moved[path] - array, axis=1).max(initial=0))}
                 for path, array in original.items() if path in moved and moved[path].shape == array.shape]
    stage.GetSessionLayer().Clear()
    restored = rig.evaluate(-1)
    reset = points(restored)
    errors = [float(np.linalg.norm(reset[path] - array, axis=1).max(initial=0))
              for path, array in original.items() if path in reset and reset[path].shape == array.shape]
    report.update({'root': str(root.GetPath()), 'edited_attribute': str(tx.GetPath()),
                   'root_translation_delta': delta, 'edited_valid': bool(edited.valid),
                   'restored_valid': bool(restored.valid),
                   'geometry': movements, 'max_geometry_displacement': max((m['max_displacement'] for m in movements), default=0),
                   'max_restore_error': max(errors, default=0),
                   'restored_meshes': len(errors), 'evaluated_meshes': len(original),
                   'asset_unchanged': hashlib.sha256(source.read_bytes()).hexdigest() == before,
                   'session_empty': stage.GetSessionLayer().empty,
                   'libraries': loaded_libraries(),
                   'plugins': [{'name': p.name, 'path': p.path, 'loaded': bool(p.isLoaded)}
                               for p in Plug.Registry().GetAllPlugins() if 'rig' in p.name.lower()]})
    runtime_images = [p for p in report['libraries'] if Path(p).name == 'librigExec.dylib']
    report['runtime_image_count'] = len(runtime_images)
    converter_plugins = [p for p in report['plugins'] if p['name'] == 'usdBlenderRig']
    converter_images = [p for p in report['libraries'] if 'libusdBlenderRig.' in Path(p).name]
    report['require_core_runtime'] = args.require_core_runtime
    report['converter_plugin_registered'] = bool(converter_plugins)
    report['converter_plugin_loaded'] = any(p['loaded'] for p in converter_plugins)
    report['converter_library_loaded'] = bool(converter_images)
    report['legacy_type_paths'] = [str(p.GetPath()) for p in stage.Traverse()
                                   if p.GetTypeName().startswith('RigExecBlender')]
    owners = {}
    for prim in stage.Traverse():
        typename = str(prim.GetTypeName())
        if not typename.startswith('RigExec') or typename in owners:
            continue
        plugin = Plug.Registry().GetPluginForType(Tf.Type.FindByName(typename))
        owners[typename] = {'plugin': plugin.name, 'path': plugin.path} if plugin else None
    report['schema_owners'] = owners
    report['all_rig_schemas_owned_by_core'] = bool(owners) and all(
        owner and owner['plugin'] == 'rigExecSchema' for owner in owners.values())

    report['completed'] = True
    report['passed'] = bool(saved.valid and edited.valid and restored.valid and
        report['frames_available'] == len(controls) and
        report['controls_away_from_origin'] > len(controls) // 2 and
        report['max_geometry_displacement'] > 0.01 and
        report['max_restore_error'] < 1e-6 and len(errors) == len(original) and original and
        report['asset_unchanged'] and report['session_empty'] and
        (sys.platform != 'darwin' or len(runtime_images) == 1) and
        (not args.require_core_runtime or (not report['converter_plugin_loaded'] and
                                          not converter_images and
                                          report['all_rig_schemas_owned_by_core'])) and
        (not args.reject_legacy_types or not report['legacy_type_paths']))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print('STOCK_CONTROL_RUNTIME', source.name, 'PASS' if report['passed'] else 'FAIL',
          'controls', len(controls), 'away', report['controls_away_from_origin'],
          'geometry', report['max_geometry_displacement'], 'restore', report['max_restore_error'],
          'runtime_images', len(runtime_images), flush=True)
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
