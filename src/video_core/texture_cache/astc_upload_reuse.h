// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <span>
#include <utility>

#include "common/common_types.h"

namespace VideoCommon {

class AstcUploadReuse {
public:
    static constexpr size_t MaxTextureBytes = 1U << 20;
    static constexpr size_t MaxEntries = 4;

    [[nodiscard]] std::span<u8> ReadBuffer(size_t size) {
        if (size == 0 || size > MaxTextureBytes) {
            return {};
        }
        if (!read_buffer) {
            read_buffer.reset(new (std::nothrow) u8[MaxTextureBytes]);
        }
        return read_buffer ? std::span{read_buffer.get(), size} : std::span<u8>{};
    }

    [[nodiscard]] bool Matches(size_t image, u64 tick, std::span<const u8> bytes) const {
        const auto it = Find(image);
        return it != entries.end() && it->tick == tick &&
               std::ranges::equal(std::span{it->bytes.get(), it->size}, bytes);
    }

    void Remember(size_t image, u64 tick, std::span<const u8> bytes) {
        if (bytes.empty() || bytes.size() > MaxTextureBytes) {
            Forget(image);
            return;
        }
        const auto found = Find(image);
        auto& entry = found != entries.end() ? *found : entries[next];
        if (found == entries.end()) {
            next = (next + 1) % entries.size();
        }
        if (entry.size != bytes.size()) {
            entry.bytes.reset();
            entry.bytes.reset(new (std::nothrow) u8[bytes.size()]);
            if (!entry.bytes) {
                entry = {};
                return;
            }
        }
        entry.image = image;
        entry.tick = tick;
        entry.size = bytes.size();
        std::ranges::copy(bytes, entry.bytes.get());
    }

    void Forget(size_t image) {
        if (const auto it = Find(image); it != entries.end()) {
            *it = {};
        }
    }

private:
    struct Entry {
        size_t image{};
        u64 tick{};
        size_t size{};
        std::unique_ptr<u8[]> bytes;
    };

    auto Find(size_t image) const -> std::array<Entry, MaxEntries>::const_iterator {
        return std::ranges::find_if(entries, [image](const Entry& entry) {
            return entry.bytes && entry.image == image;
        });
    }

    auto Find(size_t image) -> std::array<Entry, MaxEntries>::iterator {
        return entries.begin() + (std::as_const(*this).Find(image) - entries.cbegin());
    }

    std::array<Entry, MaxEntries> entries;
    std::unique_ptr<u8[]> read_buffer;
    size_t next{};
};

} // namespace VideoCommon
