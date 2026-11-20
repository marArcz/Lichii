// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/astc_upload_reuse.h"

using VideoCommon::AstcUploadReuse;

TEST_CASE("ASTC reuse requires identical bytes for the same resident image", "[video_core][astc]") {
    AstcUploadReuse cache;
    std::array<u8, 4> bytes{1, 2, 3, 4};
    REQUIRE_FALSE(cache.Matches(5, 0, bytes));
    cache.Remember(5, 0, bytes);
    REQUIRE(cache.Matches(5, 0, bytes));
    REQUIRE_FALSE(cache.Matches(5, 1, bytes));
    REQUIRE_FALSE(cache.Matches(6, 0, bytes));
    REQUIRE_FALSE(cache.Matches(5, 0, std::span{bytes}.first(3)));

    bytes[3] = 5;
    REQUIRE_FALSE(cache.Matches(5, 0, bytes));
    cache.Remember(5, 0, bytes);
    REQUIRE(cache.Matches(5, 0, bytes));
    cache.Forget(5);
    REQUIRE_FALSE(cache.Matches(5, 0, bytes));
}

TEST_CASE("ASTC reuse bounds resident snapshots", "[video_core][astc]") {
    AstcUploadReuse cache;
    const std::array<u8, 1> bytes{7};
    for (size_t i = 0; i <= AstcUploadReuse::MaxEntries; ++i) {
        cache.Remember(i, 0, bytes);
    }
    REQUIRE_FALSE(cache.Matches(0, 0, bytes));
    REQUIRE(cache.Matches(AstcUploadReuse::MaxEntries, 0, bytes));

    const std::vector<u8> large(AstcUploadReuse::MaxTextureBytes + 1);
    cache.Remember(1, 0, large);
    REQUIRE_FALSE(cache.Matches(1, 0, large));
    REQUIRE_FALSE(cache.Matches(1, 0, bytes));
    REQUIRE(cache.ReadBuffer(large.size()).empty());
    REQUIRE(cache.ReadBuffer(0).empty());
    REQUIRE(cache.ReadBuffer(AstcUploadReuse::MaxTextureBytes).size() ==
            AstcUploadReuse::MaxTextureBytes);
}
