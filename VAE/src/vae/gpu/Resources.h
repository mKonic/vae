#pragma once

#include "vae/gpu/Types.h"

namespace vae::gpu {

    // Resource interfaces are virtual at *resource* granularity, never per draw. A whole UI frame
    // is a handful of these objects and two or three draws, so the dispatch cost is noise; putting
    // the seam any lower would make it the hot path (design/architecture.md D5).

    struct BufferDesc {
        u64 size = 0;
        BufferUsage usage = BufferUsage::Vertex;
        MemoryKind memory = MemoryKind::HostVisible;
        std::string debugName;
    };

    class Buffer {
    public:
        virtual ~Buffer() = default;
        virtual void Upload(const void* data, u64 size, u64 offset = 0) = 0;
        virtual void* Map() = 0;                  // HostVisible only; persistently mapped
        virtual u64  Size() const = 0;
        virtual const BufferDesc& Desc() const = 0;
    };

    struct TextureDesc {
        u32 width = 1, height = 1;
        Format format = Format::RGBA8_UNORM;
        TextureUsage usage = TextureUsage::Sampled;
        Filter minFilter = Filter::Linear, magFilter = Filter::Linear;
        AddressMode addressMode = AddressMode::ClampToEdge;
        std::string debugName;
    };

    class Texture {
    public:
        virtual ~Texture() = default;
        virtual void Upload(const void* pixels, u64 size) = 0;
        virtual void UploadRegion(const void* pixels, u32 x, u32 y, u32 w, u32 h) = 0;
        virtual u32 Width()  const = 0;
        virtual u32 Height() const = 0;
        virtual const TextureDesc& Desc() const = 0;
    };

    struct ShaderDesc {
        std::vector<u32> spirv;        // SPIR-V words; glslc output, see scripts/CompileShaders.sh
        ShaderStage stage = ShaderStage::Vertex;
        std::string entryPoint = "main";
        std::string debugName;
    };

    class Shader {
    public:
        virtual ~Shader() = default;
        virtual ShaderStage Stage() const = 0;
    };

    struct PipelineDesc {
        Ref<Shader> vertex;
        Ref<Shader> fragment;
        std::vector<VertexBufferLayout> vertexBuffers;
        std::vector<BindingSlot> bindings;
        u32 pushConstantSize = 0;
        PrimitiveTopology topology = PrimitiveTopology::TriangleList;
        BlendMode blend = BlendMode::AlphaStraight;
        bool depthTest = false;
        bool depthWrite = false;
        Format colorFormat = Format::BGRA8_UNORM;   // must match the target being rendered into
        Format depthFormat = Format::Undefined;
        std::string debugName;
    };

    class Pipeline {
    public:
        virtual ~Pipeline() = default;
        virtual const PipelineDesc& Desc() const = 0;
    };

    // A concrete set of resources bound to a pipeline's binding slots.
    struct BindGroupEntry {
        u32 binding = 0;
        BindingType type = BindingType::UniformBuffer;
        Ref<Buffer> buffer;
        u64 bufferOffset = 0, bufferRange = 0;     // range 0 = whole buffer
        std::vector<Ref<Texture>> textures;        // >1 for an array binding
    };

    class BindGroup {
    public:
        virtual ~BindGroup() = default;
    };

    // An offscreen render target the UI can draw into and later sample (layer effects, the Studio
    // canvas, the glyph atlas debug view).
    class RenderTarget {
    public:
        virtual ~RenderTarget() = default;
        virtual u32 Width()  const = 0;
        virtual u32 Height() const = 0;
        virtual Ref<Texture> ColorTexture() const = 0;
        virtual void Resize(u32 width, u32 height) = 0;
    };

    struct RenderTargetDesc {
        u32 width = 1, height = 1;
        Format colorFormat = Format::RGBA8_UNORM;
        Format depthFormat = Format::Undefined;
        std::string debugName;
    };

}
