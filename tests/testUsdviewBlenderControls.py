"""Pick translated hand/foot curves and picker buttons, then edit native channels.

Run sequentially with bin/usdview.sh --testScript and a converted rig. The
snapshot inspector target must be built. Evidence goes to
USDBLENDERRIG_VIEW_PROOF; all edits stay in the session layer.
"""
import ctypes
import hashlib
import json
import os
from pathlib import Path
from pxr import Gf
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets


def testUsdviewInputFunction(app):
    import gizmoMath
    import gizmoScreen
    import gizmoSettings
    import gizmoUI
    import pickerUI
    import rigExecUsdview

    api = app._usdviewApi
    stage = api.stage
    stage.SetEditTarget(stage.GetSessionLayer())
    app._mainWindow.resize(1500, 1000)
    app.setViewerMode(True)
    api.dataModel.viewSettings.showHUD = False
    view = gizmoUI.StageView(api)
    view.SetPhysicalWindowSize(1280, 960)
    QtWidgets.QApplication.setActiveWindow(app._mainWindow)
    controller = gizmoUI.GetController()
    handle = rigExecUsdview.ContainerFor(api).ImagingHandle()
    inspector = ctypes.CDLL(str(Path.cwd() / 'build/libusdBlenderRigViewSnapshot.so'))
    read = inspector.UsdBlenderRigView_ReadGuideFrame
    read.argtypes = [ctypes.c_longlong, ctypes.c_char_p, ctypes.c_double,
                     ctypes.c_int, ctypes.POINTER(ctypes.c_double)]
    read.restype = ctypes.c_int
    output = Path(os.environ.get('USDBLENDERRIG_VIEW_PROOF', '/tmp/blender-controls'))
    output.mkdir(parents=True, exist_ok=True)
    label = Path(stage.GetRootLayer().realPath).stem
    report = {'stage': stage.GetRootLayer().realPath, 'sha256': hashlib.sha256(Path(stage.GetRootLayer().realPath).read_bytes()).hexdigest(), 'controls': []}

    def pump():
        app._processEvents(); view.updateGL(); app._processEvents()

    def send(widget, kind, point, button, buttons):
        local = QtCore.QPointF(*point)
        event = QtGui.QMouseEvent(kind, local, QtCore.QPointF(widget.mapToGlobal(local.toPoint())),
                                  button, buttons, QtCore.Qt.NoModifier)
        QtWidgets.QApplication.sendEvent(widget, event)
        pump()

    def click(widget, point):
        send(widget, QtCore.QEvent.MouseButtonPress, point, QtCore.Qt.LeftButton, QtCore.Qt.LeftButton)
        send(widget, QtCore.QEvent.MouseButtonRelease, point, QtCore.Qt.LeftButton, QtCore.Qt.NoButton)

    def picker_click(prim):
        panel = pickerUI.OpenPickerPanel(api)
        pump()
        for picker in panel._pickers:
            for button in picker.buttons:
                if not button.live or list(button.targets) != [str(prim.GetPath())]:
                    continue
                pv = next((v for v in panel._views if v._picker is picker and v._panel.id == button.parent), None)
                if pv is None:
                    continue
                for outer in range(panel._tabs.count()):
                    inner = panel._tabs.widget(outer)
                    for index in range(inner.count()):
                        if inner.widget(index).widget() is pv:
                            panel._tabs.setCurrentIndex(outer); inner.setCurrentIndex(index)
                pump()
                point = ((button.x + button.w/2 - pv._ox)*pv._scale(),
                         (button.y + button.h/2 - pv._oy)*pv._scale())
                click(pv, point)
                assert api.dataModel.selection.getPrimPaths() == [prim.GetPath()]
                panel.close(); pump()
                return
        panel.close()
        raise AssertionError('No live picker button for '+str(prim.GetPath()))

    names = ('IK-Wrist.L', 'IK-Foot.L') if 'Snow' in label else ('hand_ik.L', 'foot_ik.L')
    for name in names:
        prim = next(p for p in stage.Traverse() if p.GetCustomDataByKey('blender:sourceId')
                    and json.loads(p.GetCustomDataByKey('blender:sourceId'))[-1] == name)
        row = {'name': name, 'control': str(prim.GetPath())}
        controller.SetTool(gizmoUI.TOOL_SELECT)
        api.dataModel.selection.clearPrims(); pump()
        picker_click(prim)
        row['picker_selected_control'] = True
        if (prim.GetAttribute('guide:displayOpacity').Get() or 0) > 0:
            api.dataModel.selection.clearPrims(); pump()
            values = (ctypes.c_double * 16)()
            assert read(handle.CacheId(), str(prim.GetPath()).encode(), api.frame.GetValue(), api.frame.IsDefault(), values)
            guide = Gf.Matrix4d(*values)
            points = prim.GetAttribute('guide:points').Get()
            counts = prim.GetAttribute('guide:curveVertexCounts').Get()
            samples, offset = [], 0
            for count in counts:
                for i in range(count-1):
                    samples.append((Gf.Vec3d(points[offset+i])+Gf.Vec3d(points[offset+i+1]))/2)
                offset += count
            picked = None
            for angle in (0, 45, -90, 180):
                api.dataModel.viewSettings.freeCamera.rotTheta += angle
                pump()
                camera, _ = view.resolveCamera()
                projection = gizmoScreen.ViewProjection(camera)
                viewport = view.computeWindowViewport()
                for sample in samples[::max(1, len(samples)//80)]:
                    p = gizmoScreen.ProjectPoint(projection, viewport, guide.Transform(sample))
                    if not p:
                        continue
                    inside, frustum = view.computePickFrustum(*p)
                    hits = view.pick(frustum) if inside else []
                    if hits and hits[0].hitPrimPath == prim.GetPath():
                        picked = (p[0]/view.devicePixelRatioF(), p[1]/view.devicePixelRatioF())
                        break
                if picked:
                    break
            assert picked, 'Visible curve did not pick its editable owner: '+name
            click(view, picked)
            assert api.dataModel.selection.getPrimPaths() == [prim.GetPath()]
            row['curve_selected_control'] = True
        else:
            row['curve_hidden_in_source'] = True
        controller.SetTool(gizmoUI.TOOL_TRANSLATE)
        controller.SetOrientation(gizmoSettings.ORIENT_WORLD)
        pump()
        target = controller.Target()
        assert target and not target.Advisory()
        before = gizmoMath.ComputeRigFrames(stage, prim, api.frame).posed
        target.BeginDrag(); target.ApplyTranslate(Gf.Vec3d(0.02, 0.03, -0.01)); target.writer.CommitToStage()
        handle.SetTime(api.frame.GetValue()); pump()
        after = gizmoMath.ComputeRigFrames(stage, prim, api.frame).posed
        error = (after.ExtractTranslation()-before.ExtractTranslation()-Gf.Vec3d(0.02,0.03,-0.01)).GetLength()
        assert error < 2e-5, (name, error)
        row['world_translation_error'] = error
        # An ordinary mouse drag must enter preview and commit editable channels.
        handles = controller.HandleScreenPositions()
        a, b = handles['x'][0], handles['x'][-1]
        start = tuple(a[i]+(b[i]-a[i])*.45 for i in range(2))
        end = tuple(a[i]+(b[i]-a[i])*.7 for i in range(2))
        send(view, QtCore.QEvent.MouseButtonPress, start, QtCore.Qt.LeftButton, QtCore.Qt.LeftButton)
        assert controller.IsDragging()
        send(view, QtCore.QEvent.MouseMove, end, QtCore.Qt.NoButton, QtCore.Qt.LeftButton)
        send(view, QtCore.QEvent.MouseButtonRelease, end, QtCore.Qt.LeftButton, QtCore.Qt.NoButton)
        assert not controller.IsDragging()
        final = gizmoMath.ComputeRigFrames(stage, prim, api.frame).posed
        assert (final.ExtractTranslation()-after.ExtractTranslation()).GetLength() > 1e-5
        row['mouse_drag_moved_control'] = True
        view.grabFramebuffer().save(str(output / (label+'-'+name.replace('.', '_')+'.png')))
        report['controls'].append(row)
        print(row, flush=True)
    report['passed'] = True
    (output / (label+'-controls.json')).write_text(json.dumps(report, indent=2)+'\n')
