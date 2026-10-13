// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

// Locks presentation to the display's real refresh grid.
//
// Every emulated 3DS vblank owns `step` display refreshes (1 on a ~60 Hz display). A frame
// produced at emulated vblank n is assigned to a fixed display refresh and submitted halfway
// through the refresh before it, so host jitter in when the frame reaches the presenter can
// no longer move it onto a neighbouring refresh. Interpolated pairs submit the midpoint on the
// pair's own vblank and the native frame on the next one, so the two land one refresh apart.
//
// Timing comes from VK_GOOGLE_display_timing (refresh period + actual present times). When the
// display rate is not an integer multiple of the 3DS rate, or timing is unavailable, Deadline()
// returns nullopt and the caller keeps its previous pacing.
namespace MM3D::VblankPacing {

// Player settings (presentation.json): lock presentation to the display refresh, and nudge the
// emulation speed so the 3DS rate matches the display exactly (e.g. +0.28% on a 60.000 Hz TV).
inline std::atomic<bool> lock_enabled{true}, match_rate{true};
// Emulation speed multiplier published by the presenter while locked; 1.0 when inactive. The frame
// limiter applies it at 100% speed and the audio output resamples by the same ratio.
inline std::atomic<double> speed_scale{1.0};

// 3DS LCD period: FRAME_TICKS / BASE_CLOCK_RATE_ARM11 (4481136 / 268111856 s).
constexpr std::int64_t EmulatedPeriod = 16713681;

struct DisplayTiming {
    std::int64_t period; // refresh period, ns
    std::int64_t vblank; // a recent actual present (vblank) time, steady clock ns
};

struct Stats {
    std::uint64_t locked{}, resyncs{}, shrinks{};
};

class Pacer {
    std::optional<std::int64_t> anchor_time; // display time of anchor_slot
    std::int64_t anchor_slot{};
    std::int64_t min_slack{std::numeric_limits<std::int64_t>::max()};
    unsigned window{};
    double slack_average{};
    double scale{1.0};
    Stats stats{};

    static std::int64_t Snap(std::int64_t t, const DisplayTiming& d) {
        const double k = std::round(double(t - d.vblank) / double(d.period));
        return d.vblank + std::int64_t(k) * d.period;
    }
    std::int64_t Resync(std::int64_t now, std::int64_t slot, const DisplayTiming& d) {
        // First refresh whose submit point leaves `Slack` of headroom after this arrival.
        const std::int64_t earliest = now + d.period / 2 /*lead*/ + d.period / 2 /*slack*/;
        const auto k = std::int64_t(std::ceil(double(earliest - d.vblank) / double(d.period)));
        anchor_time = d.vblank + k * d.period;
        anchor_slot = slot;
        min_slack = std::numeric_limits<std::int64_t>::max();
        window = 0;
        slack_average = double(d.period) / 2;
        ++stats.resyncs;
        return *anchor_time - d.period / 2;
    }

public:
    // Frames observed between latency reductions; long enough to see the emulator's worst jitter.
    static constexpr unsigned ShrinkWindow = 300;

    void Reset() {
        anchor_time.reset();
        scale = 1.0;
    }
    // Emulation speed that makes one 3DS vblank last exactly `step` refreshes, with a small
    // correction (at most +-0.5%) that holds the average headroom at half a refresh so latency
    // stays constant instead of drifting into a resync or a dropped refresh.
    double SpeedScale() const { return scale; }
    const Stats& GetStats() const { return stats; }

    // Returns the steady-clock time at which to submit the frame for emulated vblank
    // `emu_vblank` (already advanced by one for the native frame of an interpolated pair),
    // or nullopt when presentation cannot be locked to the display. `pair_native` marks the
    // second frame of a pair: it is ready together with its midpoint, so its headroom says
    // nothing about emulator timing and is left out of the latency statistics.
    std::optional<std::int64_t> Deadline(std::int64_t now, std::int64_t emu_vblank,
                                         const std::optional<DisplayTiming>& display,
                                         bool pair_native = false) {
        if (!display || display->period < 4'000'000 || display->period > 50'000'000 ||
            std::llabs(now - display->vblank) > 1'000'000'000) {
            Reset();
            return std::nullopt;
        }
        const auto& d = *display;
        const auto step = std::llround(double(EmulatedPeriod) / double(d.period));
        if (step < 1 || std::llabs(EmulatedPeriod - step * d.period) * 50 > d.period) {
            Reset();
            return std::nullopt;
        }
        ++stats.locked;
        const std::int64_t slot = emu_vblank * step;
        if (!anchor_time) {
            scale = double(EmulatedPeriod) / double(step * d.period);
            return Resync(now, slot, d);
        }
        const std::int64_t target = Snap(*anchor_time + (slot - anchor_slot) * d.period, d);
        const std::int64_t deadline = target - d.period / 2;
        // Too late to make the assigned refresh (keep ~a quarter refresh for the compositor),
        // or a discontinuity (emulation paused/reset): start a new timeline from this frame.
        if (now > target - d.period / 4 || deadline - now > 4 * d.period) {
            return Resync(now, slot, d);
        }
        // If even the earliest frame of a whole window could have shown a refresh sooner,
        // latency has grown (display slightly slower than the 3DS clock): drop one refresh.
        if (pair_native) {
            return deadline;
        }
        const std::int64_t slack = deadline - now;
        slack_average += (double(slack) - slack_average) / 32;
        const double error = (slack_average - double(d.period) / 2) / double(d.period);
        scale = double(EmulatedPeriod) / double(step * d.period) *
                (1.0 - std::clamp(error * 0.01, -0.005, 0.005));
        if (slack < min_slack) {
            min_slack = slack;
        }
        if (++window >= ShrinkWindow) {
            if (min_slack > d.period + d.period / 4) {
                *anchor_time -= d.period;
                ++stats.shrinks;
            }
            min_slack = std::numeric_limits<std::int64_t>::max();
            window = 0;
        }
        return deadline;
    }
};

} // namespace MM3D::VblankPacing
