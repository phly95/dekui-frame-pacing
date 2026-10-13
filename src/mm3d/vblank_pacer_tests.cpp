// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
#include "mm3d/vblank_pacer.h"
using namespace MM3D::VblankPacing;
static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// Display refresh index a submission lands on (shown at the first vblank after submit).
static std::int64_t Refresh(std::int64_t submit, const DisplayTiming& d) {
    std::int64_t k = (submit - d.vblank) / d.period;
    if (d.vblank + k * d.period <= submit) ++k; // relative to the fixed grid origin
    return k;
}

struct Result {
    std::vector<std::int64_t> refreshes;
    std::uint64_t resyncs;
};

// Simulates emulator output: one frame per `every` emulated vblanks (pairs add a native frame
// on the next vblank), arriving with uniform host jitter of +-jitter_ns.
static Result Run(std::int64_t display_period, unsigned every, bool pairs, std::int64_t jitter_ns,
                  unsigned frames) {
    Pacer pacer;
    const std::int64_t origin = 1'000'000'000;
    DisplayTiming d{display_period, origin};
    std::mt19937_64 rng{42};
    std::uniform_int_distribution<std::int64_t> jitter{-jitter_ns, jitter_ns};
    Result r{};
    std::int64_t vblank = 100;
    for (unsigned i = 0; i < frames; ++i, vblank += every) {
        std::int64_t arrival = origin + 2 * d.period + (vblank - 100) * EmulatedPeriod + jitter(rng);
        for (unsigned pass = 0; pass < (pairs ? 2u : 1u); ++pass) {
            // The presenter only knows vblanks that already happened.
            d.vblank = origin + (arrival - origin) / d.period * d.period;
            const auto deadline = pacer.Deadline(arrival, vblank + pass, d, pass == 1);
            Check(deadline.has_value(), "pacer should lock to a ~60 Hz display");
            const std::int64_t submit = std::max(arrival, *deadline);
            r.refreshes.push_back(Refresh(submit, {d.period, origin}));
            arrival = submit + 50'000; // native frame of a pair is ready right after the midpoint
        }
    }
    r.resyncs = pacer.GetStats().resyncs;
    return r;
}

// Counts refresh gaps that differ from the expected cadence (a repeat or a skip on screen).
static unsigned Judders(const std::vector<std::int64_t>& r, std::int64_t expected) {
    unsigned bad = 0;
    for (std::size_t i = 1; i < r.size(); ++i)
        if (r[i] - r[i - 1] != expected) ++bad;
    return bad;
}

int main() {
    try {
        // 30 fps native frames on a display matched to the 3DS rate, +-7 ms arrival jitter:
        // every frame must hold exactly two refreshes (only the initial lock may resync).
        {
            const auto r = Run(16713681, 2, false, 7'000'000, 3000);
            Check(Judders(r.refreshes, 2) <= r.resyncs - 1, "30 fps frames must hold exactly 2 refreshes once locked");
            Check(r.resyncs <= 2, "jitter within tolerance must settle after the initial lock");
        }
        // Interpolated pairs: midpoint and native must land on consecutive refreshes.
        {
            const auto r = Run(16713681, 2, true, 7'000'000, 3000);
            Check(Judders(r.refreshes, 1) <= r.resyncs - 1, "pair frames must be one refresh apart once locked");
            Check(r.resyncs <= 2, "pairs within tolerance must settle after the initial lock");
        }
        // TV at 59.826 Hz vs 3DS 59.831 Hz: the display is slower, so latency is trimmed with a
        // rare single skip instead of growing without bound.
        {
            const auto r = Run(16715316, 2, true, 4'000'000, 20000);
            Check(Judders(r.refreshes, 1) <= 6, "rate mismatch must cost only rare skips");
        }
        // 120 Hz display: each 3DS vblank owns two refreshes.
        {
            const auto r = Run(16713681 / 2, 2, true, 3'000'000, 2000);
            Check(Judders(r.refreshes, 2) == 0, "120 Hz pairs must be two refreshes apart");
        }
        // 60.000 Hz TV with rate matching: the emulator follows SpeedScale(), so after the
        // initial lock every pair lands on consecutive refreshes with no resyncs or skips.
        {
            Pacer pacer;
            const std::int64_t origin = 1'000'000'000, period = 16666667;
            std::mt19937_64 rng{7};
            std::uniform_int_distribution<std::int64_t> jitter{-5'000'000, 5'000'000};
            std::vector<std::int64_t> refreshes;
            double emu_time = double(origin + 2 * period);
            for (std::int64_t vblank = 0; vblank < 2 * 20000; vblank += 2) {
                std::int64_t arrival = std::int64_t(emu_time) + jitter(rng);
                for (unsigned pass = 0; pass < 2; ++pass) {
                    const DisplayTiming d{period, origin + (arrival - origin) / period * period};
                    const auto deadline = pacer.Deadline(arrival, vblank + pass, d, pass == 1);
                    Check(deadline.has_value(), "60.000 Hz must lock");
                    const std::int64_t submit = std::max(arrival, *deadline);
                    refreshes.push_back(Refresh(submit, {period, origin}));
                    arrival = submit + 50'000;
                }
                emu_time += 2.0 * double(EmulatedPeriod) / pacer.SpeedScale();
            }
            Check(pacer.SpeedScale() > 1.002 && pacer.SpeedScale() < 1.0035,
                  "60.000 Hz needs ~+0.28% speed");
            Check(Judders(refreshes, 1) <= pacer.GetStats().resyncs - 1 + pacer.GetStats().shrinks,
                  "judders only from resyncs/shrinks");
            Check(pacer.GetStats().resyncs <= 2 && pacer.GetStats().shrinks == 0,
                  "rate matching must keep the lock without corrections");
        }
        // 144 Hz is not an integer multiple: the pacer must decline.
        {
            Pacer pacer;
            const DisplayTiming d{6944444, 1'000'000'000};
            Check(!pacer.Deadline(1'000'000'000, 5, d), "non-integer ratio must fall back");
            Check(!pacer.Deadline(1'000'000'000, 5, std::nullopt), "no timing must fall back");
        }
        // A frame far too late (emulation hitch) resyncs instead of piling up latency.
        {
            Pacer pacer;
            const DisplayTiming d{16713681, 0};
            auto t = pacer.Deadline(1'000, 10, d);
            Check(t.has_value(), "lock");
            const auto late = pacer.Deadline(*t + 10 * 16713681, 11, d);
            Check(late && *late > *t + 10 * 16713681, "late frame resyncs ahead of now");
            Check(pacer.GetStats().resyncs == 2, "late frame counted as resync");
        }
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
    std::cout << "vblank pacer tests passed\n";
    return 0;
}
