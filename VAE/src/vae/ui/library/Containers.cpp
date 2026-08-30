// Containers, form structure and grouping: what holds the controls.
#include "vaepch.h"
#include "vae/ui/library/Catalog.h"

namespace vae::ui::catalog {

    // ------------------------------------------------------------------- containers
    // Everything from here down is composition: frames, text and the layout modes. No new
    // behavior, because none of it needs one — which is the point of having a layout engine.

    Uuid BuildCard(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Card");
        root.Lay(Stack(Axis::Column, 12.0f, Edges(16.0f), Align::Stretch))
            .Lay(Box(Size::Px(280.0f), Size::Hug()));
        Surface(root, 10.0f);
        Shadow(root, 14.0f, 0.18f);

        B header = Frame(d, root, "Header");
        header.Lay(Stack(Axis::Column, 4.0f, Edges(0.0f), Align::Stretch))
              .Lay(Box(Size::Fill(), Size::Hug()));
        Label(d, header, "Title", "Card title", "text", 15.0f)
            .Set(doc::Prop::FontWeight, 600.0f);
        Label(d, header, "Description", "What this card is about.", "textMuted", 12.0f)
            .Set(doc::Prop::TextWrap, std::string("word"));

        B body = Frame(d, root, "Body");
        body.Slot()
            .Lay(Stack(Axis::Column, 8.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Hug()));

        B footer = Frame(d, root, "Footer");
        footer.Lay(Stack(Axis::Row, 8.0f, Edges(0.0f), Align::Center, Justify::End))
              .Lay(Box(Size::Fill(), Size::Hug()));
        return d.MakeComponent(root, "Card");
    }

    Uuid BuildSeparator(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Separator");
        // A line, not a border: it is a thing in the layout with a gap either side of it, which
        // is what makes it usable inside a stack.
        root.Lay(Box(Size::Fill(), Size::Px(1.0f)))
            .Token(doc::Prop::Fill, "border");
        return d.MakeComponent(root, "Separator");
    }

    Uuid BuildSection(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Section");
        root.Lay(Stack(Axis::Column, 10.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Hug()));
        Label(d, root, "Heading", "Section", "text", 13.0f)
            .Set(doc::Prop::FontWeight, 600.0f);

        B rule = Frame(d, root, "Rule");
        rule.Lay(Box(Size::Fill(), Size::Px(1.0f))).Token(doc::Prop::Fill, "border");

        B body = Frame(d, root, "Body");
        body.Slot()
            .Lay(Stack(Axis::Column, 8.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Hug()));
        return d.MakeComponent(root, "Section");
    }

    Uuid BuildAspectRatio(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "AspectRatio");
        root.Lay(Stack(Axis::Column, 0.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Hug()))
            .Lay([](LayoutStyle& style) { style.aspectRatio = 16.0f / 9.0f; })
            .Set(doc::Prop::CornerRadius, 8.0f)
            .Set(doc::Prop::ClipContent, true)
            .Token(doc::Prop::Fill, "surfaceAlt");
        return d.MakeComponent(root, "AspectRatio");
    }

    // A gallery: the reason grid is a layout mode. Reflows on a resize rather than overflowing.
    Uuid BuildGridView(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Grid");
        // The grid *is* the slot: what you put in a gallery is the gallery. The six cells below
        // are what it looks like until someone does.
        root.Slot()
            .Lay(Grid(0, 12.0f, 160.0f))
            .Lay(Box(Size::Fill(), Size::Hug()));
        for (int i = 1; i <= 6; ++i) {
            B cell = Frame(d, root, "Cell " + std::to_string(i));
            cell.Lay(Stack(Axis::Column, 0.0f, Edges(12.0f), Align::Center, Justify::Center))
                .Lay(Box(Size::Fill(), Size::Px(88.0f)));
            Surface(cell, 8.0f, "surfaceAlt", nullptr);
            Label(d, cell, "Label", "Cell " + std::to_string(i), "textMuted", 12.0f);
        }
        return d.MakeComponent(root, "Grid");
    }

    Uuid BuildSidebar(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Sidebar");
        root.Lay(Stack(Axis::Column, 4.0f, Edges(10.0f), Align::Stretch))
            .Lay(Box(Size::Px(220.0f), Size::Fill()))
            .Token(doc::Prop::Fill, "surface")
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f);

        Label(d, root, "Heading", "Navigation", "textMuted", 11.0f)
            .Set(doc::Prop::FontWeight, 600.0f);

        B items = Frame(d, root, "Items");
        items.Slot()
             .Lay(Stack(Axis::Column, 4.0f, Edges(0.0f), Align::Stretch))
             .Lay(Box(Size::Fill(), Size::Hug()));

        for (int i = 1; i <= 4; ++i) {
            B item = Frame(d, items, "Item " + std::to_string(i));
            item.Role(Role::Button)
                .Lay(Stack(Axis::Row, 8.0f, Edges(10.0f, 7.0f), Align::Center))
                .Lay(Box(Size::Fill(), Size::Hug()))
                .Set(doc::Prop::CornerRadius, 6.0f)
                .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt")
                .On(StateBit::Selected, doc::Prop::Fill, "surfaceAlt");
            Label(d, item, "Label", "Item " + std::to_string(i), "text", 13.0f);
        }
        return d.MakeComponent(root, "Sidebar");
    }

    // ------------------------------------------------------------------- form structure

    Uuid BuildField(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Field");
        root.Lay(Stack(Axis::Column, 5.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Px(260.0f), Size::Hug()));

        Label(d, root, "Label", "Label", "text", 12.0f).Set(doc::Prop::FontWeight, 600.0f);

        // The input's slot. A designer drops the real control in here, which is why it is a
        // frame with a name rather than a Button baked into the component.
        B control = Frame(d, root, "Control");
        control.Slot()
               .Lay(Stack(Axis::Column, 0.0f, Edges(0.0f), Align::Stretch))
               .Lay(Box(Size::Fill(), Size::Hug()));

        Label(d, root, "Help", "What this field is for.", "textMuted", 11.0f)
            .Set(doc::Prop::TextWrap, std::string("word"));
        // Hidden until there is something wrong to say. A field whose error slot is always
        // visible is a field that always looks broken.
        Label(d, root, "Error", "Something is wrong.", "danger", 11.0f).Hidden();
        return d.MakeComponent(root, "Field");
    }

    // ------------------------------------------------------------------- grouping

    Uuid BuildButtonGroup(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "ButtonGroup");
        root.Slot()
            .Lay(Stack(Axis::Row, 1.0f, Edges(1.0f), Align::Stretch))
            .Lay(Box(Size::Hug(), Size::Hug()))
            .Set(doc::Prop::CornerRadius, 7.0f)
            .Token(doc::Prop::Fill, "surfaceAlt");
        for (int i = 1; i <= 3; ++i) {
            B item = Frame(d, root, "Option " + std::to_string(i));
            item.Role(Role::Tab)
                .Lay(Stack(Axis::Row, 0.0f, Edges(14.0f, 6.0f), Align::Center, Justify::Center))
                .Lay(Box(Size::Hug(), Size::Hug()))
                .Set(doc::Prop::CornerRadius, 6.0f)
                .On(StateBit::Selected, doc::Prop::Fill, "surface")
                .OnTint(StateBit::Hovered, 0.06f);
            Label(d, item, "Label", "Option " + std::to_string(i), "text", 13.0f);
        }
        return d.MakeComponent(root, "ButtonGroup");
    }

    Uuid BuildInputGroup(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "InputGroup");
        root.Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Px(300.0f), Size::Hug()));
        // The pieces butt up against each other, so the group reads as one control. Each part
        // keeps its own name, which is how a script addresses the field and the button apart.
        B field = Frame(d, root, "Field");
        field.Role(Role::TextInput)
             .Lay(Stack(Axis::Row, 0.0f, Edges(10.0f, 7.0f), Align::Center))
             .Lay(Box(Size::Fill(), Size::Px(32.0f)))
             .Set(doc::Prop::CornerRadius, 6.0f)
             .Set(doc::Prop::Placeholder, std::string("Search"))
             .Token(doc::Prop::Fill, "surface")
             .Token(doc::Prop::Stroke, "border")
             .Set(doc::Prop::StrokeWidth, 1.0f);
        Label(d, field, "Text", "", "text", 13.0f);

        B action = Frame(d, root, "Action");
        action.Role(Role::Button)
              .Lay(Stack(Axis::Row, 0.0f, Edges(12.0f, 7.0f), Align::Center, Justify::Center))
              .Lay(Box(Size::Hug(), Size::Px(32.0f)))
              .Set(doc::Prop::CornerRadius, 6.0f)
              .Token(doc::Prop::Fill, "accent")
              .OnTint(StateBit::Hovered, 0.12f)
              .OnTint(StateBit::Pressed, -0.10f);
        Label(d, action, "Label", "Go", "accentText", 13.0f);
        return d.MakeComponent(root, "InputGroup");
    }

    // A row in a list of things: an icon, a name and a description, and somewhere on the right
    // for whatever acts on it.
    Uuid BuildItem(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Item");
        root.Lay(Stack(Axis::Row, 10.0f, Edges(10.0f), Align::Center))
            .Lay(Box(Size::Fill(), Size::Hug()))
            .Set(doc::Prop::CornerRadius, 8.0f)
            .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt");

        Label(d, root, "Icon", "", "textMuted", 16.0f)
            .Set(doc::Prop::FontFamily, std::string("JetBrainsMono Nerd Font"));

        B text = Frame(d, root, "Text");
        text.Lay(Stack(Axis::Column, 2.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Hug()));
        Label(d, text, "Title", "Item", "text", 13.0f).Set(doc::Prop::FontWeight, 500.0f);
        Label(d, text, "Description", "A line about it.", "textMuted", 11.0f);

        B trailing = Frame(d, root, "Trailing");
        trailing.Slot()
                .Lay(Stack(Axis::Row, 6.0f, Edges(0.0f), Align::Center, Justify::End))
                .Lay(Box(Size::Hug(), Size::Hug()));
        return d.MakeComponent(root, "Item");
    }

}
