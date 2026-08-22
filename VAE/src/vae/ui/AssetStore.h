#pragma once

#include "vae/gpu/Device.h"
#include "vae/ui/ViewTree.h"
#include "vae/vector/Svg.h"

#include <filesystem>
#include <map>

namespace vae::ui {

    // The other half of `AssetTable`: files on disk turned into textures, once each. The document
    // holds ids and relative paths; this holds the pixels, and it is deliberately not part of the
    // document — a texture belongs to a device, and the same project opens on machines with
    // different ones.
    class AssetStore final : public AssetTable {
    public:
        void SetDevice(gpu::Device* device);
        // Where relative asset paths resolve from: the project's folder.
        void SetRoot(std::filesystem::path root);
        const std::filesystem::path& Root() const { return m_Root; }

        // Binds the ids in a document to their files. Safe to call every time the document
        // changes; only paths that actually moved are reloaded.
        void Rebind(const doc::Document& document);

        Ref<gpu::Texture> Image(Uuid asset) const override;
        Ref<gpu::Texture> Vector(Uuid asset, Vec2 pixels, const Color* tint) const override;
        // Whether the file behind an asset is artwork rather than a picture. What a drop on the
        // canvas becomes depends on it: a vector is redrawn at whatever size it ends up, and a
        // photograph is not.
        bool IsVector(Uuid asset) const;
        // Whether the file behind an asset is a sound. Nothing about it is drawn, which is exactly
        // why it has to be asked: everything else here assumes a picture.
        bool IsSound(Uuid asset) const;
        // Where the file actually is, resolved against the project's folder. Empty when there is
        // no such asset. Sound needs this because it is played from disk rather than decoded here
        // — a texture belongs to a device, and a waveform belongs to the audio engine.
        std::filesystem::path FileOf(Uuid asset) const;
        // Whether the artwork asked to be told what colour to be. What the editor gives a freshly
        // placed icon depends on it.
        bool FollowsText(Uuid asset) const;
        // Pixel size, or {0,0} when the asset is missing or could not be decoded. The Studio shows
        // it, and a layout that wants an image's aspect needs it.
        Vec2 SizeOf(Uuid asset) const;
        // Why an asset is not showing, or empty. A silent missing image is a bug report with no
        // information in it.
        std::string ProblemWith(Uuid asset) const;

        void Clear();

    private:
        struct Entry {
            std::string path;             // as written in the document
            Ref<gpu::Texture> texture;
            Vec2 size{ 0.0f, 0.0f };
            std::string problem;

            // Artwork is not decoded once and kept: it is drawn at the size it is asked for, and
            // drawn again when that changes. The picture is parsed once, the pixels are not.
            bool vector = false;
            bool sound = false;
            vector::Picture picture;
            struct Drawn { Ref<gpu::Texture> texture; Vec2 size{ 0.0f, 0.0f }; Color tint{}; };
            std::map<u64, Drawn> rasters;
        };

        void Load(Entry& entry) const;
        Entry* Find(Uuid asset) const;

        gpu::Device* m_Device = nullptr;
        std::filesystem::path m_Root;
        mutable std::map<Uuid, Entry> m_Entries;
    };

}
