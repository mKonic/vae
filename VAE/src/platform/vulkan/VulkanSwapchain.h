#pragma once

#include "vae/gpu/Device.h"
#include "platform/vulkan/VulkanCommon.h"

namespace vae::gpu {

    class VulkanDevice;

    class VulkanSwapchain final : public Swapchain {
    public:
        VulkanSwapchain(VulkanDevice& device, VkSurfaceKHR surface, u32 width, u32 height, bool vsync);
        ~VulkanSwapchain() override;

        u32 Width()  const override { return m_Width; }
        u32 Height() const override { return m_Height; }
        Format ColorFormat() const override { return FromVk(m_Format); }
        void Resize(u32 width, u32 height) override;

        VkSwapchainKHR Raw() const { return m_Swapchain; }
        VkFormat VkColorFormat() const { return m_Format; }
        VkImage     Image(u32 i) const { return m_Images[i]; }
        VkImageView View(u32 i)  const { return m_Views[i]; }
        VkSemaphore RenderFinished(u32 i) const { return m_RenderFinished[i]; }
        u32 ImageCount() const { return static_cast<u32>(m_Images.size()); }
        bool Valid() const { return m_Swapchain != VK_NULL_HANDLE && m_Width > 0 && m_Height > 0; }

    private:
        void Build(u32 width, u32 height);
        void Destroy();

        VulkanDevice&  m_Device;
        VkSurfaceKHR   m_Surface   = VK_NULL_HANDLE;
        VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
        VkFormat       m_Format    = VK_FORMAT_UNDEFINED;
        u32            m_Width = 0, m_Height = 0;
        bool           m_VSync = true;

        std::vector<VkImage>     m_Images;
        std::vector<VkImageView> m_Views;
        // One per swapchain image, not per frame-in-flight: the semaphore a present waits on must
        // belong to the image being presented, or two frames can signal the same semaphore.
        std::vector<VkSemaphore> m_RenderFinished;
    };

}
