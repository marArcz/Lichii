// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/astc_decode_scratch.h"

using namespace VideoCommon;

TEST_CASE("ASTC decode scratch counts the host mip chain", "[video_core][astc]") {
    REQUIRE(AstcDecodeLayerBytes(7, 5, 3) == (35 + 6 + 1) * 4);
    REQUIRE(AstcDecodeLayerBytes(1, 8, 4) == (8 + 4 + 2 + 1) * 4);
    REQUIRE(AstcDecodeLayerBytes(7, 5, 1) == 140);
    REQUIRE(AstcDecodeLayerBytes(16384, 16384, 1) == u64{1} << 30);
}

TEST_CASE("ASTC array batches fit the decode budget where one layer fits", "[video_core][astc]") {
    const u64 layer_bytes = AstcDecodeLayerBytes(1365, 1365, 11);
    const u32 batch = AstcDecodeBatchLayers(layer_bytes, 90);
    REQUIRE(batch == 6);
    REQUIRE(layer_bytes * batch <= MaxAstcDecodeBatchBytes);
    REQUIRE(layer_bytes * (batch + 1) > MaxAstcDecodeBatchBytes);
    REQUIRE(AstcDecodeBatchLayers(layer_bytes, 3) == 3);
    REQUIRE(AstcDecodeBatchLayers(16, 90) == 90);
}

TEST_CASE("ASTC decode accounts for an oversized single layer", "[video_core][astc]") {
    const u64 mipmapped_layer = AstcDecodeLayerBytes(4096, 4096, 13);
    REQUIRE(mipmapped_layer > MaxAstcDecodeBatchBytes);
    REQUIRE(AstcDecodeBatchLayers(mipmapped_layer, 90) == 1);
    REQUIRE_FALSE(AstcDecodeNeedsWait(MaxPendingAstcDecodeBytes - mipmapped_layer,
                                    mipmapped_layer));
    REQUIRE(AstcDecodeNeedsWait(MaxPendingAstcDecodeBytes - mipmapped_layer + 1,
                              mipmapped_layer));

    const u64 layer_bytes = AstcDecodeLayerBytes(12288, 12288, 1);
    REQUIRE(layer_bytes == u64{576} << 20);
    REQUIRE(AstcDecodeBatchLayers(layer_bytes, 4) == 1);
    REQUIRE_FALSE(AstcDecodeNeedsWait(0, layer_bytes));
    REQUIRE(AstcDecodeNeedsWait(1, layer_bytes));
}

TEST_CASE("ASTC decode waits before exceeding the pending budget", "[video_core][astc]") {
    REQUIRE_FALSE(AstcDecodeNeedsWait(MaxPendingAstcDecodeBytes - MaxAstcDecodeBatchBytes,
                                    MaxAstcDecodeBatchBytes));
    REQUIRE(AstcDecodeNeedsWait(MaxPendingAstcDecodeBytes - MaxAstcDecodeBatchBytes + 1,
                              MaxAstcDecodeBatchBytes));
    REQUIRE(AstcDecodeNeedsWait(MaxPendingAstcDecodeBytes, 1));
    REQUIRE_FALSE(AstcDecodeNeedsWait(0, std::numeric_limits<u64>::max()));
    REQUIRE(AstcDecodeNeedsWait(1, std::numeric_limits<u64>::max()));
}
