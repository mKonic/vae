#include "vaepch.h"
#include "platform/vulkan/VulkanCommandList.h"

#include "platform/vulkan/VulkanDevice.h"
#include "platform/vulkan/VulkanResources.h"
#include "platform/vulkan/VulkanSwapchain.h"

namespace vae::gpu {

    void VulkanCommandList::Begin(VkCommandBuffer cmd, u32 swapchainImageIndex) {
        m_Cmd = cmd;
        m_ImageIndex = swapchainImageIndex;
        m_BoundPipeline = nullptr;
        m_CurrentTarget = nullptr;
        m_InPass = false;
        m_TouchedSwapchain = false;
    }

    void VulkanCommandList::End() { m_Cmd = VK_NULL_HANDLE; }

    void VulkanCommandList::Barrier(VkImage image, VkImageLayout from, VkImageLayout to,
                                    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                                    VkImageAspectFlags aspect) {
        VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = { aspect, 0, 1, 0, 1 };

        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(m_Cmd, &dep);
    }

    void VulkanCommandList::BeginRenderPass(const RenderPassDesc& desc) {
        VAE_CORE_ASSERT(!m_InPass, "BeginRenderPass while a pass is already open");
        m_InPass = true;
        m_CurrentTarget = static_cast<VulkanRenderTarget*>(desc.target);

        VkImageView colorView = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        u32 width = 0, height = 0;

        if (m_CurrentTarget) {
            auto* color = static_cast<VulkanTexture*>(m_CurrentTarget->ColorTexture().get());
            Barrier(color->Image(), color->Layout(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            color->SetLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            colorView = m_CurrentTarget->ColorView();
            depthView = m_CurrentTarget->DepthView();
            width  = m_CurrentTarget->Width();
            height = m_CurrentTarget->Height();
        } else {
            auto* sc = m_Device.SwapchainImpl();
            VAE_CORE_ASSERT(sc && sc->Valid(), "no swapchain to render into");

            if (!m_TouchedSwapchain) {
                Barrier(sc->Image(m_ImageIndex), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                m_TouchedSwapchain = true;
            }
            colorView = sc->View(m_ImageIndex);
            width  = sc->Width();
            height = sc->Height();
        }

        VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        color.imageView   = colorView;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp      = desc.loadOp == LoadOp::Clear    ? VK_ATTACHMENT_LOAD_OP_CLEAR
                          : desc.loadOp == LoadOp::Load     ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                            : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = { { desc.clearColor.r, desc.clearColor.g,
                                     desc.clearColor.b, desc.clearColor.a } };

        VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        depth.imageView   = depthView;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = { desc.clearDepth, 0 };

        VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        info.renderArea = { {0, 0}, { width, height } };
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = depthView ? &depth : nullptr;

        vkCmdBeginRendering(m_Cmd, &info);

        // NOT flipped. Vulkan's clip space already has +y pointing down, which is exactly the UI
        // convention, so the negative-height viewport that game engines use to recover OpenGL's
        // y-up would invert every layout rect. Verified visually at P2: a vertex authored at
        // y = -0.6 must draw near the TOP of the window.
        VkViewport vp{ 0.0f, 0.0f,
                       static_cast<f32>(width), static_cast<f32>(height), 0.0f, 1.0f };
        vkCmdSetViewport(m_Cmd, 0, 1, &vp);
        VkRect2D scissor{ {0, 0}, { width, height } };
        vkCmdSetScissor(m_Cmd, 0, 1, &scissor);
    }

    void VulkanCommandList::EndRenderPass() {
        VAE_CORE_ASSERT(m_InPass, "EndRenderPass without a pass");
        vkCmdEndRendering(m_Cmd);
        m_InPass = false;

        if (m_CurrentTarget) {
            // Leave an offscreen target sampleable, since the next thing that touches it is almost
            // always a shader reading it back (a layer composite, or the Studio canvas).
            auto* color = static_cast<VulkanTexture*>(m_CurrentTarget->ColorTexture().get());
            Barrier(color->Image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            color->SetLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        m_CurrentTarget = nullptr;
    }

    void VulkanCommandList::BindPipeline(const Ref<Pipeline>& pipeline) {
        m_BoundPipeline = static_cast<VulkanPipeline*>(pipeline.get());
        vkCmdBindPipeline(m_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_BoundPipeline->Raw());
    }

    void VulkanCommandList::BindBindGroup(const Ref<BindGroup>& group) {
        auto* g = static_cast<VulkanBindGroup*>(group.get());
        VkDescriptorSet set = g->Raw();
        vkCmdBindDescriptorSets(m_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->Layout(), 0, 1, &set, 0, nullptr);
    }

    void VulkanCommandList::BindVertexBuffer(u32 slot, const Ref<Buffer>& buffer, u64 offset) {
        VkBuffer raw = static_cast<VulkanBuffer*>(buffer.get())->Raw();
        vkCmdBindVertexBuffers(m_Cmd, slot, 1, &raw, &offset);
    }

    void VulkanCommandList::BindIndexBuffer(const Ref<Buffer>& buffer, u64 offset) {
        vkCmdBindIndexBuffer(m_Cmd, static_cast<VulkanBuffer*>(buffer.get())->Raw(),
                             offset, VK_INDEX_TYPE_UINT32);
    }

    void VulkanCommandList::SetViewport(const Viewport& vp) {
        VkViewport v{ vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth };
        vkCmdSetViewport(m_Cmd, 0, 1, &v);
    }

    void VulkanCommandList::SetScissor(const ScissorRect& rect) {
        VkRect2D r{ { rect.x, rect.y }, { rect.width, rect.height } };
        vkCmdSetScissor(m_Cmd, 0, 1, &r);
    }

    void VulkanCommandList::SetPushConstants(const void* data, u32 size) {
        VAE_CORE_ASSERT(m_BoundPipeline, "push constants before a pipeline is bound");
        vkCmdPushConstants(m_Cmd, m_BoundPipeline->Layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
    }

    void VulkanCommandList::Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) {
        vkCmdDraw(m_Cmd, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void VulkanCommandList::DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                                        i32 vertexOffset, u32 firstInstance) {
        vkCmdDrawIndexed(m_Cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

}
