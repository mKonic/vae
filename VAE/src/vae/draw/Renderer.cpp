#include "vaepch.h"
#include "vae/draw/Renderer.h"

#include "vae/gpu/ShaderUtil.h"

namespace vae::draw {

    namespace {
        constexpr u32 kInitialQuads   = 4096;
        constexpr u32 kInitialShadows = 512;

        struct PushConstants { f32 viewportX, viewportY; };

        u64 HashCombine(u64 seed, u64 value) {
            return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
        }
    }

    bool Renderer::Init(gpu::Device& device, gpu::Format colorFormat) {
        m_Device = &device;

        auto quadVs   = gpu::LoadShader(device, "quad.vert",   gpu::ShaderStage::Vertex);
        auto quadFs   = gpu::LoadShader(device, "quad.frag",   gpu::ShaderStage::Fragment);
        auto shadowVs = gpu::LoadShader(device, "shadow.vert", gpu::ShaderStage::Vertex);
        auto shadowFs = gpu::LoadShader(device, "shadow.frag", gpu::ShaderStage::Fragment);
        if (!quadVs || !quadFs || !shadowVs || !shadowFs) return false;

        {
            gpu::PipelineDesc desc;
            desc.vertex = quadVs;
            desc.fragment = quadFs;
            desc.bindings = {
                { 0, gpu::BindingType::StorageBuffer, 1,
                  static_cast<u8>(gpu::StageBit(gpu::ShaderStage::Vertex)
                                | gpu::StageBit(gpu::ShaderStage::Fragment)) },
                { 1, gpu::BindingType::SampledTexture, DrawList::kMaxTexturesPerBatch,
                  gpu::StageBit(gpu::ShaderStage::Fragment) },
            };
            desc.pushConstantSize = sizeof(PushConstants);
            desc.blend = gpu::BlendMode::AlphaStraight;
            desc.colorFormat = colorFormat;
            desc.debugName = "draw.quad";
            m_QuadPipeline = device.CreatePipeline(desc);
        }
        {
            gpu::PipelineDesc desc;
            desc.vertex = shadowVs;
            desc.fragment = shadowFs;
            desc.bindings = {
                { 0, gpu::BindingType::StorageBuffer, 1,
                  static_cast<u8>(gpu::StageBit(gpu::ShaderStage::Vertex)
                                | gpu::StageBit(gpu::ShaderStage::Fragment)) },
            };
            desc.pushConstantSize = sizeof(PushConstants);
            desc.blend = gpu::BlendMode::AlphaStraight;
            desc.colorFormat = colorFormat;
            desc.debugName = "draw.shadow";
            m_ShadowPipeline = device.CreatePipeline(desc);
        }

        gpu::TextureDesc white;
        white.width = white.height = 1;
        white.format = gpu::Format::RGBA8_UNORM;
        white.debugName = "draw.white";
        m_White = device.CreateTexture(white);
        const u32 pixel = 0xFFFFFFFFu;
        m_White->Upload(&pixel, sizeof(pixel));

        m_Slots.resize(std::max(device.Caps().framesInFlight, 1u));
        for (auto& slot : m_Slots) EnsureCapacity(slot, kInitialQuads, kInitialShadows);

        VAE_CORE_INFO("draw renderer ready ({} frame slots, {} textures per batch)",
                      m_Slots.size(), DrawList::kMaxTexturesPerBatch);
        return m_QuadPipeline && m_ShadowPipeline;
    }

    void Renderer::Shutdown() {
        m_BindGroups.clear();
        m_Slots.clear();
        m_White.reset();
        m_QuadPipeline.reset();
        m_ShadowPipeline.reset();
        m_Device = nullptr;
    }

    void Renderer::EnsureCapacity(Slot& slot, u32 quads, u32 shadows) {
        if (quads > slot.quadCapacity) {
            gpu::BufferDesc desc;
            desc.size = u64(quads) * sizeof(QuadInstance);
            desc.usage = gpu::BufferUsage::Storage;
            desc.memory = gpu::MemoryKind::HostVisible;
            desc.debugName = "draw.quads";
            slot.quads = m_Device->CreateBuffer(desc);
            slot.quadCapacity = quads;
            m_BindGroups.clear();       // the cached sets point at the old buffer
        }
        if (shadows > slot.shadowCapacity) {
            gpu::BufferDesc desc;
            desc.size = u64(shadows) * sizeof(ShadowInstance);
            desc.usage = gpu::BufferUsage::Storage;
            desc.memory = gpu::MemoryKind::HostVisible;
            desc.debugName = "draw.shadows";
            slot.shadows = m_Device->CreateBuffer(desc);
            slot.shadowCapacity = shadows;
            m_BindGroups.clear();
        }
    }

    void Renderer::NewFrame() {
        m_SlotIndex = (m_SlotIndex + 1) % static_cast<u32>(m_Slots.size());
        Slot& slot = m_Slots[m_SlotIndex];

        // Grow to fit whatever last frame needed. This is the only place buffers grow, and it is
        // safe here because Device::BeginFrame already waited on this slot's fence.
        if (m_QuadHighWater > slot.quadCapacity || m_ShadowHighWater > slot.shadowCapacity) {
            EnsureCapacity(slot,
                           std::max(m_QuadHighWater * 2, slot.quadCapacity),
                           std::max(m_ShadowHighWater * 2, slot.shadowCapacity));
            m_WarnedOverflow = false;
        }

        slot.quadCursor = 0;
        slot.shadowCursor = 0;
        m_QuadHighWater = 0;
        m_ShadowHighWater = 0;
        m_DrawCalls = 0;
    }

    Ref<gpu::BindGroup> Renderer::QuadBindGroup(const Slot& slot, const DrawList& list,
                                                const Batch& batch) {
        u64 key = HashCombine(0x9e37u, reinterpret_cast<u64>(slot.quads.get()));
        for (u32 i = 0; i < batch.textureCount; ++i)
            key = HashCombine(key, reinterpret_cast<u64>(list.Textures()[batch.textureBase + i].get()));

        if (auto it = m_BindGroups.find(key); it != m_BindGroups.end()) return it->second;

        // Every slot in the array must be a live descriptor even when the batch uses fewer, so the
        // tail is filled with the 1x1 white texture rather than left dangling.
        std::vector<Ref<gpu::Texture>> textures;
        textures.reserve(DrawList::kMaxTexturesPerBatch);
        for (u32 i = 0; i < batch.textureCount; ++i)
            textures.push_back(list.Textures()[batch.textureBase + i]);
        while (textures.size() < DrawList::kMaxTexturesPerBatch) textures.push_back(m_White);

        std::vector<gpu::BindGroupEntry> entries(2);
        entries[0].binding = 0;
        entries[0].type = gpu::BindingType::StorageBuffer;
        entries[0].buffer = slot.quads;
        entries[1].binding = 1;
        entries[1].type = gpu::BindingType::SampledTexture;
        entries[1].textures = std::move(textures);

        auto group = m_Device->CreateBindGroup(m_QuadPipeline, entries);
        m_BindGroups.emplace(key, group);
        return group;
    }

    Ref<gpu::BindGroup> Renderer::ShadowBindGroup(const Slot& slot) {
        const u64 key = HashCombine(0x5ad0u, reinterpret_cast<u64>(slot.shadows.get()));
        if (auto it = m_BindGroups.find(key); it != m_BindGroups.end()) return it->second;

        std::vector<gpu::BindGroupEntry> entries(1);
        entries[0].binding = 0;
        entries[0].type = gpu::BindingType::StorageBuffer;
        entries[0].buffer = slot.shadows;

        auto group = m_Device->CreateBindGroup(m_ShadowPipeline, entries);
        m_BindGroups.emplace(key, group);
        return group;
    }

    void Renderer::Render(gpu::CommandList& cmd, const DrawList& list, Vec2 viewportSize) {
        if (list.Empty() || !m_QuadPipeline) return;
        Slot& slot = m_Slots[m_SlotIndex];

        const u32 quadBase   = slot.quadCursor;
        const u32 shadowBase = slot.shadowCursor;
        const u32 quadCount   = static_cast<u32>(list.Quads().size());
        const u32 shadowCount = static_cast<u32>(list.Shadows().size());

        m_QuadHighWater   = std::max(m_QuadHighWater,   quadBase + quadCount);
        m_ShadowHighWater = std::max(m_ShadowHighWater, shadowBase + shadowCount);

        // Overflow clamps this frame and grows the next one, rather than reallocating a buffer the
        // GPU may still be reading.
        const u32 quadsToDraw   = std::min(quadCount,   slot.quadCapacity   - quadBase);
        const u32 shadowsToDraw = std::min(shadowCount, slot.shadowCapacity - shadowBase);
        if ((quadsToDraw < quadCount || shadowsToDraw < shadowCount) && !m_WarnedOverflow) {
            VAE_CORE_WARN("draw buffers full ({} quads, {} shadows) — growing next frame",
                          m_QuadHighWater, m_ShadowHighWater);
            m_WarnedOverflow = true;
        }

        if (quadsToDraw)
            slot.quads->Upload(list.Quads().data(), u64(quadsToDraw) * sizeof(QuadInstance),
                               u64(quadBase) * sizeof(QuadInstance));
        if (shadowsToDraw)
            slot.shadows->Upload(list.Shadows().data(), u64(shadowsToDraw) * sizeof(ShadowInstance),
                                 u64(shadowBase) * sizeof(ShadowInstance));

        const PushConstants push{ viewportSize.x, viewportSize.y };

        for (const auto& batch : list.Batches()) {
            if (batch.count == 0) continue;

            if (batch.kind == PrimitiveKind::Quad) {
                if (batch.first >= quadsToDraw) continue;
                const u32 count = std::min(batch.count, quadsToDraw - batch.first);
                cmd.BindPipeline(m_QuadPipeline);
                cmd.BindBindGroup(QuadBindGroup(slot, list, batch));
                cmd.SetPushConstants(&push, sizeof(push));
                cmd.Draw(6, count, 0, quadBase + batch.first);
            } else {
                if (batch.first >= shadowsToDraw) continue;
                const u32 count = std::min(batch.count, shadowsToDraw - batch.first);
                cmd.BindPipeline(m_ShadowPipeline);
                cmd.BindBindGroup(ShadowBindGroup(slot));
                cmd.SetPushConstants(&push, sizeof(push));
                cmd.Draw(6, count, 0, shadowBase + batch.first);
            }
            ++m_DrawCalls;
        }

        slot.quadCursor   = quadBase + quadsToDraw;
        slot.shadowCursor = shadowBase + shadowsToDraw;
    }

}
