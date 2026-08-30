#include "Panels.h"

#include "../ScriptSession.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"

#include <imgui.h>
#include <imgui_internal.h>   // ImTextCharFromUtf8, for centring a glyph on its ink

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <system_error>
#include <vector>

namespace vae {

    namespace {

        namespace fs = std::filesystem;

        // Nerd Font glyphs; the font is loaded with the private-use range, the same as the Layers
        // panel's icons.
        constexpr const char* kIconFolder = "";
        constexpr const char* kIconUp     = "";
        constexpr const char* kIconScreen = "";
        constexpr const char* kIconCode   = "";
        constexpr const char* kIconImage  = "";
        constexpr const char* kIconFont   = "";
        constexpr const char* kIconSound  = "";
        constexpr const char* kIconFile   = "";

        // What a project is made of, on disk. Folders are content too — a project with an assets
        // folder is a project you have to be able to walk into.
        enum class Kind { Folder, Screen, Script, Image, Sound, Font, Other };

        struct Entry {
            fs::path path;
            std::string name;
            Kind kind = Kind::Other;
            std::uintmax_t size = 0;
        };

        // Two lines of name under each tile, which is what it takes to tell "screens-example" from
        // "screens-example.store" without hovering either of them.
        constexpr int kLabelLines = 2;

        struct Memory {
            fs::path directory;
            fs::path root;
            char filter[96] = "";
            f32 tile = 76.0f;
            fs::path renaming;
            char rename[128] = "";
            fs::path deleting;
        };
        Memory g_Files;

        Kind KindOf(const fs::path& path, bool directory) {
            if (directory) return Kind::Folder;
            std::string ext = path.extension().string();
            std::ranges::transform(ext, ext.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".vae") return Kind::Screen;
            if (ext == ".lua" || ext == ".cpp" || ext == ".h" || ext == ".hpp") return Kind::Script;
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".svg") return Kind::Image;
            if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") return Kind::Sound;
            if (ext == ".ttf" || ext == ".otf") return Kind::Font;
            return Kind::Other;
        }

        const char* IconFor(Kind kind) {
            switch (kind) {
                case Kind::Folder: return kIconFolder;
                case Kind::Screen: return kIconScreen;
                case Kind::Script: return kIconCode;
                case Kind::Image:  return kIconImage;
                case Kind::Sound:  return kIconSound;
                case Kind::Font:   return kIconFont;
                default:           return kIconFile;
            }
        }

        ImVec4 TintFor(Kind kind) {
            switch (kind) {
                case Kind::Folder: return ImVec4(0.62f, 0.72f, 0.95f, 1.0f);
                case Kind::Screen: return ImVec4(0.72f, 0.62f, 0.95f, 1.0f);
                case Kind::Script: return ImVec4(0.55f, 0.82f, 0.58f, 1.0f);
                case Kind::Image:  return ImVec4(0.45f, 0.74f, 0.95f, 1.0f);
                case Kind::Sound:  return ImVec4(0.95f, 0.62f, 0.72f, 1.0f);
                case Kind::Font:   return ImVec4(0.94f, 0.78f, 0.50f, 1.0f);
                default:           return ImVec4(0.62f, 0.65f, 0.72f, 1.0f);
            }
        }

        std::string SizeText(std::uintmax_t bytes) {
            char buffer[32];
            if (bytes < 1024)         std::snprintf(buffer, sizeof buffer, "%llu B",
                                                    static_cast<unsigned long long>(bytes));
            else if (bytes < 1048576) std::snprintf(buffer, sizeof buffer, "%.1f KB",
                                                    static_cast<double>(bytes) / 1024.0);
            else                      std::snprintf(buffer, sizeof buffer, "%.1f MB",
                                                    static_cast<double>(bytes) / 1048576.0);
            return buffer;
        }

        bool Matches(const std::string& name, const std::string& needle) {
            if (needle.empty()) return true;
            const auto it = std::search(name.begin(), name.end(), needle.begin(), needle.end(),
                                        [](char a, char b) {
                                            return std::tolower(static_cast<unsigned char>(a))
                                                == std::tolower(static_cast<unsigned char>(b));
                                        });
            return it != name.end();
        }

        // A glyph drawn to fill a box: scaled so its *ink* fits, and centred on that ink rather
        // than on its advance box. Both matter and for the same reason — a glyph's ink is a
        // fraction of its em box, and not a symmetric one. Centring the advance leaves the icon
        // visibly off the name under it; sizing by the em box leaves a small icon marooned in a
        // large square with a gap above the name. A fixed multiplier fixes the folder and overflows
        // the wide glyphs, so the scale is measured per glyph instead.
        void DrawGlyph(ImDrawList* draw, const char* glyph, ImVec2 min, ImVec2 max, ImU32 colour,
                       f32 fill = 0.96f) {
            ImFont* font = ImGui::GetFont();
            const f32 cx = (min.x + max.x) * 0.5f;
            const f32 cy = (min.y + max.y) * 0.5f;
            const f32 box = std::min(max.x - min.x, max.y - min.y);

            unsigned int codepoint = 0;
            ImTextCharFromUtf8(&codepoint, glyph, nullptr);

            const auto Ink = [&](f32 size) -> const ImFontGlyph* {
                ImFontBaked* baked = font->GetFontBaked(size);
                return baked ? baked->FindGlyph(static_cast<ImWchar>(codepoint)) : nullptr;
            };

            const ImFontGlyph* probe = Ink(box);
            if (!probe) {
                // No such glyph in this face: fall back to the advance box rather than nothing.
                const ImVec2 extent = font->CalcTextSizeA(box, FLT_MAX, 0.0f, glyph);
                draw->AddText(font, box, ImVec2(cx - extent.x * 0.5f, cy - extent.y * 0.5f),
                              colour, glyph);
                return;
            }

            const f32 inkW = probe->X1 - probe->X0;
            const f32 inkH = probe->Y1 - probe->Y0;
            const f32 largest = std::max(inkW, inkH);
            f32 size = box;
            if (largest > 0.1f)
                size = std::clamp(box * (box * fill) / largest, box * 0.5f, box * 3.0f);

            const ImFontGlyph* ink = Ink(size);
            if (!ink) ink = probe;
            // AddText puts the ink at pen + (X0,Y0)..(X1,Y1); solve for the pen that lands the
            // ink's own centre on the box's centre.
            draw->AddText(font, size,
                          ImVec2(cx - (ink->X0 + ink->X1) * 0.5f, cy - (ink->Y0 + ink->Y1) * 0.5f),
                          colour, glyph);
        }

        std::string Ellipsize(const std::string& text, f32 maxWidth) {
            if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text;
            std::string out = text;
            while (out.size() > 1 && ImGui::CalcTextSize((out + "\xE2\x80\xA6").c_str()).x > maxWidth)
                out.pop_back();
            return out + "\xE2\x80\xA6";
        }

        // Word-wrapped into at most `maxLines` lines. A file grid that truncates every name after
        // twelve characters is a grid of files you cannot tell apart.
        std::vector<std::string> WrapLabel(ImFont* font, f32 fontSize, const std::string& text,
                                           f32 wrapWidth, int maxLines) {
            std::vector<std::string> lines;
            const char* begin = text.c_str();
            const char* end = begin + text.size();
            const char* cursor = begin;
            while (cursor < end && static_cast<int>(lines.size()) < maxLines) {
                const char* stop = font->CalcWordWrapPosition(fontSize, cursor, end, wrapWidth);
                if (stop <= cursor) stop = cursor + 1;   // one glyph wider than the box: still progress
                const bool last = static_cast<int>(lines.size()) == maxLines - 1;
                if (last && stop < end) {
                    lines.push_back(Ellipsize(std::string(cursor, end), wrapWidth));
                    break;
                }
                lines.emplace_back(cursor, stop);
                cursor = stop;
                while (cursor < end && *cursor == ' ') ++cursor;   // spaces folded into the break
            }
            return lines;
        }

        void OpenInFileManager(const fs::path& path) {
            const std::string command = "xdg-open '" + path.string() + "' >/dev/null 2>&1 &";
            (void)std::system(command.c_str());
        }

    }

    std::filesystem::path DrawFilesPanel(ScriptSession& session, EditorState& state) {
        ImGui::Begin("Files###Files");

        // Wherever the project lives. Before anything has been saved that is the engine root, which
        // is also where a new project's script would be written — so the panel is never empty and
        // never pointing somewhere the editor is not actually working.
        fs::path root = state.Path().empty() ? session.SourcePath().parent_path()
                                             : state.Path().parent_path();
        if (root.empty()) root = FileSystem::EngineRoot();

        std::error_code ec;
        if (g_Files.root != root) {
            g_Files.root = root;
            g_Files.directory = root;
        }
        if (g_Files.directory.empty() || !fs::is_directory(g_Files.directory, ec))
            g_Files.directory = root;

        // --- header: up, breadcrumb, filter, tile size ------------------------------------------
        ImGui::BeginDisabled(g_Files.directory == root);
        if (ImGui::Button(kIconUp)) g_Files.directory = g_Files.directory.parent_path();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Up one folder");

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        {
            // The trail is clickable, so getting back out is one click on the name you want rather
            // than several on an arrow.
            const fs::path relative = fs::relative(g_Files.directory, root.parent_path(), ec);
            fs::path walk = root.parent_path();
            bool first = true;
            for (const auto& part : relative) {
                walk /= part;
                if (!first) { ImGui::SameLine(0.0f, 4.0f); ImGui::TextDisabled("/"); ImGui::SameLine(0.0f, 4.0f); }
                first = false;
                const std::string name = part.string();
                if (walk == g_Files.directory)
                    ImGui::TextColored(ImVec4(0.62f, 0.72f, 0.95f, 1.0f), "%s", name.c_str());
                else if (ImGui::TextLink(name.c_str()))
                    g_Files.directory = walk;
            }
        }

        const f32 filterWidth = 150.0f;
        const f32 sliderWidth = 96.0f;
        const f32 right = ImGui::GetWindowContentRegionMax().x;
        ImGui::SameLine(std::max(right - filterWidth - sliderWidth - 8.0f, ImGui::GetCursorPosX()));
        ImGui::SetNextItemWidth(filterWidth);
        ImGui::InputTextWithHint("##filter", "Filter", g_Files.filter, sizeof g_Files.filter);
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::SetNextItemWidth(sliderWidth);
        ImGui::SliderFloat("##tile", &g_Files.tile, 48.0f, 132.0f, "%.0f px");

        ImGui::Separator();

        // --- what is here ------------------------------------------------------------------------
        std::vector<Entry> entries;
        for (const auto& item : fs::directory_iterator(g_Files.directory, ec)) {
            const std::string name = item.path().filename().string();
            if (!name.empty() && name[0] == '.') continue;          // dotfiles are not content
            const bool directory = item.is_directory(ec);
            const Kind kind = KindOf(item.path(), directory);
            if (kind == Kind::Other && !directory) continue;
            if (!Matches(name, g_Files.filter)) continue;
            entries.push_back({ item.path(), name, kind, directory ? 0u : item.file_size(ec) });
        }
        // Folders first, then by kind, then by name: the order you look for them in.
        std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
            return a.kind != b.kind ? a.kind < b.kind : a.name < b.name;
        });

        fs::path openProject;

        ImGui::BeginChild("##grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

        if (entries.empty()) {
            ImGui::TextDisabled(g_Files.filter[0] ? "Nothing matches that."
                                                  : "Nothing here yet. Save the project and it shows up.");
        }

        const f32 art = g_Files.tile;
        const f32 padX = 8.0f, padTop = 6.0f, padBottom = 4.0f, labelGap = 3.0f, gap = 8.0f;
        const f32 lineHeight = ImGui::GetTextLineHeight();
        const f32 tileW = art + padX * 2.0f;
        const f32 tileH = padTop + art + labelGap + kLabelLines * lineHeight + padBottom;
        const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap)
                                                         / (tileW + gap)));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 labelColour = ImGui::GetColorU32(ImVec4(0.85f, 0.86f, 0.89f, 1.0f));

        int index = 0;
        for (const Entry& entry : entries) {
            if (index % columns != 0) ImGui::SameLine(0.0f, gap);
            ++index;

            ImGui::PushID(entry.name.c_str());
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##tile", ImVec2(tileW, tileH));

            const bool hovered = ImGui::IsItemHovered();
            const bool current = entry.path == session.SourcePath() || entry.path == state.Path();
            if (hovered || ImGui::IsItemActive() || current) {
                const ImVec4 fill = current      ? ImVec4(0.36f, 0.51f, 0.89f, 0.22f)
                                  : ImGui::IsItemActive() ? ImVec4(1, 1, 1, 0.10f)
                                                          : ImVec4(1, 1, 1, 0.06f);
                draw->AddRectFilled(origin, ImVec2(origin.x + tileW, origin.y + tileH),
                                    ImGui::GetColorU32(fill), 7.0f);
                draw->AddRect(origin, ImVec2(origin.x + tileW, origin.y + tileH),
                              ImGui::GetColorU32(ImVec4(1, 1, 1, 0.10f)), 7.0f);
            }

            const f32 boxLeft = origin.x + (tileW - art) * 0.5f;
            const f32 boxTop = origin.y + padTop;
            DrawGlyph(draw, IconFor(entry.kind), ImVec2(boxLeft, boxTop),
                      ImVec2(boxLeft + art, boxTop + art),
                      ImGui::GetColorU32(TintFor(entry.kind)));

            // Up to two centred lines directly under the artwork, each one centred on the tile the
            // artwork is centred on — so the name and its icon share an axis.
            f32 lineY = boxTop + art + labelGap;
            for (const std::string& line : WrapLabel(ImGui::GetFont(), ImGui::GetFontSize(),
                                                     entry.name, tileW - 6.0f, kLabelLines)) {
                const ImVec2 extent = ImGui::CalcTextSize(line.c_str());
                draw->AddText(ImVec2(origin.x + (tileW - extent.x) * 0.5f, lineY), labelColour,
                              line.c_str());
                lineY += lineHeight;
            }

            const auto activate = [&] {
                switch (entry.kind) {
                    case Kind::Folder: g_Files.directory = entry.path; break;
                    case Kind::Script:
                        // Opening a script means showing it in the editor next to the canvas, not
                        // opening a second window over the design.
                        session.OpenSource(entry.path);
                        ImGui::SetWindowFocus("Script###Script");
                        break;
                    case Kind::Screen:
                        openProject = entry.path;
                        break;
                    default: break;
                }
            };

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                activate();
            else if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
                     && entry.kind == Kind::Folder)
                activate();

            if (hovered) {
                ImGui::SetTooltip("%s%s%s", entry.name.c_str(),
                                  entry.kind == Kind::Folder ? "" : "  ",
                                  entry.kind == Kind::Folder ? "" : SizeText(entry.size).c_str());
            }

            if (ImGui::BeginPopupContextItem("##menu")) {
                if (ImGui::MenuItem("Open")) activate();
                if (ImGui::MenuItem("Show in file manager")) OpenInFileManager(entry.path);
                ImGui::Separator();
                if (ImGui::MenuItem("Rename…")) {
                    g_Files.renaming = entry.path;
                    std::snprintf(g_Files.rename, sizeof g_Files.rename, "%s", entry.name.c_str());
                }
                if (ImGui::MenuItem("Delete…")) g_Files.deleting = entry.path;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        // Right-clicking the empty space is how a folder gets made, which is where people look.
        if (ImGui::BeginPopupContextWindow("##background",
                                           ImGuiPopupFlags_MouseButtonRight
                                           | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem("New folder")) {
                fs::path candidate = g_Files.directory / "New folder";
                for (int i = 1; fs::exists(candidate, ec) && i < 100; ++i)
                    candidate = g_Files.directory / ("New folder " + std::to_string(i));
                fs::create_directory(candidate, ec);
            }
            if (ImGui::MenuItem("Create script", nullptr, false, !session.HasSource()))
                session.CreateSource();
            ImGui::Separator();
            if (ImGui::MenuItem("Show in file manager")) OpenInFileManager(g_Files.directory);
            ImGui::EndPopup();
        }

        ImGui::EndChild();

        // --- rename and delete, behind a confirmation ---------------------------------------------
        if (!g_Files.renaming.empty()) {
            ImGui::OpenPopup("Rename###RenameFile");
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                    ImVec2(0.5f, 0.5f));
        }
        if (ImGui::BeginPopupModal("Rename###RenameFile", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(280.0f);
            const bool entered = ImGui::InputText("##name", g_Files.rename, sizeof g_Files.rename,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("Rename") || entered) {
                const fs::path target = g_Files.renaming.parent_path() / g_Files.rename;
                fs::rename(g_Files.renaming, target, ec);
                if (ec) VAE_ERROR("rename failed: {}", ec.message());
                g_Files.renaming.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                g_Files.renaming.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (!g_Files.deleting.empty()) {
            ImGui::OpenPopup("Delete###DeleteFile");
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                    ImVec2(0.5f, 0.5f));
        }
        if (ImGui::BeginPopupModal("Delete###DeleteFile", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete %s?", g_Files.deleting.filename().string().c_str());
            ImGui::TextDisabled("This cannot be undone.");
            ImGui::Separator();
            if (ImGui::Button("Delete")) {
                fs::remove_all(g_Files.deleting, ec);
                if (ec) VAE_ERROR("delete failed: {}", ec.message());
                g_Files.deleting.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                g_Files.deleting.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End();
        return openProject;
    }

}
