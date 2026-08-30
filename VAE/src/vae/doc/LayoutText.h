#pragma once

#include "vae/doc/Value.h"
#include "vae/layout/LayoutTypes.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vae::doc {

    // A LayoutStyle as text, and the one table that knows how.
    //
    // The nineteen field names used to be written out three separate times inside the codec — once
    // in the reserved-attribute list, once in the writer as nineteen `if (s.x != d.x)` lines, and
    // once in the reader as a nineteen-branch else-if chain — with nothing binding them together.
    // Adding a twentieth field and forgetting one of the three failed quietly in the worst
    // direction: the encoder wrote an attribute the reader rejected as unknown, so a file VAE
    // wrote was a file VAE would not open.
    //
    // One table instead. The reader, the writer, the reserved-name check and the overlay merge all
    // walk it, so they cannot disagree about what a layout field is called or how it is spelled.
    namespace text {

        // The scalars a layout field is written in. Public because a size and a set of edges are
        // spellings a designer types, not just ones a codec emits.
        std::string SizeText(const layout::Size& size);
        std::optional<layout::Size> SizeFromText(std::string_view s);
        std::string EdgesText(const Edges& e);
        std::optional<Edges> EdgesFromText(std::string_view s);

        const char* LayoutModeName(layout::LayoutMode m);
        const char* AxisName(layout::Axis a);
        const char* AlignName(layout::Align a);
        const char* JustifyName(layout::Justify j);
        const char* ConstraintName(layout::Constraint c);

        std::optional<layout::LayoutMode> LayoutModeFromName(std::string_view n);
        std::optional<layout::Axis>       AxisFromName(std::string_view n);
        std::optional<layout::Align>      AlignFromName(std::string_view n);
        std::optional<layout::Justify>    JustifyFromName(std::string_view n);
        std::optional<layout::Constraint> ConstraintFromName(std::string_view n);

    }

    // One field of a LayoutStyle, and everything anyone needs to know about it.
    //
    // Function pointers rather than member pointers because the nineteen fields have eleven
    // different types; a member pointer would need the type back at every call site, which is the
    // nineteen-branch switch this table exists to delete.
    struct LayoutField {
        std::string_view name;

        // Reads `text` into `style`. False when the text does not name a value this field can hold
        // — which is a document that will not open rather than a field quietly left at its default.
        bool (*read)(layout::LayoutStyle& style, std::string_view text);

        // The field as text, always. What "only write what differs" is written *from*, rather than
        // the rule itself — the two callers differ about what they are comparing against.
        std::string (*text)(const layout::LayoutStyle& style);

        // Whether two styles disagree about this one field. The codec asks against a default style,
        // which is "does this need writing at all"; the Inspector asks against the base style,
        // which is "is this an overlay the designer just authored".
        bool (*differs)(const layout::LayoutStyle& a, const layout::LayoutStyle& b);
    };

    // In the order LayoutStyle declares them, which is what a written node reads best in — a
    // map would give alphabetical, and `align` before `axis` before `width` says nothing.
    std::span<const LayoutField> LayoutFields();

    // The field of that name, or null. This is what "is this attribute a layout field?" asks.
    const LayoutField* LayoutFieldNamed(std::string_view name);

}
