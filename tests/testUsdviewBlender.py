"""Real usdview material, native picker, control and root-drag verification.

Run with bin/usdview.sh, USDBLENDERRIG_USDVIEW pointing to testusdview,
--testScript this file and a converted production stage. Screenshots and
measurements go to USDBLENDERRIG_VIEW_PROOF (default /tmp/blender-usdview).
"""
import json
import hashlib
import os
from pathlib import Path
import sys

import numpy as np
from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets


def testUsdviewInputFunction(app):
    import gizmoMath
    import gizmoScreen
    import gizmoSettings
    import gizmoUI
    import pickerScene
    import pickerUI
    import rigexec

    api = app._usdviewApi
    stage = api.stage
    output = Path(os.environ.get('USDBLENDERRIG_VIEW_PROOF', '/tmp/blender-usdview'))
    output.mkdir(parents=True, exist_ok=True)
    label = Path(stage.GetRootLayer().realPath).stem
    report = {'stage': stage.GetRootLayer().realPath,
              'package_sha256': hashlib.sha256(
                  Path(stage.GetRootLayer().realPath).read_bytes()).hexdigest()}
    # HUD timing text must not create a false pixel-difference result.
    api.dataModel.viewSettings.showHUD = False
    print("viewer started",flush=True)
    for unused in range(8): app._processEvents()
    report['gizmo_frame_module'] = gizmoMath.ComputeRigFrames.__module__
    assert report['gizmo_frame_module'] == 'gizmoMath'
    assert not getattr(gizmoMath.ComputeRigFrames, '_blender_adapter', False)
    assert 'usdBlenderRigUsdview' not in sys.modules, 'Blender companion loaded'
    assert gizmoUI.GetController(), 'RigExec gizmo did not load'
    textures = [UsdShade.Shader(p) for p in stage.Traverse() if p.GetAttribute('info:id').Get() == 'UsdUVTexture']
    assert textures, 'No texture shaders'
    for shader in textures:
        asset = shader.GetInput('file').Get()
        assert asset and (asset.resolvedPath or '<UDIM>' in asset.path), str(shader.GetPath())
    report['texture_shaders'] = len(textures)
    tips = [p for p in stage.Traverse() if p.GetName().startswith('IkTip_')]
    for tip in tips:
        assert tip.GetAttribute('guide:radius').Get() == 0
        assert tip.GetAttribute('guide:displayOpacity').Get() == 0
    report['hidden_ik_tips'] = len(tips)
    pickers = pickerScene.load_all(stage)
    report['picker_pages'] = sum(len(p.panels) for p in pickers)
    assert report['picker_pages'] > 0
    print("textures and picker pages",report,flush=True)
    root = next(p for p in stage.Traverse() if p.GetTypeName() == 'RigExecControl'
                and p.GetCustomDataByKey('blender:sourceId')
                and json.loads(p.GetCustomDataByKey('blender:sourceId'))[-1] == 'root'
                and (p.GetAttribute('guide:displayOpacity').Get() or 0) > 0)
    source = next(p for p in stage.Traverse() if p.GetTypeName() == 'RigExecJoint'
                  and p.GetRelationship('blender:channelControl')
                  and p.GetRelationship('blender:channelControl').GetTargets() == [root.GetPath()])
    report['control_id'] = root.GetCustomDataByKey('blender:sourceId')
    report['control_path'] = str(root.GetPath())
    report['source_path'] = str(source.GetPath())
    assert root.GetAttribute('guide:shape').Get() == 'custom'
    assert root.GetAttribute('guide:wireWidth').Get() > 0
    report['control_wire_width'] = root.GetAttribute('guide:wireWidth').Get()
    assert root.GetAttribute('guide:points').Get()
    assert not root.GetAttribute('posed:space').HasAuthoredConnections()
    assert root.GetAttribute('default:space').HasAuthoredConnections()
    assert str(root.GetAttribute('avars:rotationOrder').Get()) == 'XYZ'
    assert root.GetAttribute('avars:rspin').Get() == 0
    assert root.GetAttribute('avars:unitScaleFactor').Get() == 1
    assert tuple(root.GetAttribute('avars:rotationSign').Get()) == (1, 1, 1)
    for channel in ('tx', 'ty', 'tz', 'rx', 'ry', 'rz', 'sx', 'sy', 'sz'):
        assert not root.GetAttribute('avars:' + channel).HasAuthoredConnections(), channel
    switches = [p for p in stage.Traverse() if p.GetTypeName() == 'RigExecSpaceSwitch'
                and p.GetRelationship('rigExec:target')
                and p.GetRelationship('rigExec:target').GetTargets() == [root.GetPath()]]
    assert len(switches) == 1, 'Native editable control has no unique space switch'
    report['space_switch'] = str(switches[0].GetPath())
    assert not source.GetRelationship('blender:editorFrame')

    # A real button click exercises the unmodified RigExec picker and its
    # ordinary selection path. Some rigs omit root from the picker, so any
    # live button targeting a converted control is sufficient here.
    panel = pickerUI.OpenPickerPanel(api)
    app._processEvents()
    pick = None
    for picker in panel._pickers:
        for button in picker.buttons:
            if not button.live or len(button.targets) != 1:
                continue
            target = stage.GetPrimAtPath(button.targets[0])
            if not target or not target.GetCustomDataByKey('blender:sourceId'):
                continue
            view = next((v for v in panel._views if v._picker is picker
                         and v._panel.id == button.parent), None)
            if view is None:
                continue
            for fx, fy in ((0.5, 0.5), (0.3, 0.5), (0.7, 0.5)):
                x, y = button.x + button.w * fx, button.y + button.h * fy
                hits = picker.hits(button.parent, x, y, view._modes, False)
                if hits and hits[0].id == button.id:
                    pick = (view, button, target, x, y)
                    break
            if pick:
                break
        if pick:
            break
    assert pick, 'No clickable native picker control'
    picker_view, button, picked, px, py = pick
    for outer in range(panel._tabs.count()):
        inner = panel._tabs.widget(outer)
        for index in range(inner.count()):
            if inner.widget(index).widget() is picker_view:
                panel._tabs.setCurrentIndex(outer)
                inner.setCurrentIndex(index)
    app._processEvents()
    point = QtCore.QPointF((px - picker_view._ox) * picker_view._scale(),
                           (py - picker_view._oy) * picker_view._scale())
    for kind, buttons in ((QtCore.QEvent.MouseButtonPress, QtCore.Qt.LeftButton),
                          (QtCore.QEvent.MouseButtonRelease, QtCore.Qt.NoButton)):
        event = QtGui.QMouseEvent(kind, point, picker_view.mapToGlobal(point.toPoint()),
                                  QtCore.Qt.LeftButton, buttons, QtCore.Qt.NoModifier)
        QtWidgets.QApplication.sendEvent(picker_view, event)
        app._processEvents()
    assert api.dataModel.selection.getPrimPaths() == [picked.GetPath()]
    report['picker_clicked_target'] = str(picked.GetPath())
    panel.close()

    api.dataModel.selection.setPrim(root)
    app._processEvents()
    assert api.dataModel.selection.getPrimPaths() == [root.GetPath()]
    print("native picker and control selection",flush=True)
    controller = gizmoUI.GetController()
    controller.SetTool(gizmoUI.TOOL_TRANSLATE)
    controller.SetOrientation(gizmoSettings.ORIENT_WORLD)
    stage.SetEditTarget(stage.GetSessionLayer())
    view = gizmoUI.StageView(api)
    QtWidgets.QApplication.setActiveWindow(app._mainWindow)
    for unused in range(8): app._processEvents()
    target = controller.Target()
    assert target and not target.Advisory(), target.Advisory() if target else 'No editable target'
    frames = gizmoMath.ComputeRigFrames(stage, root, api.frame)
    assert not frames.reason and frames.published, frames.reason
    print("gizmo frame ready",flush=True)
    controller.SetTool(gizmoUI.TOOL_SELECT)
    api.dataModel.selection.clearPrims()
    app._processEvents()
    guide_points = root.GetAttribute('guide:points').Get()
    counts = root.GetAttribute('guide:curveVertexCounts').Get()
    samples = []
    offset = 0
    for count in counts:
        for index in range(int(count) - 1):
            a, b = guide_points[offset + index], guide_points[offset + index + 1]
            samples.append((Gf.Vec3d(a) + Gf.Vec3d(b)) * 0.5)
        offset += int(count)
    assert offset == len(guide_points)
    camera, unused_aspect = view.resolveCamera()
    viewport = view.computeWindowViewport()
    projection = gizmoScreen.ViewProjection(camera)
    world = frames.posed * frames.assetToWorld
    picked_screen = None
    stride = max(1, len(samples) // 40)
    for sample in samples[::stride]:
        position = gizmoScreen.ProjectPoint(projection, viewport, world.Transform(sample))
        if not position or not (0 <= position[0] < viewport[2] and 0 <= position[1] < viewport[3]):
            continue
        in_bounds, frustum = view.computePickFrustum(*position)
        hits = view.pick(frustum) if in_bounds else []
        if hits and hits[0].hitPrimPath == root.GetPath():
            picked_screen = (position[0] / view.devicePixelRatioF(),
                             position[1] / view.devicePixelRatioF())
            break
    assert picked_screen, 'No visible native control guide could be picked'
    for kind, buttons in ((QtCore.QEvent.MouseButtonPress, QtCore.Qt.LeftButton),
                          (QtCore.QEvent.MouseButtonRelease, QtCore.Qt.NoButton)):
        local = QtCore.QPointF(*picked_screen)
        event = QtGui.QMouseEvent(kind, local, view.mapToGlobal(local.toPoint()),
                                  QtCore.Qt.LeftButton, buttons, QtCore.Qt.NoModifier)
        QtWidgets.QApplication.sendEvent(view, event)
        app._processEvents()
    assert api.dataModel.selection.getPrimPaths() == [root.GetPath()], 'Guide click did not select its native control'
    report['shape_pick_screen'] = list(picked_screen)
    controller.SetTool(gizmoUI.TOOL_TRANSLATE)
    app._processEvents()
    native_stage = Usd.Stage.Open(stage.GetRootLayer(),stage.GetSessionLayer())
    rig = rigexec.Rig(native_stage, '/Rig');rig.compile()
    pose = rig.evaluate(float(api.frame.GetValue()))
    assert pose.valid
    before_frame = Gf.Matrix4d(*pose.joint_frame(str(source.GetPath()),True).to_matrix4())
    control_frame = Gf.Matrix4d(*pose.control_frame(str(root.GetPath())).to_matrix4())
    assert np.allclose(control_frame, frames.posed, atol=2e-5, rtol=0), 'Gizmo and native control frames disagree'
    mesh = max((p for p in stage.Traverse() if p.IsA(UsdGeom.Mesh)
                and UsdGeom.Imageable(p).ComputeVisibility() != 'invisible'),
               key=lambda p: len(p.GetAttribute('points').Get() or []))
    before = np.asarray(pose.moved_property(str(mesh.GetPath())+'.points'),dtype=float)
    # An exact requested world delta verifies the channel conversion; a
    # subsequent mouse drag verifies the actual projection/event/commit path.
    writer = gizmoMath.Writer(stage, api.frame, gizmoMath.WRITE_DEFAULT)
    edit, reason = gizmoMath.MakeTarget(stage,root,gizmoMath.CHANNELS_POSE,writer)
    assert edit, reason
    pivot, reason = gizmoMath.MakeTarget(stage, root, gizmoMath.CHANNELS_PIVOT, writer)
    assert pivot is None and 'default:space' in reason, reason
    delta = Gf.Vec3d(0.04,0.025,0.01)
    edit.BeginDrag()
    edit.ApplyTranslate(delta);writer.CommitToStage()
    for unused in range(4): app._processEvents()
    pose = rig.evaluate(float(api.frame.GetValue()))
    exact_frame = Gf.Matrix4d(*pose.joint_frame(str(source.GetPath()),True).to_matrix4())
    error = (exact_frame.ExtractTranslation()-before_frame.ExtractTranslation()-delta).GetLength()
    assert error < 2e-5, 'World drag did not move the Blender root 1:1: '+str(error)
    report['exact_world_drag_error'] = error
    points = np.asarray(pose.moved_property(str(mesh.GetPath())+'.points'),dtype=float)
    report['root_mesh_displacement_error'] = float(np.linalg.norm(points-before-np.asarray(delta),axis=1).max(initial=0))
    assert report['root_mesh_displacement_error'] < 2e-5, report['root_mesh_displacement_error']
    print("exact drag verified",report,flush=True)
    controller.SetTool(gizmoUI.TOOL_TRANSLATE)
    for unused in range(4): app._processEvents()
    handles = controller.HandleScreenPositions()
    assert 'x' in handles, handles
    a,b = handles['x'][0],handles['x'][-1]
    lerp = lambda t: (a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t)
    start,end = lerp(0.45),lerp(0.7)
    def send(kind,point,button,buttons):
        pos = QtCore.QPointF(*point)
        global_pos = view.mapToGlobal(QtCore.QPoint(int(point[0]),int(point[1])))
        event = QtGui.QMouseEvent(kind,pos,QtCore.QPointF(global_pos),button,buttons,QtCore.Qt.NoModifier)
        QtWidgets.QApplication.sendEvent(view,event);app._processEvents()
    send(QtCore.QEvent.MouseButtonPress,start,QtCore.Qt.LeftButton,QtCore.Qt.LeftButton)
    assert controller.IsDragging(), 'Mouse did not grab the control gizmo'
    for t in (0.25,0.5,0.75,1):
        point = (start[0]+(end[0]-start[0])*t,start[1]+(end[1]-start[1])*t)
        send(QtCore.QEvent.MouseMove,point,QtCore.Qt.NoButton,QtCore.Qt.LeftButton)
    send(QtCore.QEvent.MouseButtonRelease,end,QtCore.Qt.LeftButton,QtCore.Qt.NoButton)
    assert not controller.IsDragging()
    for unused in range(4): app._processEvents()
    pose = rig.evaluate(float(api.frame.GetValue()))
    final = np.asarray(pose.moved_property(str(mesh.GetPath())+'.points'),dtype=float)
    final_frame = Gf.Matrix4d(*pose.joint_frame(str(source.GetPath()),True).to_matrix4())
    drag = final_frame.ExtractTranslation()-exact_frame.ExtractTranslation()
    assert drag.GetLength() > 1e-4, 'Mouse drag authored no effective bone movement'
    report['mouse_drag_displacement'] = list(drag)
    report['mouse_mesh_displacement_error'] = float(np.linalg.norm(final-points-np.asarray(drag),axis=1).max(initial=0))
    assert report['mouse_mesh_displacement_error'] < 2e-5, report['mouse_mesh_displacement_error']
    # The same ordinary control owns editable rotation and scale channels.
    # Check their downstream bone/mesh effect, then restore those values so
    # the framebuffer still records the translation-only pose above.
    for channel, apply in (
            ('rz', lambda target: target.ApplyRotateChannel(2, 5.0)),
            ('sx', lambda target: target.ApplyScale(0, 1.02))):
        attr = root.GetAttribute('avars:' + channel)
        initial = attr.Get(api.frame)
        writer = gizmoMath.Writer(stage, api.frame, gizmoMath.WRITE_DEFAULT)
        edit, reason = gizmoMath.MakeTarget(stage, root, gizmoMath.CHANNELS_POSE, writer)
        assert edit, reason
        edit.BeginDrag()
        apply(edit)
        writer.CommitToStage()
        changed = attr.Get(api.frame)
        assert abs(changed - initial) > 1e-5, channel
        for unused in range(4): app._processEvents()
        changed_pose = rig.evaluate(float(api.frame.GetValue()))
        changed_points = np.asarray(changed_pose.moved_property(str(mesh.GetPath())+'.points'), dtype=float)
        displacement = float(np.linalg.norm(changed_points - final, axis=1).max(initial=0))
        assert displacement > 1e-5, channel + ' did not move evaluated geometry'
        report[channel + '_mesh_displacement'] = displacement
        gizmoMath.SetAnimated(attr, initial, api.frame)
        for unused in range(4): app._processEvents()
    view.updateGL();app._processEvents()
    api.dataModel.selection.clearPrims();app._processEvents()
    material = view.grabFramebuffer();material.save(str(output/(label+'-materials.png')))
    # Change texture uniforms while keeping subset draw items intact.
    # Storm's scene-material toggle currently fails on multi-material meshes.
    color_textures = {}
    for p in stage.Traverse():
        if p.GetAttribute('info:id').Get() != 'UsdPreviewSurface':
            continue
        source = UsdShade.Shader(p).GetInput('diffuseColor').GetConnectedSource()
        if source and source[0].GetPrim().GetAttribute('info:id').Get() == 'UsdUVTexture':
            shader = UsdShade.Shader(source[0].GetPrim())
            scale = shader.GetInput('scale')
            color_textures[str(shader.GetPath())] = scale.Get() if scale else Gf.Vec4f(1)
    assert color_textures, 'No connected diffuse textures'
    for path in color_textures:
        UsdShade.Shader(stage.GetPrimAtPath(path)).CreateInput('scale',Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(0,0,0,1))
    for unused in range(4): view.updateGL();app._processEvents()
    plain = view.grabFramebuffer();plain.save(str(output/(label+'-textures-suppressed.png')))
    assert material != plain, 'Diffuse texture uniforms made no framebuffer difference'
    report['material_framebuffer_differs'] = True
    for path, value in color_textures.items():
        UsdShade.Shader(stage.GetPrimAtPath(path)).GetInput('scale').Set(value)
    report['passed'] = True
    (output/(label+'-usdview.json')).write_text(json.dumps(report,indent=2))
    print(json.dumps(report),flush=True)
