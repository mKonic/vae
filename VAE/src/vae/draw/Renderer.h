#pragma once

#include "vae/draw/DrawList.h"
#include "vae/gpu/Device.h"

#include <unordered_map>

namespace vae::draw {

    // Turns a DrawList into GPU work. One instanced draw per batch, so a whole screen of UI is
    // typically two or three draws.
    class Renderer {
    public:
        bool Init(gpu::Device& device, gpu::Format colorFormat);
        void Shutdown();

        // Must be called once per GPU frame, after Device::BeginFrame. Advances the ring of
        // per-frame instance buffers, resets the write cursors, and applies any growth that last
        // frame asked for (growth only ever happens here, when the slot's previous submission has
        // already retired).
        void NewFrame();

        // Appends this list's batches to the current frame. Calling it more than once per frame is
        // legal and each call appends — a Renderer that refilled its buffer from offset zero would
        // silently erase the earlier scene before the GPU ever read it.
        void Render(gpu::CommandList& cmd, const DrawList& list, Vec2 viewportSize);

        u32 DrawCallsLastFrame() const { return m_DrawCalls; }

    private:
        struct Slot {
            Ref<gpu::Buffer> quads;
            Ref<gpu::Buffer> shadows;
            u32 quadCapacity = 0, shadowCapacity = 0;
            u32 quadCursor = 0, shadowCursor = 0;
        };

        Ref<gpu::BindGroup> QuadBindGroup(const Slot& slot, const DrawList& list, const Batch& batch);
        Ref<gpu::BindGroup> ShadowBindGroup(const Slot& slot);
        void EnsureCapacity(Slot& slot, u32 quads, u32 shadows);

        gpu::Device* m_Device = nullptr;

        Ref<gpu::Pipeline> m_QuadPipeline;
        Ref<gpu::Pipeline> m_ShadowPipeline;
        Ref<gpu::Texture>  m_White;                 // fills unused slots in the texture array

        std::vector<Slot> m_Slots;
        u32 m_SlotIndex = 0;

        // Descriptor sets are cached by (buffer, texture set) because a UI's texture set barely
        // changes between frames; rebuilding one per batch per frame would be pure churn.
        std::unordered_map<u64, Ref<gpu::BindGroup>> m_BindGroups;

        u32 m_QuadHighWater = 0, m_ShadowHighWater = 0;
        u32 m_DrawCalls = 0;
        bool m_WarnedOverflow = false;
    };

}
