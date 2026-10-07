// Test-only inspection of the same immutable snapshot the Hydra results index
// reads. A lookup never activates, evaluates, publishes, or authors a stage.
#include <rigExecImaging/registry.h>
#include <rigExecImaging/snapshotStore.h>

#include <limits>

PXR_NAMESPACE_USING_DIRECTIVE

extern "C" int UsdBlenderRigView_ReadPoints(
    long long stageCacheId, const char *primPath, double frame, int isDefault,
    float *out, int maxPoints)
{
    if (!primPath || maxPoints < 0) return -2;
    auto context = rigExec::RigExecImagingRegistry::ForStageCacheId(stageCacheId, false);
    if (!context) return -2;
    auto snapshot = context->GetStore()->Get();
    const auto time = isDefault ? UsdTimeCode::Default() : UsdTimeCode(frame);
    if (!snapshot || !snapshot->Describes(context->GetBoundStage(), time)) return -2;
    const auto entry = snapshot->prims.find(SdfPath(primPath));
    if (entry == snapshot->prims.end() || !entry->second.hasPoints) return -1;
    const auto &points = entry->second.points;
    if (points.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return -2;
    const int count = static_cast<int>(points.size());
    if (!out || maxPoints < count) return count;
    for (int i = 0; i < count; ++i)
        for (int axis = 0; axis < 3; ++axis)
            out[3 * static_cast<size_t>(i) + axis] = points[i][axis];
    return count;
}
