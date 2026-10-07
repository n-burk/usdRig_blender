"""usdview integration for Blender rig control picks and native pose editing."""
from pxr import Tf
from pxr.Usdviewq.qt import QtCore
from pxr.Usdviewq.plugin import PluginContainer
from . import blenderControlModel
import sessionRegistry

_containers = sessionRegistry.SessionRegistry("Blender rig usdview containers")


class BlenderRigContainer(PluginContainer):
    def registerPlugins(self, registry, usdviewApi):
        self._api, self._changing = usdviewApi, False
        _containers.Set(usdviewApi,self)
        import rigExecUsdview
        blenderControlModel.install_warming(rigExecUsdview.RigExecUsdviewContainer)
        native = rigExecUsdview.ContainerFor(usdviewApi)
        if native:
            # It may have connected the original bound method before the
            # companion loaded. Update this window's signal connection.
            signal = usdviewApi.dataModel.currentFrameChanged
            signal.disconnect(native._blender_original_frame)
            signal.connect(native._OnFrameChanged)
        usdviewApi.dataModel.selection.signalPrimSelectionChanged.connect(self._selectionChanged)
        usdviewApi.dataModel.signalStageReplaced.connect(self._stageChanged)
        self._stageChanged()

    def configureView(self, registry, builder):
        pass

    def _stageChanged(self, *unused):
        QtCore.QTimer.singleShot(0, self._install)

    def _install(self):
        try:
            import gizmoMath
            blenderControlModel.install(gizmoMath)
        except ImportError:
            pass

    def _selectionChanged(self, *unused):
        if self._changing:
            return
        selected = list(self._api.selectedPrims or [])
        owners = [blenderControlModel.control_owner(p) for p in selected]
        if owners == selected:
            return
        self._changing = True
        try:
            selection = self._api.dataModel.selection
            with selection.batchPrimChanges:
                selection.clearPrims()
                for prim in dict.fromkeys(owners):
                    if prim:
                        selection.addPrim(prim)
        finally:
            self._changing = False


Tf.Type.Define(BlenderRigContainer)
