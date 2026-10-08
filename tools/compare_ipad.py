"""Prepare physical usdview iPad manifests and compare captured Hydra poses.

Requires the configured OpenUSD Python environment and NumPy. `prepare`
accepts a native stage, its desktop comparison JSON, an optional independent
Blender reference.json, and a fixture directory. Copy the generated manifests
and USDZ files to device Documents. The iOS DEBUG -ipadRigComparison hook
writes lossless published point/frame captures; `compare` consumes a copied
RigComparisonOutput directory. This tool never evaluates or authors rigs.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from pxr import Usd, UsdGeom

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests'))
from compareRigReference import compare_pose, resolve_edit_attribute


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + '\n')


def prepare(args):
    stage = Usd.Stage.Open(str(args.stage.resolve()))
    if not stage:
        raise ValueError('cannot open native stage')
    desktop = json.loads(args.desktop.read_text())
    name = args.desktop.stem
    reference = json.loads(args.reference.read_text()) if args.reference else None
    providers = {p.GetCustomDataByKey('blender:id'): p for p in stage.Traverse()
                 if p.GetTypeName() in {'RigExecJoint', 'RigExecControl'}
                 and p.GetCustomDataByKey('blender:id')}
    edit_providers = {}
    for ident, provider in providers.items():
        relation = provider.GetRelationship('blender:channelControl')
        targets = relation.GetTargets() if relation else []
        edit_providers[ident] = stage.GetPrimAtPath(targets[0]) if len(targets) == 1 else provider
    meshes = {p.GetCustomDataByKey('blender:id'): p for p in stage.Traverse()
              if p.IsA(UsdGeom.Mesh) and p.GetCustomDataByKey('blender:id')}
    poses = []
    bones = set()
    measured_drags = [row for row in desktop['drags'] if row.get('control')]
    if reference:
        for pose in reference['poses']:
            edits = {str(resolve_edit_attribute(edit, edit_providers, meshes).GetPath()): edit['value']
                     for edit in pose['edits']}
            poses.append({'name': pose['name'], 'edits': edits})
            bones.update(str(providers[ident].GetPath()) for ident in pose['bones'] if ident in providers)
    else:
        bones.update(row['control'] for row in measured_drags)
    controls = []
    for row in measured_drags:
        controls.append({'workload': row['workload'], 'path': row['control'],
                         'setup': row.get('workload_setup', {})})
    file_name = 'Biped.usdz' if name == 'Biped' else 'Blender-' + name + '.usdz'
    manifest = {'name': name, 'stage': file_name, 'frame': stage.GetStartTimeCode(),
                'controls': controls, 'poses': poses, 'bones': sorted(bones),
                'meshes': [{'path': str(p.GetPath()), 'vertices': len(UsdGeom.Mesh(p).GetPointsAttr().Get() or [])}
                           for p in stage.Traverse() if p.IsA(UsdGeom.Mesh)]}
    write(args.output / (name + '.json'), manifest)
    write(args.output / (name + '-metadata.json'), {
        'stage': str(args.stage.resolve()), 'desktop': str(args.desktop.resolve()),
        'reference': str(args.reference.resolve()) if args.reference else None,
        'source_sha256': hashlib.sha256(args.stage.read_bytes()).hexdigest(), 'manifest': manifest})
    print(name, 'controls', len(controls), 'poses', len(poses), 'frames', len(bones))


def compare(args):
    metadata = json.loads(args.metadata.read_text())
    stage_path = Path(metadata['stage'])
    if hashlib.sha256(stage_path.read_bytes()).hexdigest() != metadata['source_sha256']:
        raise ValueError('source stage changed since manifest preparation')
    stage = Usd.Stage.Open(str(stage_path))
    desktop = json.loads(Path(metadata['desktop']).read_text())
    report = json.loads((args.capture / 'report.json').read_text())
    if not report['completed'] or not report['interaction_passed']:
        raise ValueError('device capture did not complete: ' + report.get('error', 'unknown failure'))
    report['source_sha256'] = metadata['source_sha256']
    report['desktop_report'] = metadata['desktop']
    speed = []
    measured_drags = [row for row in desktop['drags'] if row.get('control')]
    if len(report['drags']) != len(measured_drags):
        raise ValueError('incomplete control workload capture')
    for actual, prior in zip(report['drags'], measured_drags):
        if actual['control'] != prior['control']:
            raise ValueError('control workload mismatch')
        row = {'workload': actual['workload'], 'samples': len(actual['samples'])}
        for key, desktop_key in [('native_preview_ms', 'native_preview_ms'),
                                 ('input_handler_ms', 'event_ms'),
                                 ('input_through_redraw_ms', 'input_to_paint_ms')]:
            now = float(np.median([s[key] for s in actual['samples']]))
            before = float(np.median([s[desktop_key] for s in prior['samples']]))
            row.update({key: now, 'desktop_' + desktop_key: before, key + '_ratio': now / before})
        speed.append(row)
    report['speed_comparison'] = speed
    reference_path = Path(metadata['reference']) if metadata['reference'] else None
    if reference_path:
        reference = json.loads(reference_path.read_text())
        providers, meshes = {}, {}
        for prim in stage.Traverse():
            ident = prim.GetCustomDataByKey('blender:id')
            if not ident:
                continue
            if prim.IsA(UsdGeom.Mesh):
                meshes[ident] = prim
            elif prim.GetTypeName() in {'RigExecJoint', 'RigExecControl'}:
                providers[ident] = prim
        baseline = {}
        poses = []
        metric_differences = []
        direct_comparisons = []
        for index, (sample, capture_row) in enumerate(zip(reference['poses'], report['poses'])):
            capture = json.loads((args.capture / capture_row['capture']).read_text())
            if capture['name'] != sample['name'] or not capture['read_generation_unchanged']:
                raise ValueError('pose name or capture generation mismatch')

            if args.desktop_captures:
                prior_capture = json.loads((args.desktop_captures / (report['rig'] + '-pose-%02d.json' % index)).read_text())
                if prior_capture['name'] != capture['name'] or not prior_capture['read_generation_unchanged']:
                    raise ValueError('desktop pose name or publication mismatch')
                point_error = frame_error = 0.0
                point_count = 0
                for key in ['points_float32_le_base64', 'frames_row_major']:
                    if capture[key].keys() != prior_capture[key].keys():
                        raise ValueError('desktop/device published inventory mismatch: ' + key)
                for path, raw in capture['points_float32_le_base64'].items():
                    actual = np.frombuffer(base64.b64decode(raw, validate=True), dtype='<f4').reshape(-1, 3)
                    expected = np.frombuffer(base64.b64decode(prior_capture['points_float32_le_base64'][path], validate=True), dtype='<f4').reshape(-1, 3)
                    if actual.shape != expected.shape:
                        raise ValueError('desktop/device point shape mismatch: ' + path)
                    if not np.isfinite(actual).all() or not np.isfinite(expected).all():
                        raise ValueError('nonfinite desktop/device point: ' + path)
                    point_error = max(point_error, float(np.linalg.norm(actual.astype(np.float64) - expected, axis=1).max(initial=0)))
                    point_count += len(actual)
                for path, matrix in capture['frames_row_major'].items():
                    if np.asarray(matrix).shape != (16,) or np.asarray(prior_capture['frames_row_major'][path]).shape != (16,):
                        raise ValueError('desktop/device frame shape mismatch: ' + path)
                    if not np.isfinite(matrix).all() or not np.isfinite(prior_capture['frames_row_major'][path]).all():
                        raise ValueError('nonfinite desktop/device frame: ' + path)
                    frame_error = max(frame_error, float(np.abs(np.asarray(matrix) - prior_capture['frames_row_major'][path]).max(initial=0)))
                direct_comparisons.append({'name': sample['name'], 'vertices_compared': point_count,
                    'frames_compared': len(capture['frames_row_major']), 'max_point_error': point_error,
                    'max_matrix_error': frame_error, 'passed': max(point_error, frame_error) <= reference['tolerance']})

            def points(prim):
                path = str(prim.GetPath())
                if path not in capture['points_float32_le_base64']:
                    raise KeyError(path)
                raw = base64.b64decode(capture['points_float32_le_base64'][path], validate=True)
                value = np.frombuffer(raw, dtype='<f4').reshape(-1, 3).astype(np.float64)
                if not np.isfinite(value).all():
                    raise ValueError('nonfinite device mesh points: ' + path)
                return value

            def frame(prim):
                path = str(prim.GetPath())
                if path not in capture['frames_row_major']:
                    raise KeyError(path)
                value = np.asarray(capture['frames_row_major'][path], dtype=np.float64).reshape(4, 4)
                if not np.isfinite(value).all():
                    raise ValueError('nonfinite device frame: ' + path)
                return value

            pose = compare_pose(sample, reference, reference_path, providers, meshes,
                                baseline, index, points, frame, measurement_prefix=args.output)
            pose['published_generation'] = capture['generation']
            poses.append(pose)
            prior = desktop['poses'][index]
            diffs = []
            for actual_mesh, desktop_mesh in zip(pose['meshes'], prior['meshes']):
                for key in ['max_error', 'rms_error', 'max_displacement_error',
                            'reference_max_displacement', 'native_max_displacement']:
                    if key in actual_mesh and key in desktop_mesh:
                        diffs.append(abs(actual_mesh[key] - desktop_mesh[key]))
                for key in ['topology_matches', 'uv_matches', 'actual_vertices']:
                    if actual_mesh.get(key) != desktop_mesh.get(key):
                        raise ValueError('iPad and desktop topology/UV inventory differs')
            for key in ['max_matrix_error']:
                diffs.append(abs(pose['bones'][key] - prior['bones'][key]))
            metric_differences.append({'name': sample['name'], 'max_metric_difference': max(diffs, default=0),
                                       'desktop_failing_frames': prior['bones']['failing'],
                                       'ipad_failing_frames': pose['bones']['failing']})
        if len(poses) != len(reference['poses']):
            raise ValueError('incomplete reference pose capture')
        report['pose_captures'] = report['poses']
        report['poses'] = poses
        report['tolerance'] = reference['tolerance']
        report['blender_parity_passed'] = all(p['passed'] for p in poses) and not reference.get('geometry_errors')
        report['desktop_parity_metric_comparison'] = metric_differences
        report['comparison_scope'] = ('Every captured point/frame versus independent Blender; desktop equivalence '
                                      'compares error/displacement metrics, not direct vertex-array identity.')
        if args.desktop_captures:
            report['desktop_native_pose_comparison'] = direct_comparisons
            report['desktop_native_pose_parity_passed'] = all(row['passed'] for row in direct_comparisons)
            report['desktop_capture_directory'] = str(args.desktop_captures.resolve())
            report['comparison_scope'] = ('Every captured point/frame versus independent Blender and direct '
                                          'desktop usdview point/frame arrays at the unchanged Blender tolerance.')
    write(args.output, report)
    print(report['rig'], 'capture PASS', 'Blender parity', report.get('blender_parity_passed', 'not applicable'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    prep = commands.add_parser('prepare')
    prep.add_argument('stage', type=Path)
    prep.add_argument('desktop', type=Path)
    prep.add_argument('output', type=Path)
    prep.add_argument('--reference', type=Path)
    comp = commands.add_parser('compare')
    comp.add_argument('metadata', type=Path)
    comp.add_argument('capture', type=Path)
    comp.add_argument('output', type=Path)
    comp.add_argument('--desktop-captures', type=Path,
                      help='Directory of desktop published captures from USDBLENDERRIG_VIEW_CAPTURE_POINTS=1')
    args = parser.parse_args()
    (prepare if args.command == 'prepare' else compare)(args)


if __name__ == '__main__':
    main()
