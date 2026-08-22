#pragma once

#include "../EditorState.h"

#include <imgui.h>

// Inspector field helpers. Every one of them writes through a command and ends the coalescing run
// when the edit finishes, so a drag on a number field is exactly one undo entry.
namespace vae::fields {

    inline constexpr f32 kLabelWidth = 96.0f;

    // Lays out "label   [control]" without a table, so fields can nest inside collapsing headers.
    inline bool Label(const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(kLabelWidth);
        ImGui::SetNextItemWidth(-1.0f);
        return true;
    }

    bool Number(EditorState& state, Uuid node, const char* label, doc::Prop prop, f32 fallback,
                f32 speed = 1.0f, f32 min = 0.0f, f32 max = 0.0f);
    bool Toggle(EditorState& state, Uuid node, const char* label, doc::Prop prop, bool fallback);
    bool Colour(EditorState& state, Uuid node, const char* label, doc::Prop prop, Color fallback);
    // The same field, filed under an explicit key — a state overlay like "hovered:fill".
    bool Colour(EditorState& state, Uuid node, const char* label, std::string key, doc::Prop prop,
                Color fallback);
    bool Text(EditorState& state, Uuid node, const char* label, doc::Prop prop,
              const char* hint = nullptr, bool multiline = false);
    bool Choice(EditorState& state, Uuid node, const char* label, doc::Prop prop,
                const char* const* options, int count, int fallback);

    // Layout fields take the style by reference and report whether it changed; the caller commits
    // the whole style in one command.
    bool SizeField(const char* label, layout::Size& size);
    bool EdgesField(const char* label, Edges& edges);
    bool Vec2Field(const char* label, Vec2& value, f32 speed = 1.0f);
    bool EnumField(const char* label, int& value, const char* const* options, int count);

}
