#pragma once
#include "pipeline/blp_loader.hpp"
#include <cstddef>

namespace wowee::pspconv {
// Match the desktop foliage rule: an opaque bark material must not turn
// an atlas/specular alpha channel into holes through the trunk. Genuine
// silhouettes (leaf cards, grass and DXT1 punch-through) retain their alpha.
inline void prepareMaterialTextureAlpha(pipeline::BLPImage& image, bool opaque, bool foliage) {
    if (!opaque || (foliage && image.alphaIsSilhouette())) return;
    for (size_t i=3; i<image.data.size(); i+=4) image.data[i]=255;
}
}
