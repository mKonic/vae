#include "vaepch.h"
#include "vae/text/FontDB.h"
#include "vae/text/TextCache.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Platform.h"

#include <algorithm>
#include <cctype>

namespace vae::text {

    namespace fs = std::filesystem;

    namespace {
        std::string Lower(std::string_view s) {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        bool IsFontFile(const fs::path& p) {
            const std::string ext = Lower(p.extension().string());
            return ext == ".ttf" || ext == ".otf" || ext == ".ttc";
        }
    }

    std::string FontDB::NormalizeFamily(std::string_view family) {
        std::string out;
        out.reserve(family.size());
        for (char c : family) {
            if (c == ' ' || c == '-' || c == '_') continue;
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    }

    bool FontDB::FamilyKeyLess::operator()(const std::string& a, const std::string& b) const {
        return NormalizeFamily(a) < NormalizeFamily(b);
    }

    FontDB& FontDB::Get() { static FontDB db; return db; }

    FontWeight FontDB::WeightFromName(std::string_view name) {
        const std::string n = Lower(name);
        // Order matters: "extrabold" contains "bold", "semibold" contains "bold", and
        // "extralight" contains "light". Longest and most specific first.
        struct Entry { const char* token; FontWeight weight; };
        static constexpr Entry kEntries[] = {
            { "extrabold", FontWeight::ExtraBold }, { "ultrabold", FontWeight::ExtraBold },
            { "semibold",  FontWeight::SemiBold  }, { "demibold",  FontWeight::SemiBold  },
            { "extralight",FontWeight::ExtraLight}, { "ultralight",FontWeight::ExtraLight},
            { "black",     FontWeight::Black     }, { "heavy",     FontWeight::Black     },
            { "bold",      FontWeight::Bold      },
            { "medium",    FontWeight::Medium    },
            { "light",     FontWeight::Light     },
            { "thin",      FontWeight::Thin      }, { "hairline",  FontWeight::Thin      },
            { "regular",   FontWeight::Regular   }, { "normal",    FontWeight::Regular   },
            { "book",      FontWeight::Regular   },
        };
        for (const auto& entry : kEntries)
            if (n.find(entry.token) != std::string::npos) return entry.weight;
        return FontWeight::Regular;
    }

    FontSlant FontDB::SlantFromName(std::string_view name) {
        const std::string n = Lower(name);
        return (n.find("italic") != std::string::npos || n.find("oblique") != std::string::npos)
             ? FontSlant::Italic : FontSlant::Normal;
    }

    std::string FontDB::FamilyFromFilename(std::string_view stem) {
        // "JetBrainsMonoNerdFont-Regular" -> "JetBrainsMonoNerdFont" -> "JetBrains Mono Nerd Font"
        std::string name(stem);
        if (const auto dash = name.find_last_of("-_"); dash != std::string::npos)
            name = name.substr(0, dash);

        // Strip a style suffix that was glued on without a separator.
        static constexpr const char* kStyles[] = {
            "ExtraBold", "SemiBold", "ExtraLight", "Regular", "Medium", "Light", "Bold",
            "Black", "Thin", "Italic", "Oblique",
        };
        bool stripped = true;
        while (stripped) {
            stripped = false;
            for (const char* style : kStyles) {
                const std::size_t len = std::strlen(style);
                if (name.size() > len && name.compare(name.size() - len, len, style) == 0) {
                    name.resize(name.size() - len);
                    stripped = true;
                }
            }
        }

        // Split CamelCase into words so the family reads the way a user would type it.
        std::string out;
        for (std::size_t i = 0; i < name.size(); ++i) {
            const char c = name[i];
            const bool boundary = i > 0 && std::isupper(static_cast<unsigned char>(c))
                                        && (std::islower(static_cast<unsigned char>(name[i - 1]))
                                            || (i + 1 < name.size()
                                                && std::islower(static_cast<unsigned char>(name[i + 1]))));
            if (boundary && !out.empty() && out.back() != ' ') out.push_back(' ');
            out.push_back(c);
        }

        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out.empty() ? std::string(stem) : out;
    }

    void FontDB::RegisterFile(const fs::path& path, std::string family,
                              FontWeight weight, FontSlant slant) {
        // A newly registered face can be a better answer than whatever a family resolved to before.
        m_Styles.clear();
        Ref<Font> preloaded;
        if (family.empty()) {
            preloaded = Font::LoadFromFile(path);
            if (!preloaded) return;
            family = preloaded->FamilyName();
            if (!preloaded->StyleName().empty()) {
                weight = WeightFromName(preloaded->StyleName());
                slant  = SlantFromName(preloaded->StyleName());
            }
        }

        auto& faces = m_Families[family];
        for (const auto& face : faces)
            if (face.info.path == path) return;

        Face face;
        face.info.family = std::move(family);
        face.info.weight = weight;
        face.info.slant  = slant;
        face.info.path   = path;
        face.font        = preloaded;
        face.info.loaded = preloaded != nullptr;
        faces.push_back(std::move(face));
    }

    u32 FontDB::RegisterDirectory(const fs::path& dir, bool recursive, bool readMetadata) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return 0;

        u32 count = 0;
        auto Consider = [&](const fs::path& path) {
            if (!IsFontFile(path)) return;
            const std::string stem = path.stem().string();
            if (readMetadata) RegisterFile(path);
            else RegisterFile(path, FamilyFromFilename(stem), WeightFromName(stem), SlantFromName(stem));
            ++count;
        };

        if (recursive) {
            for (auto it = fs::recursive_directory_iterator(
                     dir, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
                Consider(it->path());
        } else {
            for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator(); it.increment(ec))
                Consider(it->path());
        }
        return count;
    }

    u32 FontDB::ScanSystemFonts() {
        u32 count = 0;
        for (const fs::path& dir : platform::FontDirectories())
            if (!dir.empty()) count += RegisterDirectory(dir);
        return count;
    }

    void FontDB::LoadDefaults() {
        // Engine-bundled faces first, so a project always has a working default even on a machine
        // with no fonts installed at all.
        const u32 bundled = RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);
        const u32 system  = ScanSystemFonts();

        if (!HasFamily(m_DefaultFamily)) {
            // Prefer the bundled family; fall back to whatever the first registered family is
            // rather than leaving the default pointing at nothing.
            for (const char* candidate : { "JetBrains Mono Nerd Font", "JetBrains Mono",
                                           "DejaVu Sans", "Noto Sans" }) {
                if (HasFamily(candidate)) { m_DefaultFamily = candidate; break; }
            }
            if (!HasFamily(m_DefaultFamily) && !m_Families.empty())
                m_DefaultFamily = m_Families.begin()->first;
        }

        if (m_FallbackFamilies.empty()) {
            for (const char* candidate : { "JetBrains Mono Nerd Font", "Noto Sans", "DejaVu Sans",
                                           "Noto Sans CJK", "Noto Color Emoji" })
                if (HasFamily(candidate) && candidate != m_DefaultFamily)
                    m_FallbackFamilies.emplace_back(candidate);
        }

        VAE_CORE_INFO("fonts: {} bundled + {} system faces across {} families; default '{}'",
                      bundled, system, m_Families.size(), m_DefaultFamily);
    }

    FontDB::Face* FontDB::FindBest(std::string_view family, FontWeight weight, FontSlant slant) {
        auto it = m_Families.find(std::string(family));
        if (it == m_Families.end() || it->second.empty()) return nullptr;

        Face* best = nullptr;
        int bestScore = std::numeric_limits<int>::max();
        for (auto& face : it->second) {
            if (face.failed) continue;
            // Slant is a hard preference (a designer asking for italic and getting upright is
            // wrong in a way a 100-unit weight miss is not), weight a soft one.
            const int slantPenalty = (face.info.slant == slant) ? 0 : 10000;
            const int delta = std::abs(static_cast<int>(face.info.weight) - static_cast<int>(weight));
            const int score = slantPenalty + delta;
            if (score < bestScore) { bestScore = score; best = &face; }
        }
        return best;
    }

    Ref<Font> FontDB::Load(Face& face) {
        if (face.font) return face.font;
        if (face.failed) return nullptr;

        face.font = Font::LoadFromFile(face.info.path);
        face.info.loaded = face.font != nullptr;
        if (!face.font) {
            // Font::Init already said which kind of "cannot read" this was — an unsupported colour
            // format or a broken file — so this is the where, at the level that matches.
            face.failed = true;
            VAE_CORE_INFO("font face '{}' skipped: {}",
                          face.info.family, face.info.path.string());
        }
        return face.font;
    }

    Ref<Font> FontDB::Resolve(std::string_view family, FontWeight weight, FontSlant slant) {
        return Resolve(FontRequest{ std::string(family), weight, slant, 14.0f });
    }

    Ref<Font> FontDB::Resolve(const FontRequest& request) {
        const std::string family = request.family.empty() ? m_DefaultFamily : request.family;

        if (Face* face = FindBest(family, request.weight, request.slant))
            if (auto font = Load(*face)) return font;

        // If the request *was* the default and it did not resolve, there is nothing left to try.
        if (NormalizeFamily(family) == NormalizeFamily(m_DefaultFamily)) return nullptr;

        if (!request.family.empty()) {
            if (std::find(m_Warned.begin(), m_Warned.end(), request.family) == m_Warned.end()) {
                VAE_CORE_WARN("font family '{}' is not installed — using '{}'",
                              request.family, m_DefaultFamily);
                m_Warned.push_back(request.family);
            }
            if (Face* face = FindBest(m_DefaultFamily, request.weight, request.slant))
                return Load(*face);
        }
        return nullptr;
    }

    // Memoized, because this is called for every text node on every frame — twice, once to measure
    // and once to paint — and the answer only changes when the font database does. Unmemoized it
    // was a family lookup, a face search and a fresh `vector<Ref<Font>>` per call, which measured
    // as most of the per-label cost of a frame and had nothing to do with the text.
    const TextStyle& FontDB::Style(const FontRequest& request) {
        const std::string& family = request.family.empty() ? m_DefaultFamily : request.family;
        const StyleKey key{ family, request.weight, request.slant };

        if (const auto it = m_Styles.find(key); it != m_Styles.end()) {
            // Size is the one field that does not change what was resolved, so it rides on top of
            // the cached chain rather than splitting the cache by every size in the document.
            it->second.size = request.size;
            return it->second;
        }

        TextStyle style;
        style.font = Resolve(request);
        style.size = request.size;
        for (const auto& fallback : m_FallbackFamilies) {
            if (Face* face = FindBest(fallback, request.weight, request.slant))
                if (auto font = Load(*face); font && font != style.font)
                    style.fallbacks.push_back(font);
        }
        return m_Styles.emplace(key, std::move(style)).first->second;
    }

    void FontDB::SetDefaultFamily(std::string family) {
        m_DefaultFamily = std::move(family);
        m_Styles.clear();      // every resolved chain was against the old default
    }

    void FontDB::SetFallbackFamilies(std::vector<std::string> families) {
        m_FallbackFamilies = std::move(families);
        m_Styles.clear();
    }

    std::vector<std::string> FontDB::Families() const {
        std::vector<std::string> out;
        out.reserve(m_Families.size());
        for (const auto& [family, faces] : m_Families) out.push_back(family);
        return out;
    }

    std::vector<FontFaceInfo> FontDB::Faces(std::string_view family) const {
        std::vector<FontFaceInfo> out;
        auto it = m_Families.find(std::string(family));
        if (it == m_Families.end()) return out;
        for (const auto& face : it->second) out.push_back(face.info);
        return out;
    }

    bool FontDB::HasFamily(std::string_view family) const {
        return m_Families.find(std::string(family)) != m_Families.end();
    }

    void FontDB::Clear() {
        // Every shaped run in the cache points at a face that is about to stop existing.
        TextCache::Clear();
        m_Styles.clear();
        m_Families.clear();
        m_Warned.clear();
        m_FallbackFamilies.clear();
    }

}
