// The composites: split panes, richer inputs, and everything a pointer opens.
#include "vaepch.h"
#include "vae/ui/library/Catalog.h"

namespace vae::ui::catalog {

    // ------------------------------------------------------------------- split

    Uuid BuildSplitter(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Splitter");
        root.Role(Role::Splitter)
            .Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Px(180.0f)))
            .Set(doc::Prop::CornerRadius, 8.0f)
            .Set(doc::Prop::ClipContent, true)
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f)
            // A fraction, not a width: the split has to mean the same thing after a resize.
            .Set(doc::Prop::Value, 0.4f)
            .Set(doc::Prop::MinValue, 0.15f)
            .Set(doc::Prop::MaxValue, 0.85f);

        B left = Frame(d, root, "Start");
        left.Lay(Stack(Axis::Column, 0.0f, Edges(12.0f), Align::Center, Justify::Center))
            .Lay(Box(Size::Percent(0.4f), Size::Fill()))
            .Token(doc::Prop::Fill, "surface");
        Label(d, left, "Label", "Start", "textMuted", 12.0f);

        B divider = Frame(d, root, "Divider");
        divider.Role(Role::Knob)
               .Lay(Box(Size::Px(6.0f), Size::Fill()))
               .Token(doc::Prop::Fill, "border")
               .On(StateBit::Pressed, doc::Prop::Fill, "accent");

        B right = Frame(d, root, "End");
        right.Lay(Stack(Axis::Column, 0.0f, Edges(12.0f), Align::Center, Justify::Center))
             .Lay(Box(Size::Fill(), Size::Fill()))
             .Token(doc::Prop::Fill, "surfaceAlt");
        Label(d, right, "Label", "End", "textMuted", 12.0f);
        return d.MakeComponent(root, "Splitter");
    }

    // ------------------------------------------------------------------- richer input

    Uuid BuildInputOtp(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "InputOtp");
        root.Role(Role::InputOtp)
            .Lay(Stack(Axis::Row, 8.0f, Edges(0.0f), Align::Center))
            .Lay(Box(Size::Hug(), Size::Hug()));
        for (int i = 1; i <= 6; ++i) {
            B box = Frame(d, root, "Box " + std::to_string(i));
            box.Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Center, Justify::Center))
               .Lay(Box(Size::Px(38.0f), Size::Px(44.0f)))
               .Set(doc::Prop::CornerRadius, 6.0f)
               .Token(doc::Prop::Fill, "surface")
               .Token(doc::Prop::Stroke, "border")
               .Set(doc::Prop::StrokeWidth, 1.0f)
               .On(StateBit::Selected, doc::Prop::Stroke, "accent")
               .OnValue(StateBit::Selected, doc::Prop::StrokeWidth, 2.0f);
            Label(d, box, "Digit", "", "text", 18.0f).Set(doc::Prop::FontWeight, 600.0f);
        }
        return d.MakeComponent(root, "InputOtp");
    }

    Uuid BuildCarousel(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Carousel");
        root.Role(Role::Carousel)
            .Lay(Stack(Axis::Row, 8.0f, Edges(0.0f), Align::Center))
            .Lay(Box(Size::Fill(), Size::Px(180.0f)))
            .Set(doc::Prop::SelectedIndex, 0.0f);

        const auto arrow = [&](const char* name, const char* glyph) {
            B step = Frame(d, root, name);
            step.Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Center, Justify::Center))
                .Lay(Box(Size::Px(30.0f), Size::Px(30.0f)))
                .Set(doc::Prop::CornerRadius, 999.0f)
                .Token(doc::Prop::Fill, "surfaceAlt")
                .OnTint(StateBit::Hovered, 0.10f)
                .OnValue(StateBit::Disabled, doc::Prop::Opacity, 0.35f);
            Label(d, step, "Label", glyph, "text", 13.0f);
        };
        arrow("Prev", "‹");

        // The viewport clips and the track slides inside it. Absolute, because an offset is
        // only an offset in a parent that honours one.
        B viewport = Frame(d, root, "Viewport");
        viewport.Lay([](LayoutStyle& style) { style.mode = LayoutMode::Absolute; })
                .Lay(Box(Size::Fill(), Size::Fill()))
                .Set(doc::Prop::CornerRadius, 8.0f)
                .Set(doc::Prop::ClipContent, true);

        B track = Frame(d, viewport, "Track");
        track.Role(Role::Content)
             .Lay(Stack(Axis::Row, 12.0f, Edges(0.0f), Align::Stretch))
             .Lay(Box(Size::Hug(), Size::Fill()));
        for (int i = 1; i <= 3; ++i) {
            B slide = Frame(d, track, "Slide " + std::to_string(i));
            slide.Lay(Stack(Axis::Column, 0.0f, Edges(16.0f), Align::Center, Justify::Center))
                 .Lay(Box(Size::Px(320.0f), Size::Fill()))
                 .Set(doc::Prop::CornerRadius, 8.0f)
                 .Token(doc::Prop::Fill, "surfaceAlt");
            Label(d, slide, "Label", "Slide " + std::to_string(i), "textMuted", 13.0f);
        }
        arrow("Next", "›");
        return d.MakeComponent(root, "Carousel");
    }

    Uuid BuildCombobox(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Combobox");
        root.Role(Role::Combobox)
            .Lay(Stack(Axis::Row, 6.0f, Edges(0.0f), Align::Center))
            .Lay(Box(Size::Px(240.0f), Size::Hug()))
            .Set(doc::Prop::SelectedIndex, -1.0f);

        B field = Frame(d, root, "Field");
        field.Role(Role::TextInput)
             .Lay(Stack(Axis::Row, 8.0f, Edges(10.0f, 7.0f), Align::Center))
             .Lay(Box(Size::Fill(), Size::Px(32.0f)))
             .Set(doc::Prop::CornerRadius, 6.0f)
             .Set(doc::Prop::Placeholder, std::string("Search a framework..."))
             .Token(doc::Prop::Fill, "surface")
             .Token(doc::Prop::Stroke, "border")
             .Set(doc::Prop::StrokeWidth, 1.0f)
             .On(StateBit::Focused, doc::Prop::Stroke, "accent");
        Label(d, field, "Label", "", "text", 13.0f).Lay(Box(Size::Fill(), Size::Hug()));
        Label(d, field, "Chevron", "▾", "textMuted", 11.0f);

        B menu = Frame(d, root, "Menu");
        menu.Role(Role::Content)
            .Lay(Stack(Axis::Column, 2.0f, Edges(4.0f), Align::Stretch))
            .Lay(Box(Size::Px(240.0f), Size::Hug()))
            .Set(doc::Prop::CornerRadius, 6.0f)
            .Token(doc::Prop::Fill, "surface")
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f);
        Shadow(menu, 18.0f, 0.40f);

        for (const char* name : { "Next.js", "SvelteKit", "Nuxt.js", "Remix", "Astro" }) {
            B item = Frame(d, menu, name);
            item.Role(Role::DropdownItem)
                .Lay(Stack(Axis::Row, 0.0f, Edges(8.0f, 6.0f), Align::Center))
                .Lay(Box(Size::Fill(), Size::Hug()))
                .Set(doc::Prop::CornerRadius, 4.0f)
                .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt");
            Label(d, item, "Label", name, "text", 13.0f);
        }
        // Shown only when the typing matches nothing, which is most of the time it is used.
        Label(d, menu, "Empty", "No matches", "textMuted", 12.0f).Hidden();
        return d.MakeComponent(root, "Combobox");
    }

    Uuid BuildCalendar(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Calendar");
        root.Role(Role::Calendar)
            .Lay(Stack(Axis::Column, 8.0f, Edges(12.0f), Align::Stretch))
            .Lay(Box(Size::Px(280.0f), Size::Hug()));
        Surface(root, 10.0f);

        B header = Frame(d, root, "Header");
        header.Lay(Stack(Axis::Row, 8.0f, Edges(0.0f), Align::Center, Justify::SpaceBetween))
              .Lay(Box(Size::Fill(), Size::Hug()));
        const auto arrow = [&](const char* name, const char* glyph) {
            B step = Frame(d, header, name);
            step.Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Center, Justify::Center))
                .Lay(Box(Size::Px(26.0f), Size::Px(26.0f)))
                .Set(doc::Prop::CornerRadius, 6.0f)
                .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt");
            Label(d, step, "Label", glyph, "textMuted", 12.0f);
        };
        arrow("Prev", "‹");
        B title = Frame(d, header, "Title");
        title.Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Center, Justify::Center))
             .Lay(Box(Size::Fill(), Size::Hug()));
        Label(d, title, "Label", "Month", "text", 13.0f).Set(doc::Prop::FontWeight, 600.0f);
        arrow("Next", "›");

        B week = Frame(d, root, "Weekdays");
        week.Lay(Grid(7, 2.0f, 24.0f)).Lay(Box(Size::Fill(), Size::Hug()));
        for (const char* day : { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" }) {
            B cell = Frame(d, week, day);
            cell.Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Center, Justify::Center))
                .Lay(Box(Size::Fill(), Size::Px(20.0f)));
            Label(d, cell, "Label", day, "textMuted", 11.0f);
        }

        // Six rows of seven, always: a month that needs five rows must not resize the popover
        // it is sitting in when the next one needs six.
        B days = Frame(d, root, "Days");
        days.Role(Role::Content).Lay(Grid(7, 2.0f, 24.0f)).Lay(Box(Size::Fill(), Size::Hug()));
        for (int i = 1; i <= 42; ++i) {
            B cell = Frame(d, days, "Day " + std::to_string(i));
            cell.Role(Role::Tab)
                .Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Center, Justify::Center))
                .Lay(Box(Size::Fill(), Size::Px(30.0f)))
                .Set(doc::Prop::CornerRadius, 6.0f)
                .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt")
                .On(StateBit::Checked, doc::Prop::Stroke, "border")
                .OnValue(StateBit::Checked, doc::Prop::StrokeWidth, 1.0f)
                .On(StateBit::Selected, doc::Prop::Fill, "accent")
                .OnValue(StateBit::Disabled, doc::Prop::Opacity, 0.0f);
            Label(d, cell, "Label", "", "text", 12.0f)
                .On(StateBit::Selected, doc::Prop::TextColor, "accentText");
        }
        return d.MakeComponent(root, "Calendar");
    }

    // ------------------------------------------------------------------- pointer-opened

    // The bubble is a Content child, exactly as a dropdown's menu is: hidden in place, floated
    // when it opens. So the tooltip a designer styles and the one that appears are one node.
    void AddBubble(doc::Document& d, Uuid parent, const std::string& text) {
        B bubble = Frame(d, parent, "Bubble");
        bubble.Role(Role::Content)
              .Lay(Stack(Axis::Row, 0.0f, Edges(8.0f, 5.0f), Align::Center))
              .Lay(Box(Size::Hug(), Size::Hug()))
              .Set(doc::Prop::CornerRadius, 6.0f)
              .Token(doc::Prop::Fill, "text");
        Shadow(bubble, 14.0f, 0.35f);
        Label(d, bubble, "Label", text, "bg", 12.0f);
    }

    Uuid BuildTooltip(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Tooltip");
        root.Role(Role::Tooltip)
            .Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Center, Justify::Center))
            .Lay(Box(Size::Hug(), Size::Hug()))
            // Seconds of dwell before it shows. Instant tooltips fire while you cross the
            // screen; slow ones never arrive.
            .Set(doc::Prop::Duration, 0.45f);

        B trigger = Frame(d, root, "Trigger");
        trigger.Lay(Stack(Axis::Row, 0.0f, Edges(12.0f, 7.0f), Align::Center, Justify::Center))
               .Lay(Box(Size::Hug(), Size::Hug()))
               .Set(doc::Prop::CornerRadius, 6.0f)
               .Token(doc::Prop::Fill, "surfaceAlt");
        Label(d, trigger, "Label", "Hover me", "text", 13.0f);

        AddBubble(d, root, "What this does");
        return d.MakeComponent(root, "Tooltip");
    }

    Uuid BuildContextMenu(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "ContextMenu");
        root.Role(Role::ContextMenu)
            .Lay(Stack(Axis::Column, 0.0f, Edges(16.0f), Align::Center, Justify::Center))
            .Lay(Box(Size::Fill(), Size::Px(90.0f)))
            .Set(doc::Prop::CornerRadius, 8.0f)
            .Token(doc::Prop::Fill, "surface")
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f);
        Label(d, root, "Hint", "Right-click here", "textMuted", 12.0f);

        B menu = Frame(d, root, "Menu");
        menu.Role(Role::Content)
            .Lay(Stack(Axis::Column, 2.0f, Edges(4.0f), Align::Stretch))
            .Lay(Box(Size::Px(180.0f), Size::Hug()))
            .Set(doc::Prop::CornerRadius, 6.0f)
            .Token(doc::Prop::Fill, "surface")
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f);
        Shadow(menu, 18.0f, 0.40f);

        const char* names[] = { "Cut", "Copy", "Paste" };
        const char* keys[]  = { "Ctrl+X", "Ctrl+C", "Ctrl+V" };
        for (int i = 0; i < 3; ++i) {
            B item = Frame(d, menu, std::string(names[i]));
            item.Role(Role::DropdownItem)
                .Lay(Stack(Axis::Row, 12.0f, Edges(8.0f, 6.0f), Align::Center,
                           Justify::SpaceBetween))
                .Lay(Box(Size::Fill(), Size::Hug()))
                .Set(doc::Prop::CornerRadius, 4.0f)
                .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt");
            Label(d, item, "Label", names[i], "text", 13.0f);
            Label(d, item, "Shortcut", keys[i], "textMuted", 11.0f);
        }
        return d.MakeComponent(root, "ContextMenu");
    }

    // ------------------------------------------------------------------- menus

    // The menu surface a Menu, a Menubar and a ContextMenu all open. Items are actions: each
    // reports itself by name when chosen, and can carry a `goTo` instead of a script.
    Uuid AddMenuSurface(doc::Document& d, Uuid parent,
                        const std::vector<std::pair<std::string, std::string>>& items) {
        B menu = Frame(d, parent, "Menu");
        menu.Role(Role::Content)
            .Lay(Stack(Axis::Column, 2.0f, Edges(4.0f), Align::Stretch))
            .Lay(Box(Size::Px(200.0f), Size::Hug()))
            .Set(doc::Prop::CornerRadius, 6.0f)
            .Token(doc::Prop::Fill, "surface")
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f);
        Shadow(menu, 18.0f, 0.40f);

        for (const auto& [name, shortcut] : items) {
            B item = Frame(d, menu, name);
            item.Role(Role::DropdownItem)
                .Lay(Stack(Axis::Row, 12.0f, Edges(8.0f, 6.0f), Align::Center,
                           Justify::SpaceBetween))
                .Lay(Box(Size::Fill(), Size::Hug()))
                .Set(doc::Prop::CornerRadius, 4.0f)
                .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt");
            Label(d, item, "Label", name, "text", 13.0f);
            if (!shortcut.empty()) Label(d, item, "Shortcut", shortcut, "textMuted", 11.0f);
        }
        return menu;
    }

    B AddMenu(doc::Document& d, Uuid parent, const std::string& title,
              const std::vector<std::pair<std::string, std::string>>& items) {
        B root = Frame(d, parent, title);
        root.Role(Role::Menu)
            .Lay(Stack(Axis::Row, 6.0f, Edges(10.0f, 6.0f), Align::Center))
            .Lay(Box(Size::Hug(), Size::Px(30.0f)))
            .Set(doc::Prop::CornerRadius, 6.0f)
            .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt")
            .On(StateBit::Open, doc::Prop::Fill, "surfaceAlt");
        Label(d, root, "Label", title, "text", 13.0f);
        AddMenuSurface(d, root, items);
        return root;
    }

    Uuid BuildMenu(doc::Document& d) {
        B root = AddMenu(d, Uuid::Invalid(), "Menu",
                         { { "New", "Ctrl+N" }, { "Open", "Ctrl+O" }, { "Save", "Ctrl+S" } });
        // A menu of actions looks like a button, unlike a select, which looks like a field.
        root.Token(doc::Prop::Fill, "surfaceAlt")
            .Lay(Stack(Axis::Row, 6.0f, Edges(12.0f, 6.0f), Align::Center));
        Label(d, root, "Chevron", "▾", "textMuted", 11.0f);
        return d.MakeComponent(root, "Menu");
    }

    Uuid BuildMenubar(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Menubar");
        root.Lay(Stack(Axis::Row, 2.0f, Edges(4.0f), Align::Center))
            .Lay(Box(Size::Fill(), Size::Hug()));
        Surface(root, 8.0f);

        AddMenu(d, root, "File", { { "New", "Ctrl+N" }, { "Open", "Ctrl+O" },
                                   { "Save", "Ctrl+S" }, { "Quit", "Ctrl+Q" } });
        AddMenu(d, root, "Edit", { { "Undo", "Ctrl+Z" }, { "Redo", "Ctrl+Y" },
                                   { "Find", "Ctrl+F" } });
        AddMenu(d, root, "View", { { "Zoom in", "Ctrl++" }, { "Zoom out", "Ctrl+-" },
                                   { "Reset", "Ctrl+0" } });
        return d.MakeComponent(root, "Menubar");
    }

    // The bar across the top of a page: where you are, where you can go, and the one action
    // that belongs to the whole app. Links carry `goTo`, so navigating needs no script.
    Uuid BuildNavbar(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Navbar");
        root.Lay(Stack(Axis::Row, 16.0f, Edges(16.0f, 10.0f), Align::Center,
                       Justify::SpaceBetween))
            .Lay(Box(Size::Fill(), Size::Hug()))
            .Token(doc::Prop::Fill, "surface")
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f);

        B brand = Frame(d, root, "Brand");
        brand.Lay(Stack(Axis::Row, 8.0f, Edges(0.0f), Align::Center))
             .Lay(Box(Size::Hug(), Size::Hug()));
        Label(d, brand, "Mark", "", "accent", 16.0f)
             .Set(doc::Prop::FontFamily, std::string("JetBrainsMono Nerd Font"));
        Label(d, brand, "Name", "Acme", "text", 15.0f).Set(doc::Prop::FontWeight, 600.0f);

        B links = Frame(d, root, "Links");
        links.Lay(Stack(Axis::Row, 4.0f, Edges(0.0f), Align::Center))
             .Lay(Box(Size::Fill(), Size::Hug()));
        const char* pages[] = { "Home", "Projects", "Team" };
        for (int i = 0; i < 3; ++i) {
            B link = Frame(d, links, pages[i]);
            link.Role(Role::Button)
                .Lay(Stack(Axis::Row, 0.0f, Edges(10.0f, 6.0f), Align::Center))
                .Lay(Box(Size::Hug(), Size::Hug()))
                .Set(doc::Prop::CornerRadius, 6.0f)
                .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt")
                .On(StateBit::Selected, doc::Prop::Fill, "surfaceAlt");
            Label(d, link, "Label", pages[i], i == 0 ? "text" : "textMuted", 13.0f);
        }

        B action = Frame(d, root, "Action");
        action.Role(Role::Button)
              .Lay(Stack(Axis::Row, 0.0f, Edges(14.0f, 7.0f), Align::Center, Justify::Center))
              .Lay(Box(Size::Hug(), Size::Hug()))
              .Set(doc::Prop::CornerRadius, 6.0f)
              .Token(doc::Prop::Fill, "accent")
              .OnTint(StateBit::Hovered, 0.12f)
              .OnTint(StateBit::Pressed, -0.10f);
        Label(d, action, "Label", "Sign in", "accentText", 13.0f);
        return d.MakeComponent(root, "Navbar");
    }

    Uuid BuildPagination(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Pagination");
        root.Role(Role::Pagination)
            .Lay(Stack(Axis::Row, 4.0f, Edges(0.0f), Align::Center))
            .Lay(Box(Size::Hug(), Size::Hug()))
            .Set(doc::Prop::Value, 1.0f)
            // Zero pages stated means "however many buttons there are", so adding a page is
            // adding a button and nothing else.
            .Set(doc::Prop::MaxValue, 0.0f);

        const auto arrow = [&](const char* name, const char* glyph) {
            B step = Frame(d, root, name);
            step.Lay(Stack(Axis::Row, 0.0f, Edges(10.0f, 6.0f), Align::Center, Justify::Center))
                .Lay(Box(Size::Hug(), Size::Px(30.0f)))
                .Set(doc::Prop::CornerRadius, 6.0f)
                .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt")
                .OnValue(StateBit::Disabled, doc::Prop::Opacity, 0.4f);
            Label(d, step, "Label", glyph, "textMuted", 12.0f);
        };
        arrow("Prev", "‹");
        for (int i = 1; i <= 3; ++i) {
            B page = Frame(d, root, "Page " + std::to_string(i));
            page.Role(Role::Tab)
                .Lay(Stack(Axis::Row, 0.0f, Edges(11.0f, 6.0f), Align::Center, Justify::Center))
                .Lay(Box(Size::Hug(), Size::Px(30.0f)))
                .Set(doc::Prop::CornerRadius, 6.0f)
                .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt")
                .On(StateBit::Selected, doc::Prop::Fill, "accent");
            Label(d, page, "Label", std::to_string(i), "text", 13.0f)
                .On(StateBit::Selected, doc::Prop::TextColor, "accentText");
        }
        arrow("Next", "›");
        return d.MakeComponent(root, "Pagination");
    }

    // ⌘K. The filtering belongs to a script or a data source — this is the surface it draws on,
    // which is a search field, a list of what matched, and the shortcut that ran it.
    Uuid BuildCommand(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Command");
        root.Lay(Stack(Axis::Column, 0.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Px(420.0f), Size::Hug()))
            .Set(doc::Prop::ClipContent, true);
        Surface(root, 10.0f);
        Shadow(root, 26.0f, 0.45f);

        B search = Frame(d, root, "Search");
        search.Role(Role::TextInput)
              .Lay(Stack(Axis::Row, 8.0f, Edges(12.0f, 10.0f), Align::Center))
              .Lay(Box(Size::Fill(), Size::Hug()))
              .Set(doc::Prop::Placeholder, std::string("Type a command..."));
        Label(d, search, "Icon", "", "textMuted", 13.0f)
              .Set(doc::Prop::FontFamily, std::string("JetBrainsMono Nerd Font"));
        // Named "Label" on purpose: a field shows its text on the node called that, and the
        // fallback — the first text node in the widget — would be the search icon.
        Label(d, search, "Label", "", "text", 14.0f).Lay(Box(Size::Fill(), Size::Hug()));

        B rule = Frame(d, root, "Rule");
        rule.Lay(Box(Size::Fill(), Size::Px(1.0f))).Token(doc::Prop::Fill, "border");

        B results = Frame(d, root, "Results");
        results.Slot()
               .Lay(Stack(Axis::Column, 2.0f, Edges(6.0f), Align::Stretch))
               .Lay(Box(Size::Fill(), Size::Hug()));
        const char* names[] = { "New project", "Open recent", "Toggle theme" };
        const char* keys[]  = { "Ctrl+N", "Ctrl+R", "Ctrl+T" };
        for (int i = 0; i < 3; ++i) {
            B item = Frame(d, results, names[i]);
            item.Role(Role::DropdownItem)
                .Lay(Stack(Axis::Row, 10.0f, Edges(8.0f, 7.0f), Align::Center,
                           Justify::SpaceBetween))
                .Lay(Box(Size::Fill(), Size::Hug()))
                .Set(doc::Prop::CornerRadius, 6.0f)
                .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt");
            Label(d, item, "Label", names[i], "text", 13.0f);
            Label(d, item, "Shortcut", keys[i], "textMuted", 11.0f);
        }
        // The state a palette is in most of the time someone is typing.
        Label(d, results, "Empty", "No matches", "textMuted", 12.0f).Hidden();
        return d.MakeComponent(root, "Command");
    }

    // A tooltip with room to say something. Same dwell behavior, a longer wait and a card
    // instead of a strip — which is exactly the distinction the web draws.
    Uuid BuildHoverCard(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "HoverCard");
        root.Role(Role::Tooltip)
            .Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Center, Justify::Center))
            .Lay(Box(Size::Hug(), Size::Hug()))
            .Set(doc::Prop::Duration, 0.6f);

        B trigger = Frame(d, root, "Trigger");
        trigger.Lay(Stack(Axis::Row, 0.0f, Edges(4.0f, 2.0f), Align::Center))
               .Lay(Box(Size::Hug(), Size::Hug()))
               .Set(doc::Prop::CornerRadius, 4.0f)
               .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt");
        Label(d, trigger, "Label", "@acme", "accent", 13.0f).Set(doc::Prop::FontWeight, 500.0f);

        B card = Frame(d, root, "Card");
        card.Role(Role::Content)
            .Lay(Stack(Axis::Row, 10.0f, Edges(12.0f), Align::Start))
            .Lay(Box(Size::Px(280.0f), Size::Hug()));
        Surface(card, 10.0f);
        Shadow(card, 20.0f, 0.40f);

        B avatar = Frame(d, card, "Avatar");
        avatar.Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Center, Justify::Center))
              .Lay(Box(Size::Px(36.0f), Size::Px(36.0f)))
              .Set(doc::Prop::CornerRadius, 999.0f)
              .Token(doc::Prop::Fill, "surfaceAlt");
        Label(d, avatar, "Initials", "AC", "textMuted", 13.0f).Set(doc::Prop::FontWeight, 600.0f);

        B text = Frame(d, card, "Text");
        text.Lay(Stack(Axis::Column, 3.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Hug()));
        Label(d, text, "Title", "Acme Corp", "text", 13.0f).Set(doc::Prop::FontWeight, 600.0f);
        Label(d, text, "Description", "Everything you need, and a few things you do not.",
              "textMuted", 12.0f).Set(doc::Prop::TextWrap, std::string("word"));
        return d.MakeComponent(root, "HoverCard");
    }

}
