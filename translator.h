#pragma once
#include "pxr/usd/sdf/layer.h"
#include <string>
namespace usdBlenderRig {
// Versioned JSON snapshot, independent of Blender's Python and USD builds.
// sourceData embeds the snapshot JSON as inert /Rig customData blender:sourceData.
// pruneUnobserved omits armatures nothing observes; the result is not 1:1.
// probeWholeRig runs the lower-limb calibration probe on the whole rig instead
// of its dependency closure: same layer, slower, and diagnosed.
pxr::SdfLayerRefPtr Translate(const std::string &json, const std::string &source,
                             bool metadataOnly, bool strict, bool sourceData=false,
                             bool pruneUnobserved=false, bool probeWholeRig=false);
std::string Extract(const std::string &source);
}
