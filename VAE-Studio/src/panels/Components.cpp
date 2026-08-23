#include "Panels.h"
#include "../Canvas.h"

#include "vae/app/ImGuiLayer.h"

#include <algorithm>
#include <map>
#include <string_view>

#include <imgui.h>
#include <imgui_internal.h>

namespace vae {

    namespace {

        // Nerd Font glyph, the same private-use range the Layers and Files panels use. The id is
        // baked into it because ImGui takes one string, and a `constexpr const char*` does not
        // concatenate with a literal the way a literal does.
        constexpr const char* kIconSoundPlay = "\uf028##play";


        // Letters in order, not necessarily together: "inp" finds InputOtp and "clndr" finds
        // Calendar. A strict substring would make half the catalog unreachable by the abbreviation
        // people actually type.
        bool Matches(std::string_view name, std::string_view query) {
            const auto fold = [](char c) {
                return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            };
            std::size_t at = 0;
            for (const char wanted : query) {
                if (wanted == ' ') continue;
                while (at < name.size() && fold(name[at]) != fold(wanted)) ++at;
                if (at == name.size()) return false;
                ++at;
            }
            return true;
        }

    }

    void DrawComponentsPanel(EditorState& state, Canvas& canvas) {
        ImGui::Begin("Components###Components");

        // A component opened for editing is not on any screen, so the way back has to be here —
        // otherwise making one is a one-way trip out of the document.
        if (const Uuid editing = state.EditingComponent(); editing.Valid()) {
            const doc::Node* node = state.Doc().Find(editing);
            ImGui::TextColored(ImVec4(0.45f, 0.62f, 0.95f, 1.0f), "Editing %s",
                               node ? node->name.c_str() : "a component");
            ImGui::SameLine();
            if (ImGui::SmallButton("Done")) {
                state.CloseComponent();
                canvas.FrameAll(state);
            }
            ImGui::Separator();
        }

        // Fifty-odd components is past the point where scanning a grid is faster than typing, so
        // the filter comes before the grid rather than being tucked away in a menu.
        static char filter[64] = {};
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##filter", "Search components…", filter, sizeof filter,
                                 ImGuiInputTextFlags_EscapeClearsAll);

        ImGui::TextDisabled("Click to place · right-click to edit.");
        ImGui::Separator();

        const f32 width = ImGui::GetContentRegionAvail().x;
        const int columns = std::max(1, static_cast<int>(width / 120.0f));
        int column = 0;
        int shown = 0;

        for (const auto& [name, component] : state.Library().components) {
            if (!Matches(name, filter)) continue;
            ++shown;
            ImGui::PushID(name.c_str());
            if (ImGui::Button(name.c_str(), ImVec2(width / columns - 8.0f, 30.0f))) {
                // Placing at the centre of what is on screen, not at the origin: a widget dropped
                // somewhere you are not looking may as well not have been placed.
                state.PlaceInstance(name, state.ActiveScreen(), canvas.ViewCenter());
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("VAE_COMPONENT", name.c_str(), name.size() + 1);
                ImGui::TextUnformatted(name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("##component")) {
                if (ImGui::MenuItem("Edit this component")) {
                    state.OpenComponent(component);
                    canvas.FrameAll(state);
                }
                ImGui::TextDisabled("Changes reach every instance of it.");
                ImGui::EndPopup();
            }
            ImGui::PopID();

            if (++column % columns != 0) ImGui::SameLine();
        }

        if (shown == 0) {
            if (column != 0) ImGui::NewLine();
            ImGui::TextDisabled("Nothing matches \"%s\".", filter);
        }

        ImGui::End();
    }

    void DrawScreensPanel(EditorState& state, Canvas& canvas) {
        ImGui::Begin("Screens###Screens");

        if (ImGui::Button("  New screen", ImVec2(-1.0f, 0.0f))) {
            const Uuid screen = state.AddScreen("Screen " + std::to_string(state.Screens().size() + 1),
                                                { 1280.0f, 800.0f });
            state.SetActiveScreen(screen);
            canvas.FrameAll(state);
        }
        ImGui::Separator();

        const Uuid start = state.Doc().StartScreen();
        for (Uuid screen : state.Screens()) {
            const doc::Node* node = state.Doc().Find(screen);
            if (!node) continue;
            const doc::ScreenKind kind = state.Doc().KindOf(screen);

            ImGui::PushID(static_cast<int>(screen.Value()));
            // The start screen is marked rather than named in a field somewhere: which screen an
            // app opens on belongs next to the list of screens, not in a settings dialog.
            const bool isStart = screen == start;
            if (ImGui::Selectable(isStart ? ("\u25b8 " + node->name).c_str() : node->name.c_str(),
                                  screen == state.ActiveScreen())) {
                state.SetActiveScreen(screen);
                canvas.FrameAll(state);
            }

            if (ImGui::BeginPopupContextItem("##screen")) {
                if (ImGui::MenuItem("Open on this screen", nullptr, isStart, !isStart)) {
                    state.Execute(CreateScope<doc::SetStartScreenCommand>(screen));
                    state.Doc().Touch(screen);
                }
                ImGui::Separator();
                ImGui::TextDisabled("Kind");
                for (int i = 0; i < static_cast<int>(doc::ScreenKind::Count); ++i) {
                    const auto candidate = static_cast<doc::ScreenKind>(i);
                    if (ImGui::MenuItem(doc::ScreenKindName(candidate), nullptr, kind == candidate))
                        state.SetProp(screen, doc::Prop::ScreenKind,
                                      std::string(doc::ScreenKindName(candidate)));
                }
                ImGui::EndPopup();
            }

            ImGui::SameLine(ImGui::GetContentRegionMax().x - 150.0f);
            if (kind != doc::ScreenKind::Page)
                ImGui::TextColored(ImVec4(0.62f, 0.78f, 0.98f, 1.0f), "%s",
                                   doc::ScreenKindName(kind));
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 90.0f);
            ImGui::TextDisabled("%.0f x %.0f", node->layout.width.value, node->layout.height.value);
            ImGui::PopID();
        }

        if (state.Screens().size() > 1) {
            ImGui::Spacing();
            ImGui::TextDisabled("Right-click a screen for its kind and to open on it.");
        }

        ImGui::End();
    }

    namespace {

        // An ImGui-drawable handle per asset, made once. Asking for one every frame allocates a
        // descriptor set every frame, which is a leak that takes a few minutes to become a crash.
        u64 ThumbnailHandle(Uuid id, const Ref<gpu::Texture>& texture) {
            struct Cached { const gpu::Texture* texture = nullptr; u64 handle = 0; };
            static std::map<Uuid, Cached> cache;

            Cached& slot = cache[id];
            if (slot.texture == texture.get()) return slot.handle;
            if (slot.handle) app::ImGuiLayer::ReleaseTextureHandle(slot.handle);
            slot.texture = texture.get();
            slot.handle = texture ? app::ImGuiLayer::TextureHandle(texture) : 0;
            return slot.handle;
        }

    }

    // The project's palette, editable. Every token here resolves through the active theme, so a
    // colour changed once repaints every widget that named it — which is the whole reason tokens
    // exist and, until now, the one thing the editor could not do to them.
    void DrawTokensPanel(EditorState& state) {
        // A panel added after a layout was saved has nowhere to go, and floats over the canvas as
        // a stamp-sized window. Docked beside the Inspector on first sight, which is where the
        // default layout puts it and where anyone with an older layout would have dragged it.
        if (const ImGuiWindow* beside = ImGui::FindWindowByName("Inspector###Inspector"))
            if (beside->DockId != 0)
                ImGui::SetNextWindowDockID(beside->DockId, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360.0f, 480.0f), ImGuiCond_FirstUseEver);

        ImGui::Begin("Tokens###Tokens");

        static char fresh[64] = {};
        ImGui::SetNextItemWidth(-90.0f);
        const bool entered = ImGui::InputTextWithHint("##new", "New token name…", fresh,
                                                      sizeof fresh,
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        const bool taken = fresh[0] != '\0' && state.Doc().FindToken(fresh) != nullptr;
        ImGui::BeginDisabled(fresh[0] == '\0' || taken);
        if (ImGui::Button("Add", ImVec2(-1.0f, 0.0f)) || (entered && !taken && fresh[0] != '\0')) {
            // A new token starts at the accent colour rather than black, so it is visible the
            // moment it is used and obviously not finished.
            doc::Token token;
            token.dark = token.light = Color{ 0.365f, 0.51f, 0.894f, 1.0f };
            state.Execute(CreateScope<doc::SetTokenCommand>(std::string(fresh), token));
            fresh[0] = '\0';
        }
        ImGui::EndDisabled();
        if (taken) ImGui::TextDisabled("there is already a token called that");

        ImGui::Separator();

        const doc::Theme theme = state.Doc().ActiveTheme();
        std::string removing;
        std::pair<std::string, std::string> renaming;

        for (const auto& [name, token] : state.Doc().Tokens()) {
            ImGui::PushID(name.c_str());

            // The swatch shows the value for the theme the canvas is showing, because that is the
            // one the designer is looking at.
            const doc::Value shown = theme == doc::Theme::Dark ? token.dark : token.light;
            Color colour{ 0.0f, 0.0f, 0.0f, 1.0f };
            if (const Color* c = std::get_if<Color>(&shown)) colour = *c;

            ImVec4 edited(colour.r, colour.g, colour.b, colour.a);
            if (ImGui::ColorEdit4("##swatch", &edited.x,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
                doc::Token next = token;
                const Color value{ edited.x, edited.y, edited.z, edited.w };
                if (theme == doc::Theme::Dark) next.dark = value; else next.light = value;
                state.Execute(CreateScope<doc::SetTokenCommand>(name, next));
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) state.EndGesture();

            ImGui::SameLine();
            char label[64];
            std::snprintf(label, sizeof label, "%s", name.c_str());
            ImGui::SetNextItemWidth(-64.0f);
            if (ImGui::InputText("##name", label, sizeof label,
                                 ImGuiInputTextFlags_EnterReturnsTrue)
                && label[0] != '\0' && name != label) {
                renaming = { name, label };
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("×")) removing = name;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Delete this token. Anything using it falls back to nothing.");

            ImGui::PopID();
        }

        // Applied after the walk: both edit the map being iterated.
        if (!renaming.first.empty())
            state.Execute(CreateScope<doc::RenameTokenCommand>(renaming.first, renaming.second));
        if (!removing.empty())
            state.Execute(CreateScope<doc::RemoveTokenCommand>(removing));

        ImGui::End();
    }

    void DrawAssetsPanel(EditorState& state, Canvas& canvas) {
        ImGui::Begin("Assets###Assets");

        // No native file dialog to reach for, so importing is a path — the same way opening a
        // project works. A file is copied into the project folder, because an asset that lives
        // somewhere else is an asset the project loses the moment it is moved.
        static char path[512] = {};
        ImGui::SetNextItemWidth(-90.0f);
        const bool entered = ImGui::InputTextWithHint("##import", "Path to an image or a sound…",
                                                      path,
                                                      sizeof path,
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Import", ImVec2(-1.0f, 0.0f)) || entered) && path[0] != '\0') {
            if (state.ImportAsset(path).Valid()) path[0] = '\0';
        }
        if (!state.AssetError().empty())
            ImGui::TextColored(ImVec4(0.90f, 0.38f, 0.38f, 1.0f), "%s", state.AssetError().c_str());

        ImGui::Separator();
        if (state.Doc().Assets().empty()) {
            ImGui::TextDisabled("Nothing yet.");
            ImGui::TextDisabled("Import a picture and drop it on the canvas,");
            ImGui::TextDisabled("or a sound and play it from a script.");
            ImGui::End();
            return;
        }

        const f32 tile = 92.0f;
        const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / tile));
        int column = 0;
        Uuid remove = Uuid::Invalid();

        for (const doc::Document::Asset& asset : state.Doc().Assets()) {
            ImGui::PushID(static_cast<int>(asset.id.Value()));
            ImGui::BeginGroup();

            // Artwork is drawn for the tile rather than scaled up from whatever size the file
            // says it is, and in the panel's own text colour — a 24-pixel black icon blown up to
            // 92 on a dark panel is neither legible nor an honest preview.
            const bool artwork = canvas.Assets().IsVector(asset.id);
            // A sound has no picture, and inventing one — a waveform, a spectrum — would be a
            // drawing of something nobody asked to see. It gets a speaker and a button that plays
            // it, which is the only preview a sound has.
            const bool sound = canvas.Assets().IsSound(asset.id);
            const ImVec4 pen = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            const Color ink{ pen.x, pen.y, pen.z, 1.0f };
            const Ref<gpu::Texture> texture =
                sound   ? Ref<gpu::Texture>{}
              : artwork ? canvas.Assets().Vector(asset.id, { tile, tile },
                                                 canvas.Assets().FollowsText(asset.id) ? &ink
                                                                                        : nullptr)
                        : canvas.Assets().Image(asset.id);
            const Vec2 size = artwork ? Vec2{ tile, tile } : canvas.Assets().SizeOf(asset.id);
            const u64 handle = sound ? 0 : ThumbnailHandle(asset.id, texture);
            if (sound) {
                if (ImGui::Button(kIconSoundPlay, ImVec2(tile, tile)))
                    state.PreviewAsset(asset.id);
            } else if (handle) {
                // Fitted, not stretched: a thumbnail that lies about an image's shape is worse
                // than no thumbnail.
                const f32 scale = std::min(tile / std::max(size.x, 1.0f),
                                           tile / std::max(size.y, 1.0f));
                const ImVec2 shown(size.x * scale, size.y * scale);
                ImGui::Dummy(ImVec2(0.0f, (tile - shown.y) * 0.5f));
                ImGui::Image(static_cast<ImTextureID>(handle), shown);
            } else {
                ImGui::Button("?", ImVec2(tile, tile));
            }

            ImGui::TextUnformatted(asset.name.c_str());
            ImGui::EndGroup();

            // Sounds are not dragged onto the canvas: there is nothing on a screen for one to
            // land on, and a drag that does nothing is worse than one that never starts.
            if (!sound && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                const u64 id = asset.id.Value();
                ImGui::SetDragDropPayload("VAE_ASSET", &id, sizeof id);
                ImGui::TextUnformatted(asset.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered()) {
                const std::string problem = canvas.Assets().ProblemWith(asset.id);
                if (!problem.empty()) ImGui::SetTooltip("%s", problem.c_str());
                else if (sound)
                    ImGui::SetTooltip("%s\nClick to hear it. Play it from a script with "
                                      "play_sound(\"%s\").", asset.path.c_str(),
                                      asset.name.c_str());
                else ImGui::SetTooltip("%s — %.0f x %.0f\n%s", asset.path.c_str(), size.x, size.y,
                                       "Drop on the canvas to place it.");
            }
            if (ImGui::BeginPopupContextItem("##asset")) {
                if (sound) {
                    if (ImGui::MenuItem("Play it")) state.PreviewAsset(asset.id);
                    if (ImGui::MenuItem("Stop")) state.Preview().StopAll();
                } else if (ImGui::MenuItem("Use on the selection", nullptr, false,
                                           state.Primary().Valid())) {
                    state.SetProp(state.Primary(), doc::Prop::Image, doc::AssetRef{ asset.id });
                }
                if (ImGui::MenuItem("Remove from the project")) remove = asset.id;
                ImGui::TextDisabled("The file stays where it is.");
                ImGui::EndPopup();
            }

            ImGui::PopID();
            if (++column % columns != 0) ImGui::SameLine();
        }

        if (remove.Valid()) state.RemoveAsset(remove);
        ImGui::End();
    }

}
