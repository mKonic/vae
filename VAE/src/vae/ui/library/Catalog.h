#pragma once

// The builder every widget in the catalog is written with, and the list of what it builds.
//
// The catalog used to be one 1,600-line translation unit — sixty widgets of nested frames, which is
// a third of VAE-Core in a file nobody reads top to bottom. The widgets are now four files by what
// they are for; this is the vocabulary they share, and it is a header rather than four copies
// because the builder is the reason each widget is readable at a glance.

#include "vae/doc/Document.h"
#include "vae/ui/Library.h"
#include "vae/ui/Widget.h"

namespace vae::ui::catalog {

    using namespace vae::layout;


    inline doc::Token Colours(Color light, Color dark) { return doc::Token{ light, dark, {} }; }

    // A terse builder, because the library is 20 widgets of nested frames and the shape of each
    // widget should be readable at a glance rather than buried in SetProp calls.
    class B {
    public:
        B(doc::Document& document, doc::NodeKind kind, Uuid parent, std::string name)
            : m_Doc(document), m_Id(document.CreateNode(kind, parent, std::move(name))) {}

        operator Uuid() const { return m_Id; }
        Uuid Id() const { return m_Id; }

        B& Role(ui::Role role) {
            return Set(doc::Prop::Role, std::string(RoleName(role)));
        }
        B& Set(doc::Prop prop, doc::Value value) {
            m_Doc.SetProp(m_Id, prop, std::move(value));
            return *this;
        }
        B& Token(doc::Prop prop, std::string name) {
            return Set(prop, doc::TokenRef{ std::move(name) });
        }
        B& On(StateBit bit, doc::Prop prop, std::string token) {
            Node()->props.Set(StateKey(bit, prop), doc::TokenRef{ std::move(token) });
            return *this;
        }
        B& OnValue(StateBit bit, doc::Prop prop, doc::Value value) {
            Node()->props.Set(StateKey(bit, prop), std::move(value));
            return *this;
        }
        // "Lighter", not "this blue". A widget whose base colour the designer changed keeps a
        // hover that follows it.
        B& OnTint(StateBit bit, f32 amount) {
            Node()->props.Set(StateTintKey(bit), amount);
            return *this;
        }
        template<typename Fn> B& Lay(Fn&& apply) {
            apply(Node()->layout);
            m_Doc.Touch(m_Id);
            return *this;
        }
        B& Hidden() { Node()->visible = false; m_Doc.Touch(m_Id); return *this; }
        // Where an instance's own children go. Everything already in here is the placeholder
        // the component shows until someone puts something in it.
        B& Slot() { Node()->slot = true; m_Doc.Touch(m_Id); return *this; }

    private:
        doc::Node* Node() { return m_Doc.Find(m_Id); }
        doc::Document& m_Doc;
        Uuid m_Id;
    };

    inline B Frame(doc::Document& d, Uuid parent, std::string name) {
        return B(d, doc::NodeKind::Frame, parent, std::move(name));
    }

    inline B Label(doc::Document& d, Uuid parent, std::string name, std::string content,
            std::string colour = "text", f32 size = 14.0f) {
        B node(d, doc::NodeKind::Text, parent, std::move(name));
        node.Set(doc::Prop::Text, std::move(content))
            .Token(doc::Prop::TextColor, std::move(colour))
            .Set(doc::Prop::FontSize, size)
            .Set(doc::Prop::TextWrap, std::string("none"));
        return node;
    }

    inline auto Stack(Axis axis, f32 gap, Edges padding, Align align = Align::Center,
               Justify justify = Justify::Start) {
        return [=](LayoutStyle& style) {
            style.mode = LayoutMode::Stack;
            style.axis = axis;
            style.gap = gap;
            style.padding = padding;
            style.align = align;
            style.justify = justify;
        };
    }

    inline auto Box(Size width, Size height) {
        return [=](LayoutStyle& style) { style.width = width; style.height = height; };
    }

    // Equal columns, filled row by row. `columns` of zero means as many as fit at `minColumn`,
    // which is the version that reflows on a resize instead of overflowing.
    inline auto Grid(u16 columns, f32 gap, f32 minColumn = 160.0f, Edges padding = {}) {
        return [=](LayoutStyle& style) {
            style.mode = LayoutMode::Grid;
            style.columns = columns;
            style.gap = gap;
            style.minColumn = minColumn;
            style.padding = padding;
        };
    }

    // A surface with a border and a radius: the shape almost every container in the catalog is,
    // said once rather than eight times.
    inline void Surface(B& node, f32 radius = 8.0f, const char* fill = "surface",
                 const char* border = "border") {
        node.Set(doc::Prop::CornerRadius, radius)
            .Token(doc::Prop::Fill, fill);
        if (border) node.Token(doc::Prop::Stroke, border).Set(doc::Prop::StrokeWidth, 1.0f);
    }

    inline void Shadow(B& node, f32 blur, f32 alpha) {
        node.Set(doc::Prop::ShadowColor, Color{ 0.0f, 0.0f, 0.0f, alpha })
            .Set(doc::Prop::ShadowBlur, blur)
            .Set(doc::Prop::ShadowOffset, Vec2{ 0.0f, blur * 0.35f });
    }

    // --- what the catalog holds -----------------------------------------------------------------
    Uuid BuildButton(doc::Document& d);
    Uuid BuildTextInput(doc::Document& d);
    // The one builder that is shared rather than one-per-widget: a checkbox and a radio are the
    // same widget with a different corner radius.
    Uuid BuildCheckLike(doc::Document& d, const char* name, Role role, f32 boxRadius,
                        f32 markRadius, f32 inset);
    Uuid BuildSwitch(doc::Document& d);
    Uuid BuildSlider(doc::Document& d);
    Uuid BuildDropdown(doc::Document& d);
    Uuid BuildTabs(doc::Document& d);
    Uuid BuildScroll(doc::Document& d);
    Uuid BuildList(doc::Document& d);
    Uuid BuildTable(doc::Document& d);
    Uuid BuildModal(doc::Document& d);
    Uuid BuildPopover(doc::Document& d);
    Uuid BuildToast(doc::Document& d);
    Uuid BuildRouter(doc::Document& d);
    Uuid BuildIcon(doc::Document& d);
    Uuid BuildImage(doc::Document& d);
    Uuid BuildCard(doc::Document& d);
    Uuid BuildSeparator(doc::Document& d);
    Uuid BuildSection(doc::Document& d);
    Uuid BuildAspectRatio(doc::Document& d);
    Uuid BuildGridView(doc::Document& d);
    Uuid BuildSidebar(doc::Document& d);
    Uuid BuildField(doc::Document& d);
    Uuid BuildButtonGroup(doc::Document& d);
    Uuid BuildInputGroup(doc::Document& d);
    Uuid BuildItem(doc::Document& d);
    Uuid BuildBadge(doc::Document& d);
    Uuid BuildKbd(doc::Document& d);
    Uuid BuildEmpty(doc::Document& d);
    Uuid BuildAlert(doc::Document& d);
    Uuid BuildSkeleton(doc::Document& d);
    Uuid BuildAvatar(doc::Document& d);
    Uuid BuildBreadcrumb(doc::Document& d);
    Uuid BuildToggle(doc::Document& d);
    Uuid BuildCollapsible(doc::Document& d);
    Uuid BuildAccordion(doc::Document& d);
    Uuid BuildProgress(doc::Document& d);
    Uuid BuildSpinner(doc::Document& d);
    Uuid BuildChart(doc::Document& d);
    Uuid BuildSplitter(doc::Document& d);
    Uuid BuildInputOtp(doc::Document& d);
    Uuid BuildCarousel(doc::Document& d);
    Uuid BuildCombobox(doc::Document& d);
    Uuid BuildCalendar(doc::Document& d);
    Uuid BuildTooltip(doc::Document& d);
    Uuid BuildContextMenu(doc::Document& d);
    Uuid BuildMenu(doc::Document& d);
    Uuid BuildMenubar(doc::Document& d);
    Uuid BuildNavbar(doc::Document& d);
    Uuid BuildPagination(doc::Document& d);
    Uuid BuildCommand(doc::Document& d);
    Uuid BuildHoverCard(doc::Document& d);

}
