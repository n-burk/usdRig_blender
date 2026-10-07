"""Native Blender controls: pick owners and use evaluated channel frames."""
import math
from pxr import Gf


def control_owner(prim):
    if not prim:
        return prim
    rel = prim.GetRelationship('blender:control')
    paths = rel.GetTargets() if rel else []
    owner = prim.GetStage().GetPrimAtPath(paths[0]) if len(paths) == 1 else None
    return owner if owner and owner.GetTypeName() in ('RigExecControl', 'RigExecJoint') else prim


def install(gizmo):
    if getattr(gizmo.ComputeRigFrames, '_blender_adapter', False):
        return
    original = gizmo.ComputeRigFrames

    def frames(stage, prim, time, solverPosed=None, _frameCache=None):
        result = original(stage, prim, time, solverPosed, _frameCache)
        rel = prim.GetRelationship('blender:editorFrame')
        paths = rel.GetTargets() if rel else []
        if len(paths) != 1 or not prim.GetCustomDataByKey('blender:editableChannels'):
            return result
        # This native expression explicitly reads the bone's own avars.
        # Generic usdRig treats any connected posed:space as overwriting
        # them. The published helper carries its evaluated frame instead.
        published = gizmo._ReadPublishedControlFrame(stage, paths[0], time)
        if published is None or not all(math.isfinite(published[r][c]) for r in range(4) for c in range(4)):
            return result
        avars = gizmo.AvarsMatrix(prim, time)
        if abs(avars.GetDeterminant()) < 1e-12 or not math.isfinite(result.unitScale) or abs(result.unitScale) < 1e-12:
            return result
        if any(prim.GetAttribute(n).HasAuthoredConnections() for n in gizmo.AVAR_T + gizmo.AVAR_R + gizmo.AVAR_S):
            return result
        result.posed = Gf.Matrix4d(published)
        result.P = avars.GetInverse() * result.posed
        result.published = True
        result.reason = ''
        return result

    frames._blender_adapter = True
    gizmo.ComputeRigFrames = frames


def install_warming(container_type):
    """Keep static converted rigs from warming hundreds of identical frames.

    Live evaluation and drag previews still run. Authored avar animation
    restores usdRig's normal cache policy, including spline animation.
    """
    if getattr(container_type, '_blender_warming_adapter', False):
        return
    def static(container):
        stage = container._api.dataModel.stage
        if not stage or not stage.GetRootLayer().customLayerData.get('blenderRig:source'):
            return False
        if any(layer.ListAllTimeSamples() for layer in stage.GetLayerStack()):
            return False
        if getattr(container, '_blender_channel_stage', None) != stage:
            container._blender_channel_stage = stage
            container._blender_channels = [a for p in stage.Traverse() for a in p.GetAttributes()
                                           if str(a.GetName()).startswith('avars:')]
        return not any(a.HasSpline() for a in container._blender_channels)
    wake = container_type._WakeWarmingDriver
    flush = container_type._FlushWarmingCommit
    frame = container_type._OnFrameChanged
    def wake_static(container):
        if static(container):
            container._SleepWarmingDriver()
            return
        return wake(container)
    def flush_static(container):
        if static(container):
            container._warmingFlushArmed = False
            container._warmingCommitPending = False
            container._SleepWarmingDriver()
            return
        return flush(container)
    def frame_static(container, value):
        if static(container) and container._active and container._lib:
            container._Imaging().SetTime(container._FrameValue(value))
            container._SleepWarmingDriver()
            return
        return frame(container,value)
    container_type._WakeWarmingDriver = wake_static
    container_type._FlushWarmingCommit = flush_static
    container_type._OnFrameChanged = frame_static
    container_type._blender_original_frame = frame
    container_type._blender_warming_adapter = True
