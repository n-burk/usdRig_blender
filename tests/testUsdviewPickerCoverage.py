"""Exercise packaged native picker pages through stock usdview mouse events.

Run with bin/usdview.sh and TESTUSDVIEW --testScript. Set
USDBLENDERRIG_PICKER_PROOF for JSON and panel screenshots. Optional
USDBLENDERRIG_PICKER_CONTROLS is a JSON list of required source bone names.
"""
import hashlib
import json
import os
from pathlib import Path

from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets


def testUsdviewInputFunction(app):
    import pickerUI
    api = app._usdviewApi
    stage = api.stage
    output = Path(os.environ.get('USDBLENDERRIG_PICKER_PROOF', '/tmp/native-picker-proof'))
    output.mkdir(parents=True, exist_ok=True)
    source = Path(stage.GetRootLayer().realPath)
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    report = {'stage': str(source), 'completed': False, 'panels': [], 'clicks': [],
              'unavailable_panels': [], 'mirror_clicks': []}
    panel = pickerUI.OpenPickerPanel(api)
    app._processEvents()
    wanted = json.loads(os.environ.get('USDBLENDERRIG_PICKER_CONTROLS', '[]'))
    candidates = {}
    covered = set()
    for picker in panel._pickers:
        for button in picker.buttons:
            if not button.live or not button.targets:
                continue
            for path in button.targets:
                target = stage.GetPrimAtPath(path)
                assert target and target.GetTypeName() == 'RigExecControl', path
                ident = target.GetCustomDataByKey('blender:sourceId')
                assert ident, 'Picker must select native editable source controls'
                for channel in ('tx', 'ty', 'tz', 'rx', 'ry', 'rz', 'sx', 'sy', 'sz'):
                    attribute = target.GetAttribute('avars:' + channel)
                    assert attribute and not attribute.HasAuthoredConnections(), attribute.GetPath()
                covered.add(ident)
                if len(button.targets) == 1:
                    candidates.setdefault(json.loads(ident)[-1], []).append((picker, button))
    # Stock UI skips panels containing no live actions. Record their authored
    # unsupported settings independently, rather than claiming visible tabs.
    visible_panels = {view._panel.id for view in panel._views}
    for picker in panel._pickers:
        for item in picker.panels:
            if item.id in visible_panels:
                continue
            prim = stage.GetPrimAtPath(item.id)
            buttons = list(prim.GetChildren())
            assert 'source settings (unavailable)' in item.label.lower(), item.label
            assert buttons and all(b.GetCustomDataByKey('blender:unavailable') and
                                   b.GetCustomDataByKey('blender:unsupportedReason') and
                                   not b.GetRelationship('rigExec:picker:controls').GetTargets()
                                   for b in buttons)
            report['unavailable_panels'].append({'label': item.label, 'path': item.id,
                'buttons': len(buttons), 'visible': False,
                'reason': 'Stock picker hides panels with zero live actions'})
    report['unavailable_settings'] = sum(p['buttons'] for p in report['unavailable_panels'])
    assert len(report['unavailable_panels']) == 3
    assert report['unavailable_settings'] == 115
    report['functional_settings'] = 0
    assert covered, 'No source controls in packaged picker'
    report['unique_source_controls'] = len(covered)

    def reachable(picker, button):
        view = next(v for v in panel._views
                    if v._picker is picker and v._panel.id == button.parent)
        for fx in (0.5, 0.25, 0.75, 0.1, 0.9):
            for fy in (0.5, 0.25, 0.75, 0.1, 0.9):
                x, y = button.x + button.w * fx, button.y + button.h * fy
                hits = picker.hits(button.parent, x, y, view._modes, False)
                if hits and hits[0].id == button.id:
                    return view, x, y
        return None

    checks = []
    for name in wanted:
        preferred = sorted(candidates.get(name, []), key=lambda match:
            match[0].panel(match[1].parent).label not in ('Body', 'Face'))
        match = next(((p, b) for p, b in preferred if reachable(p, b)), None)
        assert match, 'No clickable button for source control: ' + name
        checks.append(match)
    for view in panel._views:
        picker = view._picker
        match = next(((picker, b) for b in picker.visible(view._panel.id)
                      if b.live and len(b.targets) == 1 and reachable(picker, b)), None)
        if not match:
            buttons = [b for b in picker.buttons if b.parent == view._panel.id and b.text]
            assert 'unavailable' in view._panel.label.lower() and buttons, view._panel.label
            assert all(not b.live and 'unavailable' in b.text.lower() for b in buttons), view._panel.label
            report['unavailable_panels'].append({'label': view._panel.label, 'buttons': len(buttons)})
            continue
        checks.append(match)

    visited = set()
    for picker, button in checks:
        view, x, y = reachable(picker, button)
        scroll = None
        for outer in range(panel._tabs.count()):
            inner = panel._tabs.widget(outer)
            for index in range(inner.count()):
                if inner.widget(index).widget() is view:
                    panel._tabs.setCurrentIndex(outer)
                    inner.setCurrentIndex(index)
                    scroll = inner.widget(index)
        assert scroll is not None
        app._processEvents()
        point = QtCore.QPointF((x - view._ox) * view._scale(), (y - view._oy) * view._scale())
        scroll.ensureVisible(int(point.x()), int(point.y()), 20, 20)
        app._processEvents()
        api.dataModel.selection.clearPrims()
        for kind, buttons in ((QtCore.QEvent.MouseButtonPress, QtCore.Qt.LeftButton),
                              (QtCore.QEvent.MouseButtonRelease, QtCore.Qt.NoButton)):
            event = QtGui.QMouseEvent(kind, point, view.mapToGlobal(point.toPoint()),
                                     QtCore.Qt.LeftButton, buttons, QtCore.Qt.NoModifier)
            QtWidgets.QApplication.sendEvent(view, event)
            app._processEvents()
        chosen = [str(p) for p in api.dataModel.selection.getPrimPaths()]
        assert chosen == list(button.targets), (button.text, chosen, button.targets)
        report['clicks'].append({'panel': view._panel.label, 'button': button.text, 'targets': chosen})
        if view._panel.id not in visited:
            visited.add(view._panel.id)
            path = output / ('panel-%02d.png' % len(visited))
            view.grab().save(str(path))
            report['panels'].append({'label': view._panel.label, 'image': path.name})
    # Exercise the authored button pairing via Alt-click, using the same
    # mouse event path as ordinary clicks; never synthesize a selection.
    for label, name in (('Body', 'FK-UpperArm.L'), ('Face', 'CTR-Eye.L')):
        match = next(((p, b) for p, b in candidates.get(name, [])
                      if p.panel(b.parent).label == label and b.mirror and reachable(p, b)), None)
        assert match, 'No reachable mirrored semantic alias: ' + name
        picker, button = match
        view, x, y = reachable(picker, button)
        peer = next((b for b in picker.visible(button.parent)
                     if b.id == button.mirror.rsplit('/', 1)[-1] and b.live), None)
        assert peer and peer.targets and peer.targets != button.targets
        for outer in range(panel._tabs.count()):
            inner = panel._tabs.widget(outer)
            for index in range(inner.count()):
                if inner.widget(index).widget() is view:
                    panel._tabs.setCurrentIndex(outer)
                    inner.setCurrentIndex(index)
                    scroll = inner.widget(index)
        app._processEvents()
        point = QtCore.QPointF((x - view._ox) * view._scale(), (y - view._oy) * view._scale())
        scroll.ensureVisible(int(point.x()), int(point.y()), 20, 20)
        app._processEvents()
        api.dataModel.selection.clearPrims()
        for kind, buttons in ((QtCore.QEvent.MouseButtonPress, QtCore.Qt.LeftButton),
                              (QtCore.QEvent.MouseButtonRelease, QtCore.Qt.NoButton)):
            event = QtGui.QMouseEvent(kind, point, view.mapToGlobal(point.toPoint()),
                                     QtCore.Qt.LeftButton, buttons, QtCore.Qt.AltModifier)
            QtWidgets.QApplication.sendEvent(view, event)
            app._processEvents()
        chosen = [str(p) for p in api.dataModel.selection.getPrimPaths()]
        assert set(chosen) == set(button.targets) | set(peer.targets), (label, chosen, button.targets, peer.targets)
        report['mirror_clicks'].append({'panel': label, 'source_control': name,
                                       'button': button.id, 'mirror': peer.id, 'targets': chosen})
    panel.close()
    report['source_unchanged'] = hashlib.sha256(source.read_bytes()).hexdigest() == before
    assert report['source_unchanged']
    report['completed'] = True
    (output / 'picker.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PICKER_COVERAGE_OK', len(covered), len(report['clicks']),
          'mirror_clicks', len(report['mirror_clicks']),
          'hidden_settings', report['unavailable_settings'], flush=True)
