#pragma once

#include "vae/base/Base.h"
#include "vae/base/Math.h"

#include <string>
#include <vector>

namespace vae::gpu {

    enum class Backend : u8 { Vulkan, D3D12, Software };

    const char* BackendName(Backend b);
    // Parses VAE_GPU / --gpu=. Unknown names fall back to Vulkan with a warning.
    Backend ParseBackend(std::string_view name);

    enum class Format : u8 {
        Undefined = 0,
        R8_UNORM, RG8_UNORM, RGBA8_UNORM, RGBA8_SRGB, BGRA8_UNORM, BGRA8_SRGB,
        R16_SFLOAT, RG16_SFLOAT, RGBA16_SFLOAT,
        R32_SFLOAT, RG32_SFLOAT, RGB32_SFLOAT, RGBA32_SFLOAT,
        R32_UINT, RG32_UINT, RGBA32_UINT,
        D32_SFLOAT,
    };

    u32 FormatSize(Format f);
    bool IsDepthFormat(Format f);

    enum class BufferUsage : u8 { Vertex, Index, Uniform, Storage, Staging };

    // Where a buffer lives. HostVisible is written every frame by the CPU (instance data, uniforms);
    // DeviceLocal is uploaded once (static geometry, glyph atlas pages).
    enum class MemoryKind : u8 { DeviceLocal, HostVisible };

    enum class TextureUsage : u8 { Sampled, RenderTarget, DepthTarget };

    enum class Filter : u8 { Nearest, Linear };
    enum class AddressMode : u8 { ClampToEdge, Repeat, MirroredRepeat, ClampToBorder };

    enum class ShaderStage : u8 { Vertex, Fragment, Compute };

    enum class PrimitiveTopology : u8 { TriangleList, TriangleStrip, LineList };

    // Straight-alpha "over" is the UI default. Premultiplied is what offscreen layers composite with,
    // since compositing a blurred/faded layer with straight alpha double-darkens the edges.
    enum class BlendMode : u8 { None, AlphaStraight, AlphaPremultiplied, Additive };

    enum class LoadOp : u8 { Load, Clear, DontCare };

    enum class BindingType : u8 { UniformBuffer, StorageBuffer, SampledTexture };

    struct VertexAttribute {
        u32 location = 0;
        Format format = Format::RG32_SFLOAT;
        u32 offset = 0;
    };

    enum class VertexStepMode : u8 { Vertex, Instance };

    struct VertexBufferLayout {
        u32 stride = 0;
        VertexStepMode stepMode = VertexStepMode::Vertex;
        std::vector<VertexAttribute> attributes;
    };

    struct BindingSlot {
        u32 binding = 0;
        BindingType type = BindingType::UniformBuffer;
        u32 count = 1;                 // >1 = array, e.g. the sampler array the batcher indexes
        u8  stages = 0;                // bitmask over ShaderStage
    };

    constexpr u8 StageBit(ShaderStage s) { return static_cast<u8>(1u << static_cast<u8>(s)); }

    struct Viewport {
        f32 x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
        f32 minDepth = 0.0f, maxDepth = 1.0f;
    };

    struct ScissorRect { i32 x = 0, y = 0; u32 width = 0, height = 0; };

}
