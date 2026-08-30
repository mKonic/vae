// The controls: what a person clicks, types into or drags.
#include "vaepch.h"
#include "vae/ui/library/Catalog.h"

namespace vae::ui::catalog {

    // ---------------------------------------------------------------------- widgets

    Uuid BuildButton(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Button");
        root.Role(Role::Button)
            .Lay(Stack(Axis::Row, 6.0f, Edges(14.0f, 8.0f), Align::Center, Justify::Center))
            .Lay(Box(Size::Hug(), Size::Px(32.0f)))
            .Set(doc::Prop::CornerRadius, 6.0f)
            .Token(doc::Prop::Fill, "accent")
            .OnTint(StateBit::Hovered, 0.12f)
            .OnTint(StateBit::Pressed, -0.10f)
            .On(StateBit::Disabled, doc::Prop::Fill, "surfaceAlt")
            .On(StateBit::Focused, doc::Prop::Stroke, "accentHover")
            .OnValue(StateBit::Focused, doc::Prop::StrokeWidth, 2.0f);
        Label(d, root, "Label", "Button", "accentText")
            .Set(doc::Prop::FontWeight, 500.0f)
            .On(StateBit::Disabled, doc::Prop::TextColor, "textMuted");
        return d.MakeComponent(root, "Button");
    }

    Uuid BuildTextInput(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "TextInput");
        root.Role(Role::TextInput)
            .Lay(Stack(Axis::Row, 0.0f, Edges(10.0f, 7.0f), Align::Center))
            .Lay(Box(Size::Px(220.0f), Size::Px(32.0f)))
            .Set(doc::Prop::CornerRadius, 6.0f)
            .Set(doc::Prop::ClipContent, true)
            .Token(doc::Prop::Fill, "surface")
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f)
            .Set(doc::Prop::Placeholder, std::string("Type here"))
            .Set(doc::Prop::Text, std::string())
            .On(StateBit::Hovered, doc::Prop::Stroke, "textMuted")
            .On(StateBit::Focused, doc::Prop::Stroke, "accent")
            .OnValue(StateBit::Focused, doc::Prop::StrokeWidth, 2.0f);
        Label(d, root, "Label", "", "text").Lay(Box(Size::Fill(), Size::Hug()));
        return d.MakeComponent(root, "TextInput");
    }

    // Checkbox, radio and switch share a shape: an indicator the behavior shows or moves, and a
    // label beside it. Only the geometry differs.
    Uuid BuildCheckLike(doc::Document& d, const char* name, Role role, f32 boxRadius,
                        f32 tickInset, f32 tickRadius) {
        B root = Frame(d, Uuid::Invalid(), name);
        root.Role(role)
            .Lay(Stack(Axis::Row, 8.0f, Edges(0.0f), Align::Center))
            .Lay(Box(Size::Hug(), Size::Hug()))
            .Set(doc::Prop::Checked, false);

        B box = Frame(d, root, "Box");
        box.Lay(Box(Size::Px(18.0f), Size::Px(18.0f)))
           .Set(doc::Prop::CornerRadius, boxRadius)
           .Token(doc::Prop::Fill, "surface")
           .Token(doc::Prop::Stroke, "border")
           .Set(doc::Prop::StrokeWidth, 1.0f);

        B tick = Frame(d, box, "Tick");
        tick.Role(Role::Indicator)
            .Lay(Box(Size::Px(18.0f - tickInset * 2.0f), Size::Px(18.0f - tickInset * 2.0f)))
            .Lay([=](LayoutStyle& style) { style.offsetStart = { tickInset, tickInset }; })
            .Set(doc::Prop::CornerRadius, tickRadius)
            .Token(doc::Prop::Fill, "accent")
            .Hidden();

        Label(d, root, "Label", name, "text");
        return d.MakeComponent(root, name);
    }

    Uuid BuildSwitch(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Switch");
        root.Role(Role::Switch)
            .Lay(Stack(Axis::Row, 8.0f, Edges(0.0f), Align::Center))
            .Lay(Box(Size::Hug(), Size::Hug()))
            .Set(doc::Prop::Checked, false);

        B track = Frame(d, root, "Track");
        track.Role(Role::Track)
             .Lay(Box(Size::Px(40.0f), Size::Px(22.0f)))
             .Set(doc::Prop::CornerRadius, 11.0f)
             .Token(doc::Prop::Fill, "surfaceAlt")
             .On(StateBit::Checked, doc::Prop::Fill, "accent");

        B knob = Frame(d, track, "Knob");
        knob.Role(Role::Knob)
            .Lay(Box(Size::Px(18.0f), Size::Px(18.0f)))
            .Lay([](LayoutStyle& style) {
                style.offsetStart = { 2.0f, 2.0f };
                style.offsetEnd = { 2.0f, 2.0f };
            })
            .Set(doc::Prop::CornerRadius, 9.0f)
            .Token(doc::Prop::Fill, "accentText");

        Label(d, root, "Label", "Switch", "text");
        return d.MakeComponent(root, "Switch");
    }

    Uuid BuildSlider(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Slider");
        root.Role(Role::Slider)
            .Lay(Box(Size::Px(200.0f), Size::Px(20.0f)))
            .Set(doc::Prop::MinValue, 0.0f)
            .Set(doc::Prop::MaxValue, 1.0f)
            .Set(doc::Prop::Value, 0.5f);

        B track = Frame(d, root, "Track");
        track.Role(Role::Track)
             .Lay(Box(Size::Fill(), Size::Px(4.0f)))
             .Lay([](LayoutStyle& style) {
                 // Center already centres; offsetStart is an offset FROM the centre, so any
                 // value here slides the track off the knob it is supposed to run through.
                 style.constraintX = Constraint::StartEnd;
                 style.constraintY = Constraint::Center;
             })
             .Set(doc::Prop::CornerRadius, 2.0f)
             .Token(doc::Prop::Fill, "surfaceAlt");

        B fill = Frame(d, track, "Fill");
        fill.Role(Role::Fill)
            .Lay(Box(Size::Percent(0.5f), Size::Fill()))
            .Set(doc::Prop::CornerRadius, 2.0f)
            .Token(doc::Prop::Fill, "accent");

        B knob = Frame(d, root, "Knob");
        knob.Role(Role::Knob)
            .Lay(Box(Size::Px(16.0f), Size::Px(16.0f)))
            .Lay([](LayoutStyle& style) { style.constraintY = Constraint::Center; })
            .Set(doc::Prop::CornerRadius, 8.0f)
            .Token(doc::Prop::Fill, "accentText")
            .Token(doc::Prop::Stroke, "accent")
            .Set(doc::Prop::StrokeWidth, 2.0f)
            .On(StateBit::Pressed, doc::Prop::Stroke, "accentActive");
        return d.MakeComponent(root, "Slider");
    }

    Uuid BuildDropdown(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Dropdown");
        root.Role(Role::Dropdown)
            .Lay(Stack(Axis::Row, 8.0f, Edges(10.0f, 6.0f), Align::Center, Justify::SpaceBetween))
            .Lay(Box(Size::Px(180.0f), Size::Px(32.0f)))
            .Set(doc::Prop::CornerRadius, 6.0f)
            .Token(doc::Prop::Fill, "surface")
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f)
            .Set(doc::Prop::SelectedIndex, -1.0f)
            .On(StateBit::Hovered, doc::Prop::Stroke, "textMuted")
            .On(StateBit::Open, doc::Prop::Stroke, "accent");

        Label(d, root, "Label", "Select...", "text").Lay(Box(Size::Fill(), Size::Hug()));
        Label(d, root, "Chevron", "▾", "textMuted");

        B menu = Frame(d, root, "Menu");
        menu.Role(Role::Content)
            .Lay(Stack(Axis::Column, 2.0f, Edges(4.0f), Align::Stretch))
            .Lay(Box(Size::Px(180.0f), Size::Hug()))
            .Set(doc::Prop::CornerRadius, 6.0f)
            .Token(doc::Prop::Fill, "surface")
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f);
        Shadow(menu, 18.0f, 0.40f);

        for (int i = 1; i <= 3; ++i) {
            B item = Frame(d, menu, "Item " + std::to_string(i));
            item.Role(Role::DropdownItem)
                .Lay(Stack(Axis::Row, 0.0f, Edges(8.0f, 6.0f), Align::Center))
                .Lay(Box(Size::Fill(), Size::Hug()))
                .Set(doc::Prop::CornerRadius, 4.0f)
                .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt");
            Label(d, item, "Label", "Option " + std::to_string(i), "text");
        }
        return d.MakeComponent(root, "Dropdown");
    }

    Uuid BuildTabs(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Tabs");
        root.Role(Role::Tabs)
            .Lay(Stack(Axis::Column, 8.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Hug()))
            .Set(doc::Prop::SelectedIndex, 0.0f);

        B strip = Frame(d, root, "Strip");
        strip.Lay(Stack(Axis::Row, 4.0f, Edges(0.0f), Align::Center))
             .Lay(Box(Size::Hug(), Size::Hug()));

        for (int i = 1; i <= 3; ++i) {
            B tab = Frame(d, strip, "Tab " + std::to_string(i));
            tab.Role(Role::Tab)
               .Lay(Stack(Axis::Row, 0.0f, Edges(12.0f, 8.0f), Align::Center))
               .Lay(Box(Size::Hug(), Size::Hug()))
               .Set(doc::Prop::CornerRadius, 6.0f)
               .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt")
               .On(StateBit::Selected, doc::Prop::Fill, "surface");
            Label(d, tab, "Label", "Tab " + std::to_string(i), "text");
        }

        for (int i = 1; i <= 3; ++i) {
            B panel = Frame(d, root, "Panel " + std::to_string(i));
            panel.Role(Role::Content)
                 .Lay(Stack(Axis::Column, 8.0f, Edges(12.0f), Align::Start))
                 .Lay(Box(Size::Fill(), Size::Hug()))
                 .Set(doc::Prop::CornerRadius, 6.0f)
                 .Token(doc::Prop::Fill, "surface");
            Label(d, panel, "Body", "Panel " + std::to_string(i), "textMuted");
        }
        return d.MakeComponent(root, "Tabs");
    }

    // Adds a vertical scrollbar to `parent` and returns its track, which is also what tells
    // ContentSize to ignore it.
    Uuid AddScrollbar(doc::Document& d, Uuid parent) {
        B bar = Frame(d, parent, "Scrollbar");
        bar.Role(Role::Track)
           .Lay(Box(Size::Px(8.0f), Size::Fill()))
           .Lay([](LayoutStyle& style) {
               style.constraintX = Constraint::End;
               style.constraintY = Constraint::StartEnd;
               style.offsetStart = { 0.0f, 4.0f };
               style.offsetEnd = { 4.0f, 4.0f };
           });

        B thumb = Frame(d, bar, "Thumb");
        thumb.Role(Role::Thumb)
             .Lay(Box(Size::Fill(), Size::Px(48.0f)))
             .Set(doc::Prop::CornerRadius, 4.0f)
             .Token(doc::Prop::Fill, "border")
             .On(StateBit::Hovered, doc::Prop::Fill, "textMuted")
             .On(StateBit::Pressed, doc::Prop::Fill, "accent");
        return bar;
    }

    Uuid BuildScroll(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Scroll");
        root.Role(Role::Scroll)
            .Lay(Box(Size::Fill(), Size::Px(220.0f)))
            .Set(doc::Prop::ClipContent, true)
            .Set(doc::Prop::CornerRadius, 6.0f)
            .Token(doc::Prop::Fill, "surface");

        B content = Frame(d, root, "Content");
        content.Role(Role::Content).Slot()
               .Lay(Stack(Axis::Column, 6.0f, Edges(10.0f), Align::Stretch))
               .Lay(Box(Size::Fill(), Size::Hug()));

        AddScrollbar(d, root);
        return d.MakeComponent(root, "Scroll");
    }

    Uuid BuildList(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "List");
        root.Role(Role::List)
            .Lay(Box(Size::Fill(), Size::Px(240.0f)))
            .Set(doc::Prop::ClipContent, true)
            .Set(doc::Prop::CornerRadius, 6.0f)
            .Set(doc::Prop::ItemHeight, 28.0f)
            .Set(doc::Prop::ItemCount, 0.0f)
            .Set(doc::Prop::SelectedIndex, -1.0f)
            .Token(doc::Prop::Fill, "surface");

        // The one row that exists. Every visible row is drawn from it, which is why a list of a
        // million entries costs the same as a list of ten.
        B row = Frame(d, root, "Row");
        row.Role(Role::ListItem)
           .Lay(Stack(Axis::Row, 0.0f, Edges(8.0f, 4.0f), Align::Center))
           .Lay(Box(Size::Fill(), Size::Px(28.0f)))
           .Token(doc::Prop::TextColor, "text")
           .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt")
           .On(StateBit::Selected, doc::Prop::Fill, "accent");

        AddScrollbar(d, root);
        return d.MakeComponent(root, "List");
    }

    Uuid BuildTable(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Table");
        root.Role(Role::Table)
            .Lay(Box(Size::Fill(), Size::Px(240.0f)))
            .Set(doc::Prop::ClipContent, true)
            .Set(doc::Prop::CornerRadius, 6.0f)
            .Set(doc::Prop::ItemHeight, 26.0f)
            .Set(doc::Prop::ItemCount, 0.0f)
            .Set(doc::Prop::SelectedIndex, -1.0f)
            .Token(doc::Prop::Fill, "surface");

        B header = Frame(d, root, "Header");
        header.Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Stretch))
              .Lay(Box(Size::Fill(), Size::Px(28.0f)))
              .Token(doc::Prop::Fill, "surfaceAlt");

        const char* names[] = { "Name", "Type", "Size" };
        const f32 widths[] = { 200.0f, 120.0f, 100.0f };
        for (int i = 0; i < 3; ++i) {
            B column = Frame(d, header, std::string("Column ") + names[i]);
            column.Role(Role::TableColumn)
                  .Lay(Stack(Axis::Row, 0.0f, Edges(8.0f, 4.0f), Align::Center))
                  .Lay(Box(Size::Px(widths[i]), Size::Fill()))
                  .Set(doc::Prop::ColumnWidth, widths[i]);
            Label(d, column, "Label", names[i], "textMuted", 12.0f)
                .Set(doc::Prop::FontWeight, 600.0f);
        }

        B row = Frame(d, root, "Row");
        row.Role(Role::ListItem)
           .Lay(Box(Size::Fill(), Size::Px(26.0f)))
           .Token(doc::Prop::TextColor, "text")
           .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt")
           .On(StateBit::Selected, doc::Prop::Fill, "accent");

        AddScrollbar(d, root);
        return d.MakeComponent(root, "Table");
    }

    Uuid BuildModal(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Modal");
        root.Lay(Box(Size::Fill(), Size::Fill()));

        B scrim = Frame(d, root, "Scrim");
        scrim.Role(Role::Scrim)
             .Lay(Box(Size::Fill(), Size::Fill()))
             .Token(doc::Prop::Fill, "scrim");

        B dialog = Frame(d, root, "Dialog");
        dialog.Role(Role::Modal)
              .Lay(Stack(Axis::Column, 12.0f, Edges(20.0f), Align::Stretch))
              .Lay(Box(Size::Px(380.0f), Size::Hug()))
              .Lay([](LayoutStyle& style) {
                  style.constraintX = Constraint::Center;
                  style.constraintY = Constraint::Center;
              })
              .Set(doc::Prop::CornerRadius, 10.0f)
              .Token(doc::Prop::Fill, "surface");
        Shadow(dialog, 32.0f, 0.45f);

        Label(d, dialog, "Title", "Are you sure?", "text", 17.0f)
            .Set(doc::Prop::FontWeight, 600.0f);
        Label(d, dialog, "Message", "This cannot be undone.", "textMuted")
            .Set(doc::Prop::TextWrap, std::string("word"));

        B actions = Frame(d, dialog, "Actions");
        actions.Lay(Stack(Axis::Row, 8.0f, Edges(0.0f), Align::Center, Justify::End))
               .Lay(Box(Size::Fill(), Size::Hug()));
        return d.MakeComponent(root, "Modal");
    }

    Uuid BuildPopover(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Popover");
        root.Role(Role::Popover)
            .Lay(Stack(Axis::Column, 6.0f, Edges(10.0f), Align::Stretch))
            .Lay(Box(Size::Hug(), Size::Hug()))
            .Set(doc::Prop::CornerRadius, 8.0f)
            .Token(doc::Prop::Fill, "surface")
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f);
        Shadow(root, 22.0f, 0.40f);
        Label(d, root, "Body", "Popover", "text");
        return d.MakeComponent(root, "Popover");
    }

    Uuid BuildToast(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Toast");
        root.Role(Role::Toast)
            .Lay(Stack(Axis::Row, 10.0f, Edges(14.0f, 10.0f), Align::Center))
            .Lay(Box(Size::Hug(), Size::Hug()))
            .Set(doc::Prop::CornerRadius, 8.0f)
            .Set(doc::Prop::Duration, 4.0f)
            .Token(doc::Prop::Fill, "surfaceAlt");
        Shadow(root, 20.0f, 0.40f);
        Label(d, root, "Message", "Saved", "text");
        return d.MakeComponent(root, "Toast");
    }

    Uuid BuildRouter(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Router");
        root.Role(Role::Router).Lay(Box(Size::Fill(), Size::Fill()));
        return d.MakeComponent(root, "Router");
    }

    Uuid BuildIcon(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Icon");
        root.Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Center, Justify::Center))
            .Lay(Box(Size::Px(20.0f), Size::Px(20.0f)));
        // An icon is text in an icon font, not a special node kind. That is what makes any Nerd
        // Font glyph usable the moment the family is installed.
        Label(d, root, "Glyph", "", "text", 16.0f)
            .Set(doc::Prop::FontFamily, std::string("JetBrainsMono Nerd Font"));
        return d.MakeComponent(root, "Icon");
    }

    Uuid BuildImage(doc::Document& d) {
        B root = B(d, doc::NodeKind::Image, Uuid::Invalid(), "Image");
        root.Lay(Box(Size::Px(160.0f), Size::Px(120.0f)))
            .Set(doc::Prop::CornerRadius, 6.0f)
            .Set(doc::Prop::ImageFit, std::string("cover"))
            .Token(doc::Prop::Fill, "surfaceAlt");
        return d.MakeComponent(root, "Image");
    }

}
