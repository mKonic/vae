#include "vaepch.h"
#include "vae/app/ImGuiTheme.h"

#include <imgui.h>

namespace vae::app {

    namespace {
        // The same slate-and-indigo palette the widget library's tokens use, so the editor chrome
        // and the document being edited read as one product rather than two.
        constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) {
            return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
        }

        const ImVec4 kBg         = Rgb(22, 24, 30);
        const ImVec4 kSurface    = Rgb(33, 36, 45);
        const ImVec4 kSurfaceAlt = Rgb(45, 49, 60);
        const ImVec4 kRaised     = Rgb(56, 61, 75);
        const ImVec4 kBorder     = Rgb(65, 71, 85);
        const ImVec4 kText       = Rgb(234, 237, 242);
        const ImVec4 kTextMuted  = Rgb(147, 156, 171);
        const ImVec4 kAccent     = Rgb(93, 130, 228);
        const ImVec4 kAccentDim  = Rgb(74, 109, 206);
    }

    void ApplyStudioTheme() {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* c = style.Colors;

        c[ImGuiCol_Text]                  = kText;
        c[ImGuiCol_TextDisabled]          = kTextMuted;
        c[ImGuiCol_WindowBg]              = kSurface;
        c[ImGuiCol_ChildBg]               = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg]               = kSurfaceAlt;
        c[ImGuiCol_Border]                = kBorder;
        c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

        c[ImGuiCol_FrameBg]               = kSurfaceAlt;
        c[ImGuiCol_FrameBgHovered]        = kRaised;
        c[ImGuiCol_FrameBgActive]         = kAccentDim;

        c[ImGuiCol_TitleBg]               = kBg;
        c[ImGuiCol_TitleBgActive]         = kBg;
        c[ImGuiCol_TitleBgCollapsed]      = kBg;
        c[ImGuiCol_MenuBarBg]             = kBg;

        c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]         = kRaised;
        c[ImGuiCol_ScrollbarGrabHovered]  = kBorder;
        c[ImGuiCol_ScrollbarGrabActive]   = kAccent;

        c[ImGuiCol_CheckMark]             = kAccent;
        c[ImGuiCol_SliderGrab]            = kAccent;
        c[ImGuiCol_SliderGrabActive]      = kAccentDim;

        c[ImGuiCol_Button]                = kSurfaceAlt;
        c[ImGuiCol_ButtonHovered]         = kRaised;
        c[ImGuiCol_ButtonActive]          = kAccentDim;

        c[ImGuiCol_Header]                = kRaised;
        c[ImGuiCol_HeaderHovered]         = kAccentDim;
        c[ImGuiCol_HeaderActive]          = kAccent;

        c[ImGuiCol_Separator]             = kBorder;
        c[ImGuiCol_SeparatorHovered]      = kAccent;
        c[ImGuiCol_SeparatorActive]       = kAccent;

        c[ImGuiCol_ResizeGrip]            = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered]     = kAccentDim;
        c[ImGuiCol_ResizeGripActive]      = kAccent;

        c[ImGuiCol_Tab]                   = kBg;
        c[ImGuiCol_TabHovered]            = kRaised;
        c[ImGuiCol_TabSelected]           = kSurface;
        c[ImGuiCol_TabSelectedOverline]   = kAccent;
        c[ImGuiCol_TabDimmed]             = kBg;
        c[ImGuiCol_TabDimmedSelected]     = kSurface;

        c[ImGuiCol_DockingPreview]        = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.45f);
        c[ImGuiCol_DockingEmptyBg]        = kBg;

        c[ImGuiCol_TableHeaderBg]         = kSurfaceAlt;
        c[ImGuiCol_TableBorderStrong]     = kBorder;
        c[ImGuiCol_TableBorderLight]      = kSurfaceAlt;
        c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt]         = ImVec4(1, 1, 1, 0.02f);

        c[ImGuiCol_TextSelectedBg]        = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
        c[ImGuiCol_NavCursor]             = kAccent;

        // Squared-off but not sharp: enough radius to read as a modern tool, not so much that the
        // panels look like cards floating on nothing.
        style.WindowRounding    = 6.0f;
        style.ChildRounding     = 4.0f;
        style.FrameRounding     = 4.0f;
        style.PopupRounding     = 6.0f;
        style.ScrollbarRounding = 4.0f;
        style.GrabRounding      = 4.0f;
        style.TabRounding       = 4.0f;

        style.WindowBorderSize  = 1.0f;
        style.ChildBorderSize   = 1.0f;
        style.FrameBorderSize   = 0.0f;
        style.PopupBorderSize   = 1.0f;
        style.TabBarBorderSize  = 0.0f;

        style.WindowPadding     = ImVec2(10.0f, 10.0f);
        style.FramePadding      = ImVec2(8.0f, 5.0f);
        style.ItemSpacing       = ImVec2(8.0f, 6.0f);
        style.ItemInnerSpacing  = ImVec2(6.0f, 5.0f);
        style.IndentSpacing     = 18.0f;
        style.ScrollbarSize     = 11.0f;
        style.GrabMinSize       = 11.0f;

        style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
        style.SeparatorTextBorderSize = 1.0f;
        style.SeparatorTextPadding = ImVec2(16.0f, 6.0f);
    }

}
