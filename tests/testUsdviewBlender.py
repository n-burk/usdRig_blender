"""Real usdview material, picker, shape-pick and root-drag verification.

Run with bin/usdview.sh, USDBLENDERRIG_USDVIEW pointing to testusdview,
--testScript this file and a converted production stage. Screenshots and
measurements go to USDBLENDERRIG_VIEW_PROOF (default /tmp/blender-usdview).
"""
import json
import os
from pathlib import Path

import numpy as np
from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets


def testUsdviewInputFunction(app):
    import gizmoMath
    import gizmoSettings
    import gizmoUI
    import pickerScene
    import rigexec

    api = app._usdviewApi
    stage = api.stage
    output = Path(os.environ.get('USDBLENDERRIG_VIEW_PROOF', '/tmp/blender-usdview'))
    output.mkdir(parents=True, exist_ok=True)
    label = Path(stage.GetRootLayer().realPath).stem
    report = {'stage': stage.GetRootLayer().realPath}
    # HUD timing text must not create a false pixel-difference result.
    api.dataModel.viewSettings.showHUD = False
    print("viewer started",flush=True)
    for unused in range(8): app._processEvents()
    assert getattr(gizmoMath.ComputeRigFrames, '_blender_adapter', False), 'Blender companion did not load'
    assert gizmoUI.GetController(), 'RigExec gizmo did not load'
    textures = [UsdShade.Shader(p) for p in stage.Traverse() if p.GetAttribute('info:id').Get() == 'UsdUVTexture']
    assert textures, 'No texture shaders'
    for shader in textures:
        asset = shader.GetInput('file').Get()
        assert asset and (asset.resolvedPath or '<UDIM>' in asset.path), str(shader.GetPath())
    report['texture_shaders'] = len(textures)
    pickers = pickerScene.load_all(stage)
    report['picker_pages'] = sum(len(p.panels) for p in pickers)
    assert report['picker_pages'] > 0
    print("textures and picker pages",report,flush=True)
    root = next(p for p in stage.Traverse() if p.GetTypeName() == 'RigExecJoint'
                and p.GetDisplayName() == 'root' and p.GetRelationship('blender:editorFrame')
                and (p.GetChild('Display').GetAttribute('guide:displayOpacity').Get() or 0) > 0)
    report['control_id'] = root.GetCustomDataByKey('blender:id')
    display = root.GetChild('Display')
    assert display and display.GetAttribute('guide:wireWidth').Get() == 0
    api.dataModel.selection.setPrim(display)
    app._processEvents()
    assert api.dataModel.selection.getPrimPaths() == [root.GetPath()], 'Shape pick did not select its bone: '+str(api.dataModel.selection.getPrimPaths())
    print("shape pick remapped",flush=True)
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
    native_stage = Usd.Stage.Open(stage.GetRootLayer(),stage.GetSessionLayer())
    rig = rigexec.Rig(native_stage, '/Rig');rig.compile()
    pose = rig.evaluate(float(api.frame.GetValue()))
    assert pose.valid
    before_frame = Gf.Matrix4d(*pose.joint_frame(str(root.GetPath()),True).to_matrix4())
    assert np.allclose(before_frame, frames.posed, atol=2e-5, rtol=0), 'Gizmo and native bone frames disagree'
    mesh = max((p for p in stage.Traverse() if p.IsA(UsdGeom.Mesh)
                and UsdGeom.Imageable(p).ComputeVisibility() != 'invisible'),
               key=lambda p: len(p.GetAttribute('points').Get() or []))
    before = np.asarray(pose.moved_property(str(mesh.GetPath())+'.points'),dtype=float)
    # An exact requested world delta verifies the channel conversion; a
    # subsequent mouse drag verifies the actual projection/event/commit path.
    writer = gizmoMath.Writer(stage, api.frame, gizmoMath.WRITE_DEFAULT)
    edit, reason = gizmoMath.MakeTarget(stage,root,gizmoMath.CHANNELS_POSE,writer)
    assert edit, reason
    delta = Gf.Vec3d(0.04,0.025,0.01)
    edit.BeginDrag()
    edit.ApplyTranslate(delta);writer.CommitToStage()
    for unused in range(4): app._processEvents()
    pose = rig.evaluate(float(api.frame.GetValue()))
    exact_frame = Gf.Matrix4d(*pose.joint_frame(str(root.GetPath()),True).to_matrix4())
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
    final_frame = Gf.Matrix4d(*pose.joint_frame(str(root.GetPath()),True).to_matrix4())
    drag = final_frame.ExtractTranslation()-exact_frame.ExtractTranslation()
    assert drag.GetLength() > 1e-4, 'Mouse drag authored no effective bone movement'
    report['mouse_drag_displacement'] = list(drag)
    report['mouse_mesh_displacement_error'] = float(np.linalg.norm(final-points-np.asarray(drag),axis=1).max(initial=0))
    assert report['mouse_mesh_displacement_error'] < 2e-5, report['mouse_mesh_displacement_error']
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
