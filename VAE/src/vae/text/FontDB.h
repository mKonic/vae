#pragma once

#include "vae/text/TextLayout.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vae::text {

    // CSS numeric weights. Designers pick "Semibold", the document stores 600, and resolution
    // finds the nearest face that actually exists.
    enum class FontWeight : u16 {
        Thin = 100, ExtraLight = 200, Light = 300, Regular = 400, Medium = 500,
        SemiBold = 600, Bold = 700, ExtraBold = 800, Black = 900
    };

    enum class FontSlant : u8 { Normal, Italic };

    struct FontRequest {
        std::string family;                      // empty = the default UI family
        FontWeight  weight = FontWeight::Regular;
        FontSlant   slant  = FontSlant::Normal;
        f32         size   = 14.0f;
    };

    struct FontFaceInfo {
        std::string family;
        FontWeight  weight = FontWeight::Regular;
        FontSlant   slant  = FontSlant::Normal;
        std::filesystem::path path;
        bool loaded = false;
    };

    // The font system. Faces are registered by family/weight/slant and loaded lazily on first use,
    // so scanning a system font directory with hundreds of files costs a directory walk and not
    // hundreds of megabytes.
    //
    // Resolution follows the CSS font-matching idea in the part that matters for a design tool:
    // an exact (family, weight, slant) wins; otherwise the nearest available weight in the same
    // family wins, preferring the same direction CSS does; otherwise the fallback chain; otherwise
    // the default family. A request never fails silently — an unresolvable family logs once and
    // resolves to the default, because a designer typing a font name that is not installed should
    // see readable text plus a warning, not nothing.
    class FontDB {
    public:
        static FontDB& Get();

        // Registration. RegisterFile trusts what it is told; RegisterDirectory infers family,
        // weight and slant from the filename, which is what font files on disk actually encode.
        // An empty family means "read it from the file", which loads the face immediately.
        void RegisterFile(const std::filesystem::path& path, std::string family = {},
                          FontWeight weight = FontWeight::Regular,
                          FontSlant slant = FontSlant::Normal);
        // readMetadata opens each file to read its real family/style names — exact, but it loads
        // every file. Off for system scans (hundreds of megabytes); on for bundled and project
        // fonts, where being exact matters and the file count is small.
        u32  RegisterDirectory(const std::filesystem::path& dir, bool recursive = true,
                               bool readMetadata = false);

        // Loads the engine's own bundled faces plus the platform's font directories.
        void LoadDefaults();
        u32  ScanSystemFonts();

        Ref<Font> Resolve(const FontRequest& request);
        Ref<Font> Resolve(std::string_view family, FontWeight weight = FontWeight::Regular,
                          FontSlant slant = FontSlant::Normal);

        // A ready-to-use style with the fallback chain already attached. Memoized per (family,
        // weight, slant) — a UI asks for the same handful of styles thousands of times a frame,
        // and building the chain fresh each time was most of what a text node cost. The reference
        // is invalidated by any change to the database (registering a face, changing the default
        // or the fallbacks, clearing), so it is read and used, not held.
        const TextStyle& Style(const FontRequest& request);

        void SetDefaultFamily(std::string family);
        const std::string& DefaultFamily() const { return m_DefaultFamily; }

        // Consulted in order for codepoints the primary face lacks.
        void SetFallbackFamilies(std::vector<std::string> families);
        const std::vector<std::string>& FallbackFamilies() const { return m_FallbackFamilies; }

        // The last resort, when neither the primary face nor the fallback chain has a codepoint:
        // any registered family that does. Null when nothing installed on this machine covers it,
        // which is the only honest reason to draw a box.
        //
        // A named fallback list is a list of the scripts somebody thought of. This is not — it can
        // answer for Devanagari, Thai, Cherokee or a script that did not exist when this was
        // written, as long as a face for it is installed. The search reads font files, so its
        // answer is remembered per codepoint and the scan happens once.
        const Ref<Font>& FaceCovering(u32 codepoint, FontWeight weight, FontSlant slant);

        std::vector<std::string> Families() const;
        std::vector<FontFaceInfo> Faces(std::string_view family) const;
        bool HasFamily(std::string_view family) const;

        void Clear();

        // Exposed because filename parsing is guesswork worth testing directly.
        static FontWeight WeightFromName(std::string_view name);
        static FontSlant  SlantFromName(std::string_view name);
        static std::string FamilyFromFilename(std::string_view stem);
        // Family keys compare normalized; exposed so tests assert against the real comparison.
        static std::string NormalizeFamilyForTest(std::string_view family) { return NormalizeFamily(family); }

    private:
        struct StyleKey {
            std::string family;
            FontWeight  weight = FontWeight::Regular;
            FontSlant   slant  = FontSlant::Normal;
            bool operator==(const StyleKey&) const = default;
        };
        struct StyleKeyHash {
            std::size_t operator()(const StyleKey& key) const {
                return std::hash<std::string>{}(key.family)
                     ^ (static_cast<std::size_t>(key.weight) << 1)
                     ^ (static_cast<std::size_t>(key.slant) << 17);
            }
        };

        struct CoverageKey {
            u32        codepoint = 0;
            FontWeight weight = FontWeight::Regular;
            FontSlant  slant  = FontSlant::Normal;
            bool operator==(const CoverageKey&) const = default;
        };
        struct CoverageKeyHash {
            std::size_t operator()(const CoverageKey& key) const {
                return key.codepoint ^ (static_cast<std::size_t>(key.weight) << 21)
                                     ^ (static_cast<std::size_t>(key.slant) << 31);
            }
        };

        struct Face {
            FontFaceInfo info;
            Ref<Font> font;                 // null until first resolved
            // Sticky: a face stb_truetype cannot read (colour-bitmap emoji fonts use CBDT/CBLC,
            // which it does not support) must be skipped, not retried on every single resolve.
            bool failed = false;
        };

        Face* FindBest(std::string_view family, FontWeight weight, FontSlant slant);
        Ref<Font> Load(Face& face);

        // Family names compare case-insensitively AND ignoring spaces, hyphens and underscores,
        // so "JetBrains Mono", "jetbrainsmono" and a filename-derived "Jet Brains Mono" are all
        // the same family. That makes the lookup robust to how a name was written or guessed.
        struct FamilyKeyLess {
            bool operator()(const std::string& a, const std::string& b) const;
        };
        static std::string NormalizeFamily(std::string_view family);

        std::map<std::string, std::vector<Face>, FamilyKeyLess> m_Families;
        // Resolved styles, by what was asked for. Cleared whenever the database changes.
        std::unordered_map<StyleKey, TextStyle, StyleKeyHash> m_Styles;
        std::string m_DefaultFamily = "JetBrains Mono";
        std::vector<std::string> m_FallbackFamilies;
        std::vector<std::string> m_Warned;
        // Answers to "which installed face has this character", including the negative ones.
        std::unordered_map<CoverageKey, Ref<Font>, CoverageKeyHash> m_Coverage;
    };

}
