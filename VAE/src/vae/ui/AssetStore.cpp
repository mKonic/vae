#include "vaepch.h"
#include "vae/ui/AssetStore.h"

#include "vae/base/FileSystem.h"
#include "vae/doc/Document.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>

namespace vae::ui {

    namespace {
        // What the audio engine can read. miniaudio decodes wav, flac and mp3 itself and ogg
        // through the stb_vorbis it is built with; anything else is a file it will refuse, and
        // refusing it here means the Assets panel says so instead of the first button press.
        bool IsSoundExtension(std::string_view extension) {
            return extension == ".wav" || extension == ".ogg"
                || extension == ".mp3" || extension == ".flac";
        }
    }

    void AssetStore::SetDevice(gpu::Device* device) {
        if (m_Device == device) return;
        m_Device = device;
        // Textures belong to the device that made them, so a new one starts over.
        for (auto& [id, entry] : m_Entries) {
            entry.texture.reset();
            entry.rasters.clear();
            entry.problem.clear();
        }
    }

    void AssetStore::SetRoot(std::filesystem::path root) {
        if (m_Root == root) return;
        m_Root = std::move(root);
        for (auto& [id, entry] : m_Entries) {
            entry.texture.reset();
            entry.rasters.clear();
            entry.problem.clear();
        }
    }

    void AssetStore::Rebind(const doc::Document& document) {
        std::map<Uuid, Entry> next;
        for (const doc::Document::Asset& asset : document.Assets()) {
            auto existing = m_Entries.find(asset.id);
            // Same id, same path: keep the texture. Reloading every frame would decode a megabyte
            // of PNG per image per frame, which is a slideshow with extra steps.
            if (existing != m_Entries.end() && existing->second.path == asset.path)
                next.emplace(asset.id, std::move(existing->second));
            else {
                Entry fresh;
                fresh.path = asset.path;
                // The extension is the only thing that says which kind of file this is, and it has
                // to be known before anything is read: the two kinds are not loaded the same way.
                const std::string extension = std::filesystem::path(asset.path).extension().string();
                fresh.vector = extension == ".svg";
                fresh.sound = IsSoundExtension(extension);
                next.emplace(asset.id, std::move(fresh));
            }
        }
        m_Entries = std::move(next);
    }

    void AssetStore::Load(Entry& entry) const {
        if (entry.texture || !entry.picture.shapes.empty() || !entry.problem.empty()) return;
        // A sound is not decoded here and never will be: the audio engine reads it from disk and
        // caches it, and a second copy in a texture store would be a second copy for nothing. All
        // this has to know is whether the file is there.
        if (entry.sound) {
            if (entry.path.empty()) { entry.problem = "no file"; return; }
            std::error_code ec;
            const std::filesystem::path file = m_Root.empty() ? std::filesystem::path(entry.path)
                                                              : m_Root / entry.path;
            if (!std::filesystem::exists(file, ec))
                entry.problem = "cannot read " + file.string();
            return;
        }
        // Artwork is read without a device: parsing is arithmetic, and only the pixels need a GPU.
        // That is what lets a headless run know how big an icon is and whether it wants a colour.
        if (!m_Device && !entry.vector) { entry.problem = "no device yet"; return; }
        if (entry.path.empty()) { entry.problem = "no file"; return; }

        const std::filesystem::path file = m_Root.empty() ? std::filesystem::path(entry.path)
                                                          : m_Root / entry.path;
        // Read then decode, rather than stbi_load: the stb build here is compiled STBI_NO_STDIO,
        // and going through FileSystem keeps every read in the engine on one path anyway.
        const auto bytes = FileSystem::ReadBinary(file);
        if (!bytes) {
            entry.problem = std::string("cannot read ") + file.string();
            VAE_CORE_WARN("asset: {}", entry.problem);
            return;
        }

        // Artwork stops here: what is kept is the picture, not pixels, because the pixels depend
        // on how big it is drawn and that is not known until something asks.
        if (entry.vector) {
            std::string trouble;
            if (!vector::ParseSvg(std::string_view(reinterpret_cast<const char*>(bytes->data()),
                                                   bytes->size()),
                                  entry.picture, &trouble)) {
                entry.problem = trouble.empty() ? "not artwork VAE can read" : trouble;
                VAE_CORE_WARN("asset: {} ({})", entry.problem, entry.path);
                return;
            }
            entry.size = entry.picture.size;
            if (!trouble.empty()) VAE_CORE_WARN("asset: {} ({})", trouble, entry.path);
            return;
        }

        int width = 0, height = 0, channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()),
                                                &width, &height, &channels, 4);
        if (!pixels) {
            entry.problem = std::string("not an image VAE can read: ") + file.string();
            VAE_CORE_WARN("asset: {}", entry.problem);
            return;
        }

        gpu::TextureDesc desc;
        desc.width = static_cast<u32>(width);
        desc.height = static_cast<u32>(height);
        desc.format = gpu::Format::RGBA8_UNORM;
        desc.usage = gpu::TextureUsage::Sampled;
        desc.debugName = entry.path;

        entry.texture = m_Device->CreateTexture(desc);
        if (entry.texture)
            entry.texture->Upload(pixels, static_cast<u64>(width) * height * 4);
        else
            entry.problem = "the device refused the texture";
        entry.size = { static_cast<f32>(width), static_cast<f32>(height) };
        stbi_image_free(pixels);
    }

    bool AssetStore::IsSound(Uuid asset) const {
        const Entry* entry = Find(asset);
        return entry && entry->sound;
    }

    std::filesystem::path AssetStore::FileOf(Uuid asset) const {
        const auto it = m_Entries.find(asset);
        if (it == m_Entries.end() || it->second.path.empty()) return {};
        return m_Root.empty() ? std::filesystem::path(it->second.path)
                              : m_Root / it->second.path;
    }

    AssetStore::Entry* AssetStore::Find(Uuid asset) const {
        auto it = m_Entries.find(asset);
        if (it == m_Entries.end()) return nullptr;
        Load(it->second);
        return &it->second;
    }

    Ref<gpu::Texture> AssetStore::Image(Uuid asset) const {
        const Entry* entry = Find(asset);
        // An icon asked for as a picture is still drawn: at the size it says it is, in the colours
        // it came with. That is what makes the Assets panel's thumbnails work for both kinds.
        if (entry && entry->vector)
            return Vector(asset, entry->size.x > 0.0f ? entry->size : Vec2{ 64.0f, 64.0f }, nullptr);
        return entry ? entry->texture : Ref<gpu::Texture>{};
    }

    Ref<gpu::Texture> AssetStore::Vector(Uuid asset, Vec2 pixels, const Color* tint) const {
        Entry* entry = Find(asset);
        if (!entry || !entry->vector || entry->picture.Empty() || !m_Device) return {};

        const u32 width = static_cast<u32>(std::clamp(std::round(pixels.x), 1.0f, 4096.0f));
        const u32 height = static_cast<u32>(std::clamp(std::round(pixels.y), 1.0f, 4096.0f));
        const Color ink = tint ? *tint : Color{ 0.0f, 0.0f, 0.0f, 0.0f };

        // Keyed on the size and the colour, because those are the two things that change what the
        // pixels are. A resize drag makes a raster per frame, which a 200-pixel icon can afford;
        // what it cannot afford is doing it again every frame at a size it already has.
        u64 key = (static_cast<u64>(width) << 48) ^ (static_cast<u64>(height) << 32);
        if (tint) {
            const auto quantize = [](f32 v) {
                return static_cast<u64>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            key ^= 1ull | (quantize(ink.r) << 24) | (quantize(ink.g) << 16)
                        | (quantize(ink.b) << 8) | quantize(ink.a);
        }
        if (const auto it = entry->rasters.find(key); it != entry->rasters.end())
            return it->second.texture;

        // A resize drag walks through every intermediate size; without a ceiling the cache is a
        // slow leak of textures nobody will ask for again.
        if (entry->rasters.size() > 24) entry->rasters.clear();

        const vector::Bitmap bitmap = vector::Render(entry->picture, width, height,
                                                     tint ? &ink : nullptr);
        if (bitmap.Empty()) return {};

        gpu::TextureDesc desc;
        desc.width = bitmap.width;
        desc.height = bitmap.height;
        desc.format = gpu::Format::RGBA8_UNORM;
        desc.usage = gpu::TextureUsage::Sampled;
        desc.debugName = entry->path;

        Entry::Drawn drawn;
        drawn.texture = m_Device->CreateTexture(desc);
        drawn.size = { static_cast<f32>(width), static_cast<f32>(height) };
        drawn.tint = ink;
        if (!drawn.texture) {
            entry->problem = "the device refused the texture";
            return {};
        }
        drawn.texture->Upload(bitmap.pixels.data(), bitmap.pixels.size());
        return entry->rasters.emplace(key, std::move(drawn)).first->second.texture;
    }

    bool AssetStore::IsVector(Uuid asset) const {
        const auto it = m_Entries.find(asset);
        return it != m_Entries.end() && it->second.vector;
    }

    bool AssetStore::FollowsText(Uuid asset) const {
        const Entry* entry = Find(asset);
        return entry && entry->vector && entry->picture.FollowsText();
    }

    Vec2 AssetStore::SizeOf(Uuid asset) const {
        const Entry* entry = Find(asset);
        return entry ? entry->size : Vec2{ 0.0f, 0.0f };
    }

    std::string AssetStore::ProblemWith(Uuid asset) const {
        const Entry* entry = Find(asset);
        return entry ? entry->problem : std::string("unknown asset");
    }

    void AssetStore::Clear() { m_Entries.clear(); }

}
