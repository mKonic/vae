#pragma once

#include "vae/gpu/Resources.h"

namespace vae::gpu {

    class Swapchain;

    struct RenderPassDesc {
        // Exactly one of these. Null target = render into the swapchain image.
        RenderTarget* target = nullptr;
        LoadOp loadOp = LoadOp::Clear;
        Color clearColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        f32 clearDepth = 1.0f;
    };

    class CommandList {
    public:
        virtual ~CommandList() = default;

        virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
        virtual void EndRenderPass() = 0;

        virtual void BindPipeline(const Ref<Pipeline>& pipeline) = 0;
        virtual void BindBindGroup(const Ref<BindGroup>& group) = 0;
        virtual void BindVertexBuffer(u32 slot, const Ref<Buffer>& buffer, u64 offset = 0) = 0;
        virtual void BindIndexBuffer(const Ref<Buffer>& buffer, u64 offset = 0) = 0;

        virtual void SetViewport(const Viewport& vp) = 0;
        virtual void SetScissor(const ScissorRect& rect) = 0;
        virtual void SetPushConstants(const void* data, u32 size) = 0;

        virtual void Draw(u32 vertexCount, u32 instanceCount = 1,
                          u32 firstVertex = 0, u32 firstInstance = 0) = 0;
        virtual void DrawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0,
                                 i32 vertexOffset = 0, u32 firstInstance = 0) = 0;
    };

}
