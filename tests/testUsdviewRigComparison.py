"""Measure real usdview gizmo events and compare its published Blender poses.

Build usdBlenderRigViewSnapshot, then run from this checkout with bin/usdview.sh
and USDBLENDERRIG_USDVIEW=$TESTUSDVIEW --testScript this file <stage>.
USDBLENDERRIG_VIEW_PROOF selects the output directory. Set
USDBLENDERRIG_VIEW_REFERENCE to an independent Blender reference.json for
parity; omit it for the Biped baseline. RIGEXEC_IMAGING_PROFILE=1 enables TSV
profiles. Runs must be sequential, with the same renderer and viewport.
"""
import base64
import ctypes
import hashlib
import json
import os
from pathlib import Path
import sys
import time

import numpy as np
from pxr import Sdf, UsdGeom
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets


def testUsdviewInputFunction(app):
    import gizmoMath
    import gizmoPreview
    import gizmoSettings
    import gizmoUI
    import rigExecUsdview

    assert gizmoMath.ComputeRigFrames.__module__ == 'gizmoMath'
    assert not getattr(gizmoMath.ComputeRigFrames, '_blender_adapter', False)
    assert 'usdBlenderRigUsdview' not in sys.modules

    sys.path.insert(0, str(Path.cwd() / 'tests'))
    from compareRigReference import compare_pose

    api = app._usdviewApi
    stage = api.stage
    output = Path(os.environ.get('USDBLENDERRIG_VIEW_PROOF', '/tmp/rig-usdview-comparison'))
    output.mkdir(parents=True, exist_ok=True)
    label = Path(stage.GetRootLayer().realPath).stem.split('_')[0]
    if label.startswith('Blender-'):
        label = label[len('Blender-'):]
    report_path = output / (label + '.json')
    report = {'stage': stage.GetRootLayer().realPath,
              'measurement_source': 'usdview stage-scoped immutable Hydra snapshot',
              'latency_scope': 'Qt mouse input through synchronous viewport paint; excludes physical display scanout',
              'interaction_scope': 'control frame movement and viewport publication; mesh correctness is checked separately against Blender',
              'requested_evaluation_mode': os.environ.get('RIGEXEC_EVALUATION_MODE', 'auto'),
              'completed': False, 'drags': [], 'poses': []}

    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')

    api.dataModel.viewSettings.showHUD = False
    app._mainWindow.resize(1500, 1000)
    app.setViewerMode(True)
    stage.SetEditTarget(stage.GetSessionLayer())
    app._processEvents()
    controller = gizmoUI.GetController()
    assert controller, 'RigExec gizmo did not load'
    view = gizmoUI.StageView(api)
    view.SetPhysicalWindowSize(1280, 960)
    QtWidgets.QApplication.setActiveWindow(app._mainWindow)
    app._processEvents()
    handle = rigExecUsdview.ContainerFor(api).ImagingHandle()
    assert handle and handle.GetGeneration() > 0, 'No published viewport generation'
    module_path = Path(os.environ.get('USDBLENDERRIG_VIEW_SNAPSHOT',
                                      str(Path.cwd() / 'build/libusdBlenderRigViewSnapshot.so')))
    inspector = ctypes.CDLL(str(module_path))
    read = inspector.UsdBlenderRigView_ReadPoints
    read.argtypes = [ctypes.c_longlong, ctypes.c_char_p, ctypes.c_double,
                     ctypes.c_int, ctypes.POINTER(ctypes.c_float), ctypes.c_int]
    read.restype = ctypes.c_int

    def points(prim):
        path = str(prim.GetPath()).encode()
        count = read(handle.CacheId(), path, api.frame.GetValue(), api.frame.IsDefault(), None, 0)
        assert count >= -1, 'No matching snapshot for this stage/time'
        if count < 0:
            # No override means Hydra uses the authored points of this mesh.
            value = prim.GetAttribute('points').Get(api.frame)
            if value is None:
                # Biped's touch-pose UI overlay is an empty Mesh until used.
                return np.empty((0, 3)), 'empty drawable (no authored or published points)'
            return np.asarray(value, dtype=np.float64), 'authored (no snapshot override)'
        values = np.empty((count, 3), dtype=np.float32)
        actual = read(handle.CacheId(), path, api.frame.GetValue(), api.frame.IsDefault(),
                      values.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), count)
        assert actual == count, 'Snapshot changed during point read'
        return values.astype(np.float64), 'published snapshot'

    def frame(prim):
        values = (ctypes.c_double * 16)()
        if not handle.GetControlFrameAssetSpace(str(prim.GetPath()), api.frame.GetValue(),
                                                int(api.frame.IsDefault()), values):
            raise KeyError(str(prim.GetPath()))
        return np.asarray(values).reshape(4, 4).copy()

    all_meshes = [p for p in stage.Traverse() if p.IsA(UsdGeom.Mesh)]
    visible_meshes = [p for p in all_meshes if UsdGeom.Imageable(p).ComputeVisibility(api.frame) != 'invisible']
    report.update(viewport_logical_pixels=[view.width(), view.height()],
                  viewport_physical_pixels=list(view.GetPhysicalWindowSize()),
                  device_pixel_ratio=view.devicePixelRatioF(),
                  complexity=str(api.dataModel.viewSettings.complexity),
                  renderer=str(view._renderer.GetRendererDisplayName(view._renderer.GetCurrentRendererId())),
                  meshes=len(all_meshes), visible_meshes=len(visible_meshes),
                  mesh_vertices=sum(len(p.GetAttribute('points').Get() or []) for p in all_meshes),
                  prims=sum(1 for p in stage.Traverse()),
                  subdivision_schemes=sorted({str(p.GetAttribute('subdivisionScheme').Get()) for p in all_meshes}),
                  stage_sha256=hashlib.sha256(Path(stage.GetRootLayer().realPath).read_bytes()).hexdigest())
    view.updateGL(); app._processEvents()
    view.grabFramebuffer().save(str(output / (label + '-saved.png')))

    # Reads themselves must not evaluate, publish or dirty USD.
    generation = handle.GetGeneration()
    layer_before = stage.GetSessionLayer().ExportToString()
    published = {str(p.GetPath()): points(p)[1] for p in all_meshes}
    assert read(0, str(all_meshes[0].GetPath()).encode(), api.frame.GetValue(), api.frame.IsDefault(), None, 0) == -2
    assert read(handle.CacheId(), str(all_meshes[0].GetPath()).encode(), float(api.frame.GetValue()) + 0.25, 0, None, 0) == -2
    assert handle.GetGeneration() == generation
    assert stage.GetSessionLayer().ExportToString() == layer_before
    report['point_sources'] = published
    report['snapshot_read_is_read_only'] = True
    save()

    # Use actual Alt-drag events: directly mutating FreeCamera bypasses the
    # navigation guard and lets synchronous idle warming contaminate timings.
    camera_before = api.dataModel.viewSettings.freeCamera.clone()
    generation = handle.GetGeneration()
    warm_before = handle.GetWarmingCompletedCount()
    def camera_event(kind, point, button, buttons):
        local = QtCore.QPointF(*point)
        event = QtGui.QMouseEvent(kind, local, QtCore.QPointF(view.mapToGlobal(local.toPoint())),
                                 button, buttons, QtCore.Qt.AltModifier)
        QtWidgets.QApplication.sendEvent(view, event)

    origin = (view.width() * 0.5, view.height() * 0.5)
    camera_event(QtCore.QEvent.MouseButtonPress, origin, QtCore.Qt.LeftButton, QtCore.Qt.LeftButton)
    assert view._dragActive and view._cameraMode == 'tumble'
    tumble, warmup = [], []
    try:
        for step in range(40):
            point = (origin[0] + (step + 1) * 2 / view.devicePixelRatioF(),
                     origin[1] + (step + 1) / view.devicePixelRatioF())
            start = time.perf_counter()
            camera_event(QtCore.QEvent.MouseMove, point, QtCore.Qt.NoButton, QtCore.Qt.LeftButton)
            view.updateGL(); app._processEvents()
            elapsed = (time.perf_counter() - start) * 1000
            (warmup if step < 10 else tumble).append(elapsed)
        report['camera_tumble'] = {
            'input': 'Alt-left mouse drag', 'warmup_samples_ms': warmup, 'samples_ms': tumble,
            'median_ms': float(np.median(tumble)), 'p95_ms': float(np.percentile(tumble, 95)),
            'generation_unchanged': handle.GetGeneration() == generation,
            'warming_completed_during_drag': handle.GetWarmingCompletedCount() - warm_before}
        assert report['camera_tumble']['generation_unchanged'], 'Camera tumble published a new rig pose'
        handle.WriteProfileSummary(str(output / (label + '-camera.tsv')))
    finally:
        camera_event(QtCore.QEvent.MouseButtonRelease, point, QtCore.Qt.LeftButton, QtCore.Qt.NoButton)
        api.dataModel.viewSettings.freeCamera = camera_before
        view.updateGL(); app._processEvents()
    save()

    blender = any(p.GetCustomDataByKey('blender:id') for p in all_meshes)
    if blender:
        names = [('global root', 'root'), ('left hand IK', 'IK-Wrist.L' if label == 'Snow' else 'hand_ik.L'),
                 ('left foot IK', 'IK-Foot.L' if label == 'Snow' else 'foot_ik.L')]
        if label == 'Gamma':
            names.append(('tail tip', 'Tail-SPIK03'))
        candidates = {}
        for prim in stage.Traverse():
            source_id = prim.GetCustomDataByKey('blender:sourceId')
            if prim.GetTypeName() == 'RigExecControl' and source_id:
                name = json.loads(source_id)[-1]
                opacity = prim.GetAttribute('guide:displayOpacity')
                if name not in candidates or (opacity and (opacity.Get() or 0) > 0):
                    candidates[name] = prim
    else:
        names = [('body (feet remain constrained)', 'M_Body'), ('left hand IK', 'L_ArmIK'),
                 ('left foot IK', 'L_LegIK')]
        candidates = {p.GetName(): p for p in stage.Traverse() if p.GetTypeName() == 'RigExecControl'}

    poses_only = os.environ.get('USDBLENDERRIG_VIEW_POSES_ONLY') == '1'
    if poses_only:
        names = []
        report['interaction_scope'] = 'pose capture only; no drag timings collected'

    for workload, name in names:
        prim = candidates.get(name)
        if not prim:
            diagnostics = stage.GetDefaultPrim().GetAttribute('blender:diagnostics').Get() or []
            sources = [p for p in stage.Traverse()
                       if p.GetTypeName() == 'RigExecJoint'
                       and p.GetCustomDataByKey('blender:id')
                       and json.loads(p.GetCustomDataByKey('blender:id'))[-1] == name]
            unavailable = [(p, 'bone control is read-only in standard usdRig interaction: '
                            + p.GetCustomDataByKey('blender:id')) for p in sources]
            unavailable = [(p, reason) for p, reason in unavailable if reason in diagnostics]
            if blender and unavailable:
                source, reason = unavailable[0]
                report['drags'].append({'workload': workload, 'source': str(source.GetPath()),
                                        'status': 'unavailable', 'reason': reason})
            else:
                report['drags'].append({'workload': workload, 'error': 'control missing', 'passed': False})
            save(); continue
        api.dataModel.selection.setPrim(prim)
        controller.SetTool(gizmoUI.TOOL_TRANSLATE)
        controller.SetOrientation(gizmoSettings.ORIENT_WORLD)
        app._processEvents()
        target = controller.Target()
        row = {'workload': workload, 'control': str(prim.GetPath()), 'samples': [], 'release_ms': []}
        if not target or target.Advisory():
            row.update(error=target.Advisory() if target else 'No target', passed=False)
            report['drags'].append(row); save(); continue
        channel = gizmoPreview.Channel(api)
        assert channel and channel.HasSink(), 'No viewport preview sink'
        original_update = channel._sink.Update
        native_times = []

        def timed_update(values):
            start = time.perf_counter()
            try:
                return original_update(values)
            finally:
                native_times.append((time.perf_counter() - start) * 1000)

        channel._sink.Update = timed_update

        def send(kind, point, button, buttons):
            local = QtCore.QPointF(*point)
            global_point = view.mapToGlobal(QtCore.QPoint(int(point[0]), int(point[1])))
            event = QtGui.QMouseEvent(kind, local, QtCore.QPointF(global_point), button, buttons, QtCore.Qt.NoModifier)
            start = time.perf_counter()
            QtWidgets.QApplication.sendEvent(view, event)
            handled = time.perf_counter()
            app._processEvents()
            view.updateGL()
            app._processEvents()
            return (handled - start) * 1000, (time.perf_counter() - start) * 1000

        initial_values = {str(a.GetPath()): a.Get(api.frame) for a in prim.GetAttributes() if a.GetName().startswith('avars:')}
        setup = {}
        if not blender and name == 'L_ArmIK':
            # The Biped example opens in FK. Compare active IK deformation,
            # rather than timing a disconnected, hidden IK control.
            switch = candidates['L_Arm'].GetAttribute('avars:ikfk')
            setup[str(switch.GetPath())] = 1.0
            initial_values.update(setup)
        backup = Sdf.Layer.CreateAnonymous('benchmark-avars')
        for path in initial_values:
            if stage.GetSessionLayer().GetAttributeAtPath(path):
                Sdf.CreatePrimInLayer(backup, Sdf.Path(path).GetPrimPath())
                Sdf.CopySpec(stage.GetSessionLayer(), path, backup, path)
        if setup:
            with Sdf.ChangeBlock():
                for path, value in setup.items():
                    stage.GetAttributeAtPath(path).Set(value)
            handle.SetTime(float(api.frame.GetValue()))
            app._processEvents(); view.updateGL(); app._processEvents()
        row['workload_setup'] = setup
        initial_frame = frame(prim)
        before_points = {str(p.GetPath()): points(p)[0] for p in visible_meshes}
        handle.WriteProfileSummary(str(output / (label + '-' + name.replace('.', '_') + '-startup.tsv')))
        try:
            # Two warm-up gestures, then four measured gestures of six moves.
            for gesture in range(6):
                if gesture:
                    with Sdf.ChangeBlock():
                        for path, value in initial_values.items():
                            gizmoMath.SetAnimated(stage.GetAttributeAtPath(path), value, api.frame)
                    handle.SetTime(float(api.frame.GetValue()))
                    app._processEvents(); view.updateGL(); app._processEvents()
                handles = controller.HandleScreenPositions()
                assert 'x' in handles, handles
                a, b = handles['x'][0], handles['x'][-1]
                lerp = lambda t: (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
                start_point, end_point = lerp(0.45), lerp(0.70)
                assert all(0 <= p[0] < view.width() and 0 <= p[1] < view.height()
                           for p in (start_point, end_point)), 'Handle outside viewport'
                send(QtCore.QEvent.MouseButtonPress, start_point, QtCore.Qt.LeftButton, QtCore.Qt.LeftButton)
                assert controller.IsDragging(), 'Mouse did not grab gizmo'
                for step in range(1, 7):
                    t = step / 6
                    point = (start_point[0] + (end_point[0] - start_point[0]) * t,
                             start_point[1] + (end_point[1] - start_point[1]) * t)
                    old_generation = handle.GetGeneration()
                    first_native = len(native_times)
                    event_ms, paint_ms = send(QtCore.QEvent.MouseMove, point, QtCore.Qt.NoButton, QtCore.Qt.LeftButton)
                    # Read AFTER painting: evidence is from the generation the
                    # viewer used, not another evaluator or the authored frame.
                    sample = {'event_ms': event_ms, 'input_to_paint_ms': paint_ms,
                              'native_preview_ms': sum(native_times[first_native:]),
                              'native_updates': len(native_times) - first_native,
                              'generation_advanced': handle.GetGeneration() > old_generation,
                              'origin': frame(prim)[3, :3].tolist()}
                    if gesture >= 2:
                        row['samples'].append(sample)
                unused, release_ms = send(QtCore.QEvent.MouseButtonRelease, end_point, QtCore.Qt.LeftButton, QtCore.Qt.NoButton)
                assert not controller.IsDragging()
                if gesture >= 2:
                    row['release_ms'].append(release_ms)
                print(label, workload, 'gesture', gesture, 'complete', flush=True)
            delta = frame(prim)[3, :3] - initial_frame[3, :3]
            mesh_motion = []
            for mesh in visible_meshes:
                value, source = points(mesh)
                before = before_points[str(mesh.GetPath())]
                mesh_motion.append({'path': str(mesh.GetPath()), 'point_source': source,
                                    'max_displacement': float(np.linalg.norm(value - before, axis=1).max(initial=0)),
                                    'uniform_control_delta_error': float(np.linalg.norm(value - before - delta, axis=1).max(initial=0))})
            row.update(control_displacement=delta.tolist(), mesh_motion=mesh_motion,
                       passed=bool(np.linalg.norm(delta) > 1e-5 and all(s['generation_advanced'] and s['native_updates'] for s in row['samples'])))
            if label == 'Snow' and name == 'root':
                teeth = {m['path'].rsplit('/', 1)[-1]: m for m in mesh_motion
                         if m['path'].rsplit('/', 1)[-1] in {
                             'GEO_snow_teeth_lower', 'GEO_snow_teeth_upper',
                             'GEO_snow_gums_lower', 'GEO_snow_gums_upper'}}
                row['teeth_follow_root'] = (len(teeth) == 4 and all(
                    m['point_source'] == 'published snapshot' and
                    m['uniform_control_delta_error'] < 1e-4 for m in teeth.values()))
                row['passed'] &= row['teeth_follow_root']
            if label == 'Gamma' and name == 'Tail-SPIK03':
                body = next((m for m in mesh_motion if m['path'] == '/Rig/Geometry/MESH_Gamma'), None)
                row['tail_mesh_moves'] = bool(body and body['point_source'] == 'published snapshot'
                                              and body['max_displacement'] > 0.01)
                row['passed'] &= row['tail_mesh_moves']
            if label == 'Gamma' and name == 'foot_ik.L':
                body = next((m for m in mesh_motion if m['path'] == '/Rig/Geometry/MESH_Gamma'), None)
                row['foot_mesh_moves'] = bool(body and body['point_source'] == 'published snapshot'
                                              and body['max_displacement'] > 0.01)
                row['passed'] &= row['foot_mesh_moves']
            for field in ('event_ms', 'input_to_paint_ms', 'native_preview_ms'):
                values = [s[field] for s in row['samples']]
                row[field] = {'median': float(np.median(values)), 'p95': float(np.percentile(values, 95)),
                              'max': max(values), 'count': len(values)}
            view.grabFramebuffer().save(str(output / (label + '-' + name.replace('.', '_') + '-drag.png')))
        finally:
            channel._sink.Update = original_update
            handle.WriteProfileSummary(str(output / (label + '-' + name.replace('.', '_') + '-drag.tsv')))
            with Sdf.ChangeBlock():
                for path in initial_values:
                    if backup.GetAttributeAtPath(path):
                        Sdf.CopySpec(backup, path, stage.GetSessionLayer(), path)
                    else:
                        spec = stage.GetSessionLayer().GetAttributeAtPath(path)
                        if spec:
                            spec.owner.RemoveProperty(spec)
            handle.SetTime(float(api.frame.GetValue()))
            app._processEvents(); view.updateGL()
        report['drags'].append(row)
        save()

    reference_env = os.environ.get('USDBLENDERRIG_VIEW_REFERENCE')
    if reference_env:
        reference_path = Path(reference_env)
        reference = json.loads(reference_path.read_text())
        report.update(reference=str(reference_path.resolve()), tolerance=reference['tolerance'],
                      blender_version=reference['blender_version'],
                      parity_passed=not reference.get('geometry_errors'),
                      reference_geometry_errors=reference.get('geometry_errors', []),
                      skipped_controls=reference.get('skipped_controls', []))
        providers, edit_providers, meshes = {}, {}, {}
        for prim in stage.Traverse():
            ident = prim.GetCustomDataByKey('blender:id')
            if ident and prim.IsA(UsdGeom.Mesh):
                meshes[ident] = prim
            elif ident and prim.GetTypeName() in {'RigExecJoint', 'RigExecControl'}:
                providers[ident] = prim
                control = prim.GetRelationship('blender:channelControl')
                targets = control.GetTargets() if control else []
                edit_providers[ident] = stage.GetPrimAtPath(targets[0]) if len(targets) == 1 else prim
        initial = {(edit['id'], edit['channel']): edit_providers[edit['id']].GetAttribute('avars:' + edit['channel']).Get()
                   for sample in reference['poses'] for edit in sample['edits']}
        baseline = {}
        api.dataModel.selection.clearPrims(); app._processEvents()
        for index, sample in enumerate(reference['poses']):
            with Sdf.ChangeBlock():
                for (ident, name), value in initial.items():
                    edit_providers[ident].GetAttribute('avars:' + name).Set(value)
                for edit in sample['edits']:
                    edit_providers[edit['id']].GetAttribute('avars:' + edit['channel']).Set(edit['value'])
            assert handle.SetTime(float(api.frame.GetValue())) == 0, 'Viewport evaluation failed'
            app._processEvents(); view.updateGL(); app._processEvents()
            result = compare_pose(sample, reference, reference_path, providers, meshes,
                                  baseline, index, lambda p: points(p)[0], frame)
            if os.environ.get('USDBLENDERRIG_VIEW_CAPTURE_POINTS') == '1':
                generation = handle.GetGeneration()
                capture = {
                    'name': sample['name'], 'generation': generation,
                    'points_float32_le_base64': {
                        str(p.GetPath()): base64.b64encode(points(p)[0].astype('<f4').tobytes()).decode('ascii')
                        for p in all_meshes},
                    'frames_row_major': {str(providers[ident].GetPath()): frame(providers[ident]).reshape(-1).tolist()
                                         for ident in sample['bones'] if ident in providers}}
                capture['read_generation_unchanged'] = handle.GetGeneration() == generation
                assert capture['read_generation_unchanged'], 'Publication changed during desktop pose capture'
                capture_path = output / (label + '-pose-%02d.json' % index)
                capture_path.write_text(json.dumps(capture) + '\n')
                result['published_capture'] = capture_path.name
            result['published_generation'] = handle.GetGeneration()
            for row, mesh in zip(result['meshes'], sample['meshes']):
                prim = meshes.get(mesh['id'])
                if prim:
                    row['point_source'] = points(prim)[1]
            report['poses'].append(result)
            report['parity_passed'] &= result['passed']
            save()
            print(label, sample['name'], 'PASS' if result['passed'] else 'FAIL',
                  'mesh max', max((r.get('max_error', 0) for r in result['meshes']), default=0),
                  'bones failing', result['bones']['failing'], flush=True)
            if index in (0, 1, 2, 10):
                view.grabFramebuffer().save(str(output / (label + '-pose-%02d.png' % index)))
        handle.WriteProfileSummary(str(output / (label + '-reference-poses.tsv')))
    attempted = [row for row in report['drags'] if row.get('status') != 'unavailable']
    report['interaction_passed'] = bool(attempted) and all(row.get('passed', False) for row in attempted)
    report['unavailable_workloads'] = [row['workload'] for row in report['drags']
                                       if row.get('status') == 'unavailable']
    report['completed'] = True
    report['source_layer_unchanged'] = report['stage_sha256'] == hashlib.sha256(Path(stage.GetRootLayer().realPath).read_bytes()).hexdigest()
    save()
    assert report['interaction_passed'], 'One or more viewport drags failed; see ' + str(report_path)
    print(label, 'viewport comparison completed; parity', report.get('parity_passed', 'no Blender reference'), flush=True)
