#include "vaepch.h"
#include "vae/doc/LayoutText.h"

#include "vae/doc/ValueText.h"

#include <array>
#include <cctype>

namespace vae::doc {

    namespace text {

        // ------------------------------------------------------------------ names

        const char* LayoutModeName(layout::LayoutMode m) {
            switch (m) {
                case layout::LayoutMode::Absolute: return "absolute";
                case layout::LayoutMode::Stack:    return "stack";
                case layout::LayoutMode::Grid:     return "grid";
            }
            return "absolute";
        }

        const char* AxisName(layout::Axis a) {
            return a == layout::Axis::Row ? "row" : "column";
        }

        const char* AlignName(layout::Align a) {
            switch (a) {
                case layout::Align::Start:   return "start";
                case layout::Align::Center:  return "center";
                case layout::Align::End:     return "end";
                case layout::Align::Stretch: return "stretch";
            }
            return "start";
        }

        const char* JustifyName(layout::Justify j) {
            switch (j) {
                case layout::Justify::Start:        return "start";
                case layout::Justify::Center:       return "center";
                case layout::Justify::End:          return "end";
                case layout::Justify::SpaceBetween: return "spaceBetween";
                case layout::Justify::SpaceAround:  return "spaceAround";
                case layout::Justify::SpaceEvenly:  return "spaceEvenly";
            }
            return "start";
        }

        const char* ConstraintName(layout::Constraint c) {
            switch (c) {
                case layout::Constraint::Start:    return "start";
                case layout::Constraint::End:      return "end";
                case layout::Constraint::StartEnd: return "startEnd";
                case layout::Constraint::Center:   return "center";
                case layout::Constraint::Scale:    return "scale";
            }
            return "start";
        }

        std::optional<layout::LayoutMode> LayoutModeFromName(std::string_view n) {
            if (n == "absolute") return layout::LayoutMode::Absolute;
            if (n == "stack")    return layout::LayoutMode::Stack;
            if (n == "grid")     return layout::LayoutMode::Grid;
            return std::nullopt;
        }
        std::optional<layout::Axis> AxisFromName(std::string_view n) {
            if (n == "row")    return layout::Axis::Row;
            if (n == "column") return layout::Axis::Column;
            return std::nullopt;
        }
        std::optional<layout::Align> AlignFromName(std::string_view n) {
            if (n == "start")   return layout::Align::Start;
            if (n == "center")  return layout::Align::Center;
            if (n == "end")     return layout::Align::End;
            if (n == "stretch") return layout::Align::Stretch;
            return std::nullopt;
        }
        std::optional<layout::Justify> JustifyFromName(std::string_view n) {
            if (n == "start")        return layout::Justify::Start;
            if (n == "center")       return layout::Justify::Center;
            if (n == "end")          return layout::Justify::End;
            if (n == "spaceBetween") return layout::Justify::SpaceBetween;
            if (n == "spaceAround")  return layout::Justify::SpaceAround;
            if (n == "spaceEvenly")  return layout::Justify::SpaceEvenly;
            return std::nullopt;
        }
        std::optional<layout::Constraint> ConstraintFromName(std::string_view n) {
            if (n == "start")    return layout::Constraint::Start;
            if (n == "end")      return layout::Constraint::End;
            if (n == "startEnd") return layout::Constraint::StartEnd;
            if (n == "center")   return layout::Constraint::Center;
            if (n == "scale")    return layout::Constraint::Scale;
            return std::nullopt;
        }

        // ------------------------------------------------------------------ scalars

        // A size says its mode in its spelling: a bare number is pixels, which is the case that
        // appears on 39 of Vaecord's 61 nodes and the one worth making shortest.
        std::string SizeText(const layout::Size& size) {
            switch (size.mode) {
                case layout::SizeMode::Fixed:   return Number(size.value);
                case layout::SizeMode::Hug:     return "hug";
                case layout::SizeMode::Fill:    return size.value == 1.0f ? "fill"
                                                                         : "fill " + Number(size.value);
                case layout::SizeMode::Percent: return Number(size.value * 100.0f) + "%";
            }
            return "hug";
        }

        std::optional<layout::Size> SizeFromText(std::string_view s) {
            if (s == "hug")  return layout::Size::Hug();
            if (s == "fill") return layout::Size::Fill();
            if (s.starts_with("fill ")) {
                if (auto w = ParseNumber(s.substr(5))) return layout::Size::Fill(*w);
                return std::nullopt;
            }
            if (s.ends_with("%")) {
                if (auto f = ParseNumber(s.substr(0, s.size() - 1)))
                    return layout::Size::Percent(*f / 100.0f);
                return std::nullopt;
            }
            if (auto px = ParseNumber(s)) return layout::Size::Px(*px);
            return std::nullopt;
        }

        namespace {
            // Whitespace-separated numbers, or nothing. Local to this file: edges are the only
            // thing that takes a variable count of them.
            std::optional<std::vector<f32>> Numbers(std::string_view s) {
                std::vector<f32> out;
                std::size_t at = 0;
                while (at < s.size()) {
                    while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) ++at;
                    const std::size_t start = at;
                    while (at < s.size() && !std::isspace(static_cast<unsigned char>(s[at]))) ++at;
                    if (at == start) break;
                    const auto value = ParseNumber(s.substr(start, at - start));
                    if (!value) return std::nullopt;
                    out.push_back(*value);
                }
                return out.empty() ? std::nullopt : std::optional{ std::move(out) };
            }
        }

        // One value for all four, two for horizontal and vertical, four for l t r b. The short
        // forms are exactly Edges(f32) and Edges(h, v), so the file and the constructor agree —
        // note this is NOT css order, which is t r b l.
        std::string EdgesText(const Edges& e) {
            if (e.left == e.right && e.top == e.bottom)
                return e.left == e.top ? Number(e.left) : Number(e.left) + " " + Number(e.top);
            return Number(e.left) + " " + Number(e.top) + " " + Number(e.right) + " "
                 + Number(e.bottom);
        }

        std::optional<Edges> EdgesFromText(std::string_view s) {
            auto nums = Numbers(s);
            if (!nums) return std::nullopt;
            if (nums->size() == 1) return Edges{ (*nums)[0] };
            if (nums->size() == 2) return Edges{ (*nums)[0], (*nums)[1] };
            if (nums->size() == 4) return Edges{ (*nums)[0], (*nums)[1], (*nums)[2], (*nums)[3] };
            return std::nullopt;
        }

    }

    // ---------------------------------------------------------------------- the table

    namespace {

        using namespace vae::doc::text;

        // Every entry is the same four lines, so the difference between two fields is only ever
        // the two things that actually differ: which member, and how it is spelled.
        //
        // `write` compares against a default-constructed LayoutStyle, which is what "write only
        // what differs" means and is why it lives in the table rather than at the call site.
#define VAE_LAYOUT_FIELD(NAME, MEMBER, TO_TEXT, FROM_TEXT)                                       \
    LayoutField{                                                                                 \
        NAME,                                                                                    \
        [](layout::LayoutStyle& s, std::string_view v) {                                         \
            if (auto parsed = FROM_TEXT(v)) { s.MEMBER = *parsed; return true; }                 \
            return false;                                                                        \
        },                                                                                       \
        [](const layout::LayoutStyle& s) -> std::optional<std::string> {                         \
            if (s.MEMBER == layout::LayoutStyle{}.MEMBER) return std::nullopt;                   \
            return std::string(TO_TEXT(s.MEMBER));                                               \
        },                                                                                       \
    }

        // Booleans and integers have no *FromName pair of their own; these give them one so every
        // field goes through the same macro rather than being the one hand-written exception.
        std::optional<bool> BoolFromText(std::string_view v) {
            if (v == "true")  return true;
            if (v == "false") return false;
            return std::nullopt;
        }
        std::string BoolText(bool b) { return b ? "true" : "false"; }

        std::optional<u16> ColumnsFromText(std::string_view v) {
            if (auto n = ParseNumber(v); n && *n >= 0.0f) return static_cast<u16>(*n);
            return std::nullopt;
        }
        std::string ColumnsText(u16 n) { return Number(static_cast<f32>(n)); }

        std::string NumberText(f32 v) { return Number(v); }

        // In the order LayoutStyle declares them.
        constexpr std::size_t kFieldCount = 19;
        const std::array<LayoutField, kFieldCount>& Table() {
            static const std::array<LayoutField, kFieldCount> kFields{
                VAE_LAYOUT_FIELD("mode",        mode,        LayoutModeName,  LayoutModeFromName),
                VAE_LAYOUT_FIELD("axis",        axis,        AxisName,        AxisFromName),
                VAE_LAYOUT_FIELD("width",       width,       SizeText,        SizeFromText),
                VAE_LAYOUT_FIELD("height",      height,      SizeText,        SizeFromText),
                VAE_LAYOUT_FIELD("padding",     padding,     EdgesText,       EdgesFromText),
                VAE_LAYOUT_FIELD("gap",         gap,         NumberText,      ParseNumber),
                VAE_LAYOUT_FIELD("align",       align,       AlignName,       AlignFromName),
                VAE_LAYOUT_FIELD("justify",     justify,     JustifyName,     JustifyFromName),
                VAE_LAYOUT_FIELD("wrap",        wrap,        BoolText,        BoolFromText),
                VAE_LAYOUT_FIELD("columns",     columns,     ColumnsText,     ColumnsFromText),
                VAE_LAYOUT_FIELD("minColumn",   minColumn,   NumberText,      ParseNumber),
                VAE_LAYOUT_FIELD("rowGap",      rowGap,      NumberText,      ParseNumber),
                VAE_LAYOUT_FIELD("minSize",     minSize,     Vec2Text,        Vec2FromText),
                // An unbounded max is the default and usually absent; when only one axis is
                // bounded, to_chars writes the other as "inf" and from_chars reads it back.
                VAE_LAYOUT_FIELD("maxSize",     maxSize,     Vec2Text,        Vec2FromText),
                VAE_LAYOUT_FIELD("aspectRatio", aspectRatio, NumberText,      ParseNumber),
                VAE_LAYOUT_FIELD("offsetStart", offsetStart, Vec2Text,        Vec2FromText),
                VAE_LAYOUT_FIELD("offsetEnd",   offsetEnd,   Vec2Text,        Vec2FromText),
                VAE_LAYOUT_FIELD("constraintX", constraintX, ConstraintName,  ConstraintFromName),
                VAE_LAYOUT_FIELD("constraintY", constraintY, ConstraintName,  ConstraintFromName),
            };
            return kFields;
        }

#undef VAE_LAYOUT_FIELD

    }

    std::span<const LayoutField> LayoutFields() { return Table(); }

    const LayoutField* LayoutFieldNamed(std::string_view name) {
        for (const LayoutField& field : Table())
            if (field.name == name) return &field;
        return nullptr;
    }

}
