// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <mutex>
#include <optional>
#include <vector>
#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#ifdef ENABLE_MM3D_PRESENTATION
#include "mm3d/vblank_pacer.h"
#endif

namespace Vulkan {

class Instance;
class Scheduler;

class Swapchain {
public:
    explicit Swapchain(const Instance& instance, u32 width, u32 height, vk::SurfaceKHR surface,
                       bool low_refresh_rate);
    ~Swapchain();

    /// Creates (or recreates) the swapchain with a given size.
    void Create(u32 width, u32 height, vk::SurfaceKHR surface, bool low_refresh_rate);

    /// Acquires the next image in the swapchain.
    bool AcquireNextImage();

    /// Presents the current image and move to the next one
    void Present();

#ifdef ENABLE_MM3D_PRESENTATION
    /// Refresh period and the latest actual present time (VK_GOOGLE_display_timing), if known.
    std::optional<MM3D::VblankPacing::DisplayTiming> GetDisplayTiming();

    /// True when presents queue for vblank (the only mode a refresh-locked pacer makes sense in).
    bool IsFifo() const {
        return present_mode == vk::PresentModeKHR::eFifo;
    }
#endif

    vk::SurfaceKHR GetSurface() const {
        return surface;
    }

    vk::Image Image() const {
        return images[image_index];
    }

    vk::SurfaceFormatKHR GetSurfaceFormat() const {
        return surface_format;
    }

    vk::SwapchainKHR GetHandle() const {
        return swapchain;
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    u32 GetImageCount() const {
        return image_count;
    }

    vk::Extent2D GetExtent() const {
        return extent;
    }

    [[nodiscard]] vk::Semaphore GetImageAcquiredSemaphore() const {
        return image_acquired[frame_index];
    }

    [[nodiscard]] vk::Semaphore GetPresentReadySemaphore() const {
        return present_ready[image_index];
    }

private:
    /// Selects the best available swapchain image format
    void FindPresentFormat();

    /// Sets the best available present mode
    void SetPresentMode();

    /// Sets the surface properties according to device capabilities
    void SetSurfaceProperties();

    /// Destroys current swapchain resources
    void Destroy();

    /// Performs creation of image views and framebuffers from the swapchain images
    void SetupImages();

    /// Creates the image acquired and present ready semaphores
    void RefreshSemaphores();

private:
    const Instance& instance;
    vk::SwapchainKHR swapchain{VK_NULL_HANDLE};
    vk::SurfaceKHR surface{};
    vk::SurfaceFormatKHR surface_format;
    vk::PresentModeKHR present_mode;
    vk::Extent2D extent;
    vk::SurfaceTransformFlagBitsKHR transform;
    vk::CompositeAlphaFlagBitsKHR composite_alpha;
    std::vector<vk::Image> images;
    std::vector<vk::Semaphore> image_acquired;
    std::vector<vk::Semaphore> present_ready;
    u32 width = 0;
    u32 height = 0;
    u32 image_count = 0;
    u32 image_index = 0;
    u32 frame_index = 0;
    bool needs_recreation = true;
    bool low_refresh_rate;
#ifdef ENABLE_MM3D_PRESENTATION
    u32 present_id = 0;
    u32 timing_queries = 0;
    s64 refresh_period = 0;
    s64 last_vblank = 0;
#endif
};

} // namespace Vulkan
