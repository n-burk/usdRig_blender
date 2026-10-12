"""Exercise packaged native picker pages through stock usdview mouse events.

Run with bin/usdview.sh and TESTUSDVIEW --testScript on any converted rig. Set
USDBLENDERRIG_PICKER_PROOF for JSON and panel screenshots. Optional
expectations, none of them built in for a particular rig:
USDBLENDERRIG_PICKER_CONTROLS, a JSON list of required source bone names;
USDBLENDERRIG_PICKER_SETTINGS, the number of unavailable source settings;
USDBLENDERRIG_PICKER_MIRROR_CLICKS, the number of Alt-click mirror selections.
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
    report = {'stage': str(source), 'completed': False, 'panels': [], 'clicks': [], 'group_clicks': 0, 'mirror_clicks': []}
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
    # Every authored panel has a live action, so the stock UI hides none.
    # Unsupported source settings are picker metadata with reasons, not
    # buttons; record them rather than claiming visible tabs.
    visible_panels = {view._panel.id for view in panel._views}
    report['unavailable_settings'] = 0
    for picker in panel._pickers:
        for item in picker.panels:
            assert item.id in visible_panels, 'Panel without a live action: ' + item.id
            for button in stage.GetPrimAtPath(item.id).GetChildren():
                assert not button.GetCustomDataByKey('blender:unavailable'), button.GetPath()
                assert not button.GetCustomDataByKey('blender:source'), button.GetPath()
        if not picker.panels:
            continue
        prim = stage.GetPrimAtPath(picker.panels[0].id).GetParent()
        assert prim.GetTypeName() == 'RigExecPicker', prim.GetPath()
        unavailable = list(prim.GetCustomDataByKey('blender:unavailable') or [])
        reasons = list(prim.GetCustomDataByKey('blender:unsupportedReason') or [])
        assert len(unavailable) == len(reasons) and all(reasons), prim.GetPath()
        report['unavailable_settings'] += len(unavailable)
        assert prim.GetCustomDataByKey('blender:controlCount') == len(
            {t for b in picker.buttons if b.live for t in b.targets}), prim.GetPath()
    # The caller states how many source settings the rig has, if it knows.
    expected_settings = os.environ.get('USDBLENDERRIG_PICKER_SETTINGS')
    assert expected_settings is None or report['unavailable_settings'] == int(expected_settings), report['unavailable_settings']
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

    # The stock hit test gives a click to the topmost bounding box, so a live
    # button covered by another one cannot be used. Every selecting button
    # must be the first hit somewhere inside its own rectangle.
    report['reachable_buttons'] = 0
    for picker in panel._pickers:
        for button in picker.buttons:
            if button.live and button.targets:
                assert reachable(picker, button), 'Covered live button: ' + button.parent + '/' + button.id
                report['reachable_buttons'] += 1
    checks = []
    for name in wanted:
        preferred = sorted(candidates.get(name, []), key=lambda match:
            match[0].panel(match[1].parent).label not in ('Body', 'Face'))
        match = next(((p, b) for p, b in preferred if reachable(p, b)), None)
        assert match, 'No clickable button for source control: ' + name
        checks.append(match)
    for view in panel._views:
        picker = view._picker
        live = [b for b in picker.visible(view._panel.id) if b.live and b.targets and reachable(picker, b)]
        assert live, 'No clickable selecting button on panel: ' + view._panel.label
        # One control per panel where it has one, and every source group
        # button of an overflow panel: a group selects its whole set.
        single = next((b for b in live if len(b.targets) == 1), None)
        sets = [b for b in live if len(b.targets) > 1 and view._panel.id.rsplit('/', 1)[-1].startswith('page_')]
        checks.extend((picker, b) for b in ([single] if single else []) + sets or live[:1])
        # Natively each IK slot is stacked on its FK slot behind a mode dial.
        # The template separates them, so both halves take a real click.
        for side in 'LR':
            for fk, ik in (('Hand', 'ArmIK'), ('Foot', 'LegIK'), ('UpLeg', 'LegPV')):
                pair = [b for b in picker.visible(view._panel.id)
                        if b.live and b.id in ('b_%s_%s' % (side, fk), 'b_%s_%s' % (side, ik))]
                if len(pair) == 2:
                    assert all(reachable(picker, b) for b in pair), 'Stacked %s slots: %s' % (fk, side)
                    checks.extend((picker, b) for b in pair)

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
        # usdview reports a multi-selection in its own order: a group is compared as a set.
        assert sorted(chosen) == sorted(button.targets) and len(chosen) == len(set(chosen)), (button.text, chosen, button.targets)
        assert len(chosen) > 1 or chosen == list(button.targets), (button.text, chosen, button.targets)
        report['clicks'].append({'panel': view._panel.label, 'button': button.text, 'targets': chosen})
        report['group_clicks'] += len(chosen) > 1 and view._panel.id.rsplit('/', 1)[-1].startswith('page_')
        if view._panel.id not in visited:
            visited.add(view._panel.id)
            path = output / ('panel-%02d.png' % len(visited))
            view.grab().save(str(path))
            report['panels'].append({'label': view._panel.label, 'image': path.name})
    # Exercise the authored button pairing via Alt-click, using the same
    # mouse event path as ordinary clicks; never synthesize a selection.
    # The pair is found by role slot, not by a rig's bone names. A mirror link
    # is only authored between two live slots; a rig without a biped panel
    # (no anatomical names) has none to exercise.
    def live_peer(picker, button):
        return next((b for b in picker.visible(button.parent)
                     if b.id == button.mirror.rsplit('/', 1)[-1] and b.live), None)

    for label in ('Body', 'Face'):
        slots = [(p, b) for matches in candidates.values() for p, b in matches
                 if b.parent.rsplit('/', 1)[-1] in ('Body', 'Facial') and p.panel(b.parent).label == label]
        mirrored = [(p, b) for p, b in slots if b.mirror]
        for picker, button in mirrored:
            peer = live_peer(picker, button)
            assert peer and peer.targets and peer.targets != button.targets, button.id
        match = next(((p, b) for p, b in mirrored if reachable(p, b)), None)
        if not match:
            assert not mirrored, 'No reachable mirrored slot on ' + label
            continue
        picker, button = match
        name = json.loads(stage.GetPrimAtPath(button.targets[0]).GetCustomDataByKey('blender:sourceId'))[-1]
        view, x, y = reachable(picker, button)
        peer = live_peer(picker, button)
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
    # One Alt-click per template panel that has a mirrored slot; a rig that
    # fills both panels states 2.
    expected_mirrors = os.environ.get('USDBLENDERRIG_PICKER_MIRROR_CLICKS')
    assert expected_mirrors is None or len(report['mirror_clicks']) == int(expected_mirrors), report['mirror_clicks']
    print('PICKER_COVERAGE_OK', len(covered), len(report['clicks']),
          'reachable_buttons', report['reachable_buttons'],
          'group_clicks', report['group_clicks'],
          'mirror_clicks', len(report['mirror_clicks']),
          'unavailable_settings', report['unavailable_settings'], flush=True)
