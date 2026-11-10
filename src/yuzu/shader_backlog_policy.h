// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <optional>
#include "video_core/shader_jobs.h"

class ShaderBacklogPolicy {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr u64 MinimumPendingBuilds = 64;

    bool Observe(const VideoCommon::ShaderBuildProgress& progress, bool eligible,
                 Clock::time_point now) {
        using namespace std::chrono_literals;
        if (!eligible || !progress.supported || suppressed) {
            high_since.reset();
            low_since.reset();
            return false;
        }
        if (progress.Pending() < 8) {
            if (!low_since) {
                low_since = now;
            }
            if (now - *low_since >= 2s) {
                armed = true;
            }
        } else {
            low_since.reset();
        }
        if (progress.Pending() < MinimumPendingBuilds) {
            high_since.reset();
            return false;
        }
        if (!high_since) {
            high_since = now;
        }
        return armed && now >= cooldown_until && now - *high_since >= 2s && progress.oldest >= 1s;
    }

    void Prompted(Clock::time_point now) {
        armed = false;
        high_since.reset();
        low_since.reset();
        cooldown_until = now + std::chrono::seconds{60};
    }

    void Suppress(bool value) {
        suppressed = value;
    }

private:
    bool armed{true};
    bool suppressed{};
    std::optional<Clock::time_point> high_since;
    std::optional<Clock::time_point> low_since;
    Clock::time_point cooldown_until{Clock::time_point::min()};
};
