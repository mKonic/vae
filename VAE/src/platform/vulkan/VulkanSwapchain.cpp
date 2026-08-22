#include "vaepch.h"
#include "platform/vulkan/VulkanSwapchain.h"

#include "platform/vulkan/VulkanDevice.h"

#include <VkBootstrap.h>

namespace vae::gpu {

    VulkanSwapchain::VulkanSwapchain(VulkanDevice& device, VkSurfaceKHR surface,
                                     u32 width, u32 height, bool vsync)
        : m_Device(device), m_Surface(surface), m_VSync(vsync) {
        Build(width, height);
    }

    VulkanSwapchain::~VulkanSwapchain() { Destroy(); }

    void VulkanSwapchain::Build(u32 width, u32 height) {
        if (width == 0 || height == 0) { m_Width = width; m_Height = height; return; }

        vkb::SwapchainBuilder builder{ m_Device.Physical(), m_Device.Raw(), m_Surface };

        // UNORM, not SRGB. Colour management is a P3 decision (sRGB authoring, linear blending,
        // encode at the final composite); picking an SRGB swapchain here would silently
        // double-encode everything drawn before that decision lands.
        auto result = builder
            .set_desired_format(VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM,
                                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
            .set_desired_present_mode(m_VSync ? VK_PRESENT_MODE_FIFO_KHR
                                              : VK_PRESENT_MODE_IMMEDIATE_KHR)
            .set_desired_extent(width, height)
            .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            .set_old_swapchain(m_Swapchain)
            .build();

        if (!result) {
            VAE_CORE_ERROR("swapchain build failed: {}", result.error().message());
            return;
        }

        Destroy();

        vkb::Swapchain built = result.value();
        m_Swapchain = built.swapchain;
        m_Format    = built.image_format;
        m_Width     = built.extent.width;
        m_Height    = built.extent.height;
        m_Images    = built.get_images().value();
        m_Views     = built.get_image_views().value();

        m_RenderFinished.resize(m_Images.size());
        for (auto& sem : m_RenderFinished) {
            VkSemaphoreCreateInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            VK_CHECK(vkCreateSemaphore(m_Device.Raw(), &info, nullptr, &sem));
        }

        VAE_CORE_INFO("swapchain {}x{}, {} images", m_Width, m_Height, m_Images.size());
    }

    void VulkanSwapchain::Destroy() {
        if (!m_Swapchain && m_Views.empty()) return;
        for (auto view : m_Views) vkDestroyImageView(m_Device.Raw(), view, nullptr);
        for (auto sem : m_RenderFinished) vkDestroySemaphore(m_Device.Raw(), sem, nullptr);
        if (m_Swapchain) vkDestroySwapchainKHR(m_Device.Raw(), m_Swapchain, nullptr);
        m_Views.clear();
        m_Images.clear();
        m_RenderFinished.clear();
        m_Swapchain = VK_NULL_HANDLE;
    }

    void VulkanSwapchain::Resize(u32 width, u32 height) {
        if (width == m_Width && height == m_Height) return;
        m_Device.WaitIdle();
        Build(width, height);
    }

}
