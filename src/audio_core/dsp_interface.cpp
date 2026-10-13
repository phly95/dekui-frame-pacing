// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include "audio_core/dsp_interface.h"
#include "audio_core/sink.h"
#include "audio_core/sink_details.h"
#include "common/assert.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/dumping/backend.h"
#ifdef ENABLE_MM3D_PRESENTATION
#include "mm3d/vblank_pacer.h"
#endif

namespace AudioCore {

DspInterface::DspInterface(Core::System& system_) : system(system_) {}

DspInterface::~DspInterface() = default;

void DspInterface::SetSink(AudioCore::SinkType sink_type, std::string_view audio_device) {
    // Dispose of the current sink first to avoid contention.
    sink.reset();

    sink = AudioCore::GetSinkDetails(sink_type).create_sink(audio_device);
    sink->SetCallback(
        [this](s16* buffer, std::size_t num_frames) { OutputCallback(buffer, num_frames); });
    time_stretcher.SetOutputSampleRate(sink->GetNativeSampleRate());
}

Sink& DspInterface::GetSink() {
    ASSERT(sink);
    return *sink.get();
}

void DspInterface::EnableStretching(bool enable) {
    enable_time_stretching = enable;
}

void DspInterface::OutputFrame(StereoFrame16 frame) {
    if (!sink) {
        return;
    }

    if (sink->ImmediateSubmission()) {
        sink->PushSamples(frame.data(), frame.size());
    } else {
        fifo.Push(frame.data(), frame.size());
    }

    auto video_dumper = system.GetVideoDumper();
    if (video_dumper && video_dumper->IsDumping()) {
        video_dumper->AddAudioFrame(std::move(frame));
    }
}

void DspInterface::OutputSample(std::array<s16, 2> sample) {
    if (!sink) {
        return;
    }

    if (sink->ImmediateSubmission()) {
        sink->PushSamples(&sample, 1);
    } else {
        fifo.Push(&sample, 1);
    }

    auto video_dumper = system.GetVideoDumper();
    if (video_dumper && video_dumper->IsDumping()) {
        video_dumper->AddAudioSample(std::move(sample));
    }
}

void DspInterface::OutputCallback(s16* buffer, std::size_t num_frames) {
    // Determine if we should stretch based on the current emulation speed.
    // TODO: Only activate audio stretching when emulation speed goes below 95% threshold
    //       (see #2487) -OS
    if (performing_time_stretching && !enable_time_stretching) {
        // If we just stopped stretching, flush the stretcher before returning to normal output.
        flushing_time_stretcher = true;
    }
    performing_time_stretching = enable_time_stretching.load();

    std::size_t frames_written = 0;
    if (performing_time_stretching) {
        const std::vector<s16> in{fifo.Pop()};
        const std::size_t num_in{in.size() / 2};
        frames_written = time_stretcher.Process(in.data(), num_in, buffer, num_frames);
    } else {
        if (flushing_time_stretcher) {
            time_stretcher.Flush();
            frames_written = time_stretcher.Process(nullptr, 0, buffer, num_frames);
            flushing_time_stretcher = false;

            // Make sure any frames that did not fit are cleared from the time stretcher,
            // so that they do not bleed into the next time the stretcher is enabled.
            time_stretcher.Clear();
        }
#ifdef ENABLE_MM3D_PRESENTATION
        const double speed_scale =
            MM3D::VblankPacing::speed_scale.load(std::memory_order_relaxed);
        if (speed_scale != 1.0 && frames_written == 0) {
            frames_written = ResampleFromFifo(buffer, num_frames, speed_scale);
        } else {
            // Leaving rate control: play out what was already taken from the FIFO first.
            if (!resample_buffer.empty()) {
                const std::size_t pending = std::min(resample_buffer.size() / 2,
                                                     num_frames - frames_written);
                std::copy_n(resample_buffer.begin(), pending * 2, buffer + frames_written * 2);
                resample_buffer.erase(resample_buffer.begin(),
                                      resample_buffer.begin() + pending * 2);
                frames_written += pending;
                resample_position = 0.0;
            }
            frames_written += fifo.Pop(buffer + frames_written * 2, num_frames - frames_written);
        }
#else
        frames_written += fifo.Pop(buffer, num_frames - frames_written);
#endif
    }

    if (frames_written > 0) {
        std::memcpy(&last_frame[0], buffer + 2 * (frames_written - 1), 2 * sizeof(s16));
    }

    // Hold last emitted frame; this prevents popping.
    for (std::size_t i = frames_written; i < num_frames; i++) {
        std::memcpy(buffer + 2 * i, &last_frame[0], 2 * sizeof(s16));
    }

    // Implementation of the hardware volume slider
    // A cubic curve is used to approximate a linear change in human-perceived loudness
    const float linear_volume = std::clamp(Settings::Volume(), 0.0f, 1.0f);
    if (linear_volume != 1.0) {
        const float volume_scale_factor = linear_volume * linear_volume * linear_volume;
        for (std::size_t i = 0; i < num_frames; i++) {
            buffer[i * 2 + 0] = static_cast<s16>(buffer[i * 2 + 0] * volume_scale_factor);
            buffer[i * 2 + 1] = static_cast<s16>(buffer[i * 2 + 1] * volume_scale_factor);
        }
    }
}

#ifdef ENABLE_MM3D_PRESENTATION
std::size_t DspInterface::ResampleFromFifo(s16* buffer, std::size_t num_frames,
                                           double speed_scale) {
    // Target fill: one output callback plus two emulated vblanks of audio (~547 frames each),
    // enough to ride out the emulator's bursty delivery without adding stretcher latency.
    const double target = double(num_frames) + 2.0 * 547.0;
    const double fill = double(fifo.Size() + resample_buffer.size() / 2);
    resample_fill += (fill - resample_fill) / 16.0;
    const double error = std::clamp((resample_fill - target) / target, -1.0, 1.0);
    const double ratio = speed_scale * (1.0 + 0.005 * error);

    // Input frames needed: positions up to the last output frame, plus its right neighbour.
    const double last = resample_position + double(num_frames - 1) * ratio;
    const std::size_t needed = static_cast<std::size_t>(last) + 2;
    const std::size_t have = resample_buffer.size() / 2;
    if (needed > have) {
        resample_buffer.resize(needed * 2);
        const std::size_t got = fifo.Pop(resample_buffer.data() + have * 2, needed - have);
        resample_buffer.resize((have + got) * 2);
    }
    const std::size_t available = resample_buffer.size() / 2;

    std::size_t written = 0;
    for (; written < num_frames; ++written) {
        const auto index = static_cast<std::size_t>(resample_position);
        if (index + 1 >= available) {
            break; // FIFO ran dry; the caller holds the last frame
        }
        const double t = resample_position - double(index);
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const double a = resample_buffer[index * 2 + channel];
            const double b = resample_buffer[(index + 1) * 2 + channel];
            buffer[written * 2 + channel] = static_cast<s16>(std::lround(a + (b - a) * t));
        }
        resample_position += ratio;
    }

    const std::size_t consumed =
        std::min(static_cast<std::size_t>(resample_position), available ? available - 1 : 0);
    resample_buffer.erase(resample_buffer.begin(), resample_buffer.begin() + consumed * 2);
    resample_position -= double(consumed);
    return written;
}
#endif

} // namespace AudioCore
