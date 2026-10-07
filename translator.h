#pragma once
#include "pxr/usd/sdf/layer.h"
#include <string>
namespace usdBlenderRig {
// Versioned JSON snapshot, independent of Blender's Python and USD builds.
pxr::SdfLayerRefPtr Translate(const std::string &json, const std::string &source,
                             bool metadataOnly, bool strict);
std::string Extract(const std::string &source);
}
