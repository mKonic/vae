#pragma once

#include "vae/gpu/Device.h"
#include "platform/vulkan/VulkanCommon.h"

#include <deque>
#include <functional>

namespace vae::gpu {

    class VulkanSwapchain;
    class VulkanCommandList;

    class VulkanDevice final : public Device {
    public:
        explicit VulkanDevice(const DeviceDesc& desc);
        ~VulkanDevice() override;

        bool Ok() const { return m_Ok; }

        const DeviceCaps& Caps() const override { return m_Caps; }
        Backend GetBackend() const override { return Backend::Vulkan; }
        Swapchain* GetSwapchain() override;

        Ref<Buffer>       CreateBuffer(const BufferDesc&) override;
        Ref<Texture>      CreateTexture(const TextureDesc&) override;
        Ref<Shader>       CreateShader(const ShaderDesc&) override;
        Ref<Pipeline>     CreatePipeline(const PipelineDesc&) override;
        Ref<BindGroup>    CreateBindGroup(const Ref<Pipeline>&, const std::vector<BindGroupEntry>&) override;
        Ref<RenderTarget> CreateRenderTarget(const RenderTargetDesc&) override;

        CommandList* BeginFrame() override;
        void EndFrame() override;
        void WaitIdle() override;
        void OnWindowResize(u32 width, u32 height) override;

        // --- internals used by the other Vulkan objects -----------------------------------------
        VkDevice          Raw() const { return m_Device; }
        VkPhysicalDevice  Physical() const { return m_Physical; }
        VkInstance        Instance() const { return m_Instance; }
        VmaAllocator      Allocator() const { return m_Allocator; }
        VkQueue           GraphicsQueue() const { return m_GraphicsQueue; }
        u32               GraphicsQueueFamily() const { return m_GraphicsFamily; }
        VkDescriptorPool  DescriptorPool() const { return m_DescriptorPool; }

        // Runs a one-shot command buffer and blocks until it retires. Used for uploads and layout
        // transitions outside the frame — never on a per-frame path.
        void ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn);

        VkCommandBuffer CurrentCommandBuffer() const;
        VulkanSwapchain* SwapchainImpl() const { return m_Swapchain.get(); }
        u32 CurrentFrameIndex() const { return m_FrameIndex; }

    private:
        // Instance, surface and device are one step: vk-bootstrap's PhysicalDeviceSelector needs
        // the live vkb::Instance, which is not something worth storing past initialisation.
        bool InitVulkan(const DeviceDesc&);
        bool InitAllocator();
        bool InitFrames();
        void DestroyFrames();

        struct Frame {
            VkCommandPool   pool = VK_NULL_HANDLE;
            VkCommandBuffer cmd  = VK_NULL_HANDLE;
            VkSemaphore     imageAvailable = VK_NULL_HANDLE;
            VkFence         inFlight = VK_NULL_HANDLE;
        };

        VkInstance               m_Instance   = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_Messenger  = VK_NULL_HANDLE;
        VkSurfaceKHR             m_Surface    = VK_NULL_HANDLE;
        VkPhysicalDevice         m_Physical   = VK_NULL_HANDLE;
        VkDevice                 m_Device     = VK_NULL_HANDLE;
        VkQueue                  m_GraphicsQueue = VK_NULL_HANDLE;
        u32                      m_GraphicsFamily = 0;
        VmaAllocator             m_Allocator  = nullptr;
        VkDescriptorPool         m_DescriptorPool = VK_NULL_HANDLE;

        VkCommandPool            m_ImmediatePool = VK_NULL_HANDLE;
        VkFence                  m_ImmediateFence = VK_NULL_HANDLE;

        Scope<VulkanSwapchain>   m_Swapchain;
        Scope<VulkanCommandList> m_CommandList;

        std::vector<Frame>       m_Frames;
        u32                      m_FrameIndex = 0;
        u32                      m_ImageIndex = 0;
        bool                     m_FrameOpen  = false;
        bool                     m_Ok         = false;
        bool                     m_Validation = false;

        DeviceCaps               m_Caps;
        Window*                  m_Window = nullptr;
    };

}
