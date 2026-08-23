#pragma once

#include "vae/doc/Value.h"

#include <map>
#include <optional>

namespace vae::doc {

    // Well-known property keys. Interned as an enum rather than free strings because they are
    // written on every node, compared on every override lookup and serialized on every save.
    // Custom component properties still use string keys (see PropBag::custom).
    enum class Prop : u16 {
        // paint
        Fill, FillOpacity, Stroke, StrokeWidth, CornerRadius, Opacity, Visible, ClipContent,
        ShadowColor, ShadowOffset, ShadowBlur, ShadowSpread,
        // text
        Text, FontFamily, FontSize, FontWeight, FontItalic, TextColor, TextAlign, LineHeight,
        LetterSpacing, TextWrap,
        // image
        Image, ImageFit,
        // interaction
        Enabled, Tooltip, Cursor,
        // widgets. A node's Role is what turns a styled box into a Button — the visuals stay a
        // document a designer can open and restyle, and the role is the seam a native behavior
        // attaches through.
        Role, Checked, Value, MinValue, MaxValue, Step, Placeholder, SelectedIndex, Group,
        Multiline, Password, ReadOnly, MaxLength, Open, Route, ScrollX, ScrollY,
        ItemHeight, ItemCount, Duration, Modal, ColumnWidth,
        // A screen's kind, and where a declared navigation goes. Both live on the document so the
        // player and the exporter know what to do without a script telling them.
        ScreenKind, GoTo,
        // A chart's numbers and its shape. The numbers live on the document so a designer can lay
        // a chart out without a script, and a script sets the same property when there is one.
        Series, ChartKind,
        // Any text node can be selected and copied from. A property rather than a component,
        // because "you can take this text out" is a fact about a label, not a different widget —
        // swapping a styled label for a SelectableText to get it loses the styling.
        Selectable,
        // A container that shows one of its children at a time, by name. Loading, failed, empty
        // and the content itself are four drawings of one screen, and a designer needs to look at
        // each of them without running anything.
        Shown,
        // A container that draws its first child over and over. The designer styles one row, one
        // card, one chip; a script says how many there are. A property on the container that was
        // already there, rather than a second List that happens to be data-driven.
        Repeat,
        // What a copy of a repeated container shows: the column of the row it stands for. The
        // designer styles one row and says which part of it is the author and which is the body;
        // the app hands over the rows. Without it a repeat is the same thing drawn N times, which
        // is a placeholder rather than a list.
        //
        // "author" fills the node's natural property — text on a label, image on a picture.
        // "fill:colour" fills a named one, which is how a row carries a state colour or a badge
        // it only sometimes has ("visible:unread").
        Field,
        // On a screen: whether the app it becomes may be resized. A screen's width and height are
        // its *design* size — the artboard the canvas draws and the size the window opens at — and
        // by default the running app fills whatever window it is given and lays out again. Turning
        // this off makes those numbers a hard resolution and the window refuses to change.
        Resizable,
        // Rows to draw a repeated container with while it is being designed. The app hands over
        // the real ones at runtime; without these the canvas draws one row N times and a designer
        // is styling a template they cannot see. Text, because a row is text: the first line names
        // the columns and every line after it is a row, cells separated by `|`.
        Sample,
        // A container that fills from its far edge: content shorter than the box sits against the
        // bottom of it, and content taller than the box scrolls, staying at the end while it is at
        // the end. This is what a chat log is, and what `justify: end` only looks like until the
        // conversation is long enough to overflow — at which point a justification pushes the
        // newest message out of the top of the box instead of scrolling to it.
        StickToEnd,
        Count
    };

    const char* PropName(Prop prop);
    std::optional<Prop> PropFromName(std::string_view name);

    // What a property holds, when that is knowable from the property alone. A format where every
    // value is text — an XML attribute — cannot tell `text="1"` from `fontSize="1"` by shape, so the
    // reader asks here instead: Prop::Text is Text, Prop::FontSize is Number, and neither has to be
    // spelled with an escape.
    //
    // `Unset` means "the shape decides", which is the honest answer for the few properties that are
    // genuinely polymorphic — Prop::Value is text on a field and a number on a slider — and for
    // every custom property, whose name is a free string and whose type is nobody's to declare.
    // Those are the only ones that ever need the '$' escape to keep a numeric-looking string.
    ValueType PropValueType(Prop prop);

    // A sparse property map. Sparse on purpose: an override records only the properties that were
    // actually changed, and "unset" has to stay distinguishable from "set to the default" or an
    // instance could never fall back to its component.
    class PropBag {
    public:
        bool Has(Prop prop) const { return m_Props.contains(prop); }
        bool Has(std::string_view key) const;

        const Value* Find(Prop prop) const;
        const Value* Find(std::string_view key) const;

        void Set(Prop prop, Value value);
        void Set(std::string key, Value value);
        void Unset(Prop prop);
        void Unset(std::string_view key);

        // Typed reads with a fallback, for call sites that just want a number.
        f32         Number(Prop prop, f32 fallback = 0.0f) const;
        bool        Flag(Prop prop, bool fallback = false) const;
        Color       Colour(Prop prop, Color fallback = { 0, 0, 0, 0 }) const;
        std::string Text(Prop prop, std::string fallback = {}) const;

        // Every property set here overwrites the same property in `into`. This is the override
        // merge, and its direction is the whole component model: instance beats component default.
        void MergeInto(PropBag& into) const;

        void Clear();
        bool Empty() const { return m_Props.empty() && m_Custom.empty(); }
        std::size_t Size() const { return m_Props.size() + m_Custom.size(); }

        const std::map<Prop, Value>& Known() const { return m_Props; }
        const std::map<std::string, Value>& Custom() const { return m_Custom; }

        bool operator==(const PropBag&) const = default;

    private:
        std::map<Prop, Value> m_Props;
        std::map<std::string, Value> m_Custom;
    };

}
