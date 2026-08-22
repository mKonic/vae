#pragma once

#include "vae/gpu/CommandList.h"
#include "platform/vulkan/VulkanCommon.h"

namespace vae::gpu {

    class VulkanDevice;
    class VulkanPipeline;
    class VulkanRenderTarget;

    class VulkanCommandList final : public CommandList {
    public:
        explicit VulkanCommandList(VulkanDevice& device) : m_Device(device) {}

        void Begin(VkCommandBuffer cmd, u32 swapchainImageIndex);
        void End();

        void BeginRenderPass(const RenderPassDesc& desc) override;
        void EndRenderPass() override;

        void BindPipeline(const Ref<Pipeline>& pipeline) override;
        void BindBindGroup(const Ref<BindGroup>& group) override;
        void BindVertexBuffer(u32 slot, const Ref<Buffer>& buffer, u64 offset = 0) override;
        void BindIndexBuffer(const Ref<Buffer>& buffer, u64 offset = 0) override;

        void SetViewport(const Viewport& vp) override;
        void SetScissor(const ScissorRect& rect) override;
        void SetPushConstants(const void* data, u32 size) override;

        void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override;
        void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                         i32 vertexOffset, u32 firstInstance) override;

        VkCommandBuffer Raw() const { return m_Cmd; }
        bool TouchedSwapchain() const { return m_TouchedSwapchain; }

    private:
        // Sync2 image barrier. Every layout change in this backend goes through one place so a
        // missing transition is a compile-time-visible omission rather than a validation surprise.
        void Barrier(VkImage image, VkImageLayout from, VkImageLayout to,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                     VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                     VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

        VulkanDevice&   m_Device;
        VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
        u32             m_ImageIndex = 0;

        VulkanPipeline*     m_BoundPipeline = nullptr;
        VulkanRenderTarget* m_CurrentTarget = nullptr;
        bool m_InPass = false;
        bool m_TouchedSwapchain = false;
    };

}
