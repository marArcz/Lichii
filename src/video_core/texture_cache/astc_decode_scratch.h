// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace VideoCommon {

constexpr u64 MaxAstcDecodeBatchBytes = u64{64} << 20;
constexpr u64 MaxPendingAstcDecodeBytes = u64{512} << 20;

[[nodiscard]] constexpr u64 AstcDecodeLayerBytes(u32 width, u32 height, u32 levels) {
    u64 bytes{};
    for (u32 level = 0; level < levels; ++level) {
        bytes += u64{width} * height * 4;
        width = (std::max)(width / 2, 1U);
        height = (std::max)(height / 2, 1U);
    }
    return bytes;
}

[[nodiscard]] constexpr u32 AstcDecodeBatchLayers(u64 layer_bytes, u32 layers) {
    if (layers == 0) {
        return 0;
    }
    return static_cast<u32>((std::clamp<u64>)(
        MaxAstcDecodeBatchBytes / (std::max)(layer_bytes, u64{1}), 1, layers));
}

[[nodiscard]] constexpr bool AstcDecodeNeedsWait(u64 pending_bytes, u64 next_bytes) {
    // A single layer cannot be split further. Allow it alone, including its actual mip size.
    const u64 limit = (std::max)(MaxPendingAstcDecodeBytes, next_bytes);
    return pending_bytes > limit - next_bytes;
}

} // namespace VideoCommon
