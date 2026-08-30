// State, disclosure and feedback: what a widget says about itself.
#include "vaepch.h"
#include "vae/ui/library/Catalog.h"

namespace vae::ui::catalog {

    // ------------------------------------------------------------------- state

    Uuid BuildBadge(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Badge");
        root.Lay(Stack(Axis::Row, 4.0f, Edges(8.0f, 2.0f), Align::Center, Justify::Center))
            .Lay(Box(Size::Hug(), Size::Hug()))
            .Set(doc::Prop::CornerRadius, 999.0f)
            .Token(doc::Prop::Fill, "accent");
        Label(d, root, "Label", "Badge", "accentText", 11.0f).Set(doc::Prop::FontWeight, 600.0f);
        return d.MakeComponent(root, "Badge");
    }

    Uuid BuildKbd(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Kbd");
        root.Lay(Stack(Axis::Row, 0.0f, Edges(6.0f, 2.0f), Align::Center, Justify::Center))
            .Lay(Box(Size::Hug(), Size::Hug()))
            .Set(doc::Prop::CornerRadius, 4.0f)
            .Token(doc::Prop::Fill, "surfaceAlt")
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f);
        Label(d, root, "Key", "Ctrl", "textMuted", 11.0f);
        return d.MakeComponent(root, "Kbd");
    }

    // The state nobody plans for and every real app reaches: no results, no items, nothing yet.
    Uuid BuildEmpty(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Empty");
        root.Lay(Stack(Axis::Column, 8.0f, Edges(32.0f, 40.0f), Align::Center, Justify::Center))
            .Lay(Box(Size::Fill(), Size::Hug()));
        Label(d, root, "Icon", "", "textMuted", 28.0f)
            .Set(doc::Prop::FontFamily, std::string("JetBrainsMono Nerd Font"));
        Label(d, root, "Title", "Nothing here yet", "text", 14.0f)
            .Set(doc::Prop::FontWeight, 600.0f);
        Label(d, root, "Description", "What you make will show up here.", "textMuted", 12.0f)
            .Set(doc::Prop::TextWrap, std::string("word"));

        B action = Frame(d, root, "Action");
        action.Slot()
              .Lay(Stack(Axis::Row, 8.0f, Edges(0.0f, 8.0f), Align::Center, Justify::Center))
              .Lay(Box(Size::Hug(), Size::Hug()));
        return d.MakeComponent(root, "Empty");
    }

    // The banner, not the screen: an alert *screen* is a decision the app is waiting on, and
    // P12a made that a screen kind. This one just says something on the page.
    Uuid BuildAlert(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Alert");
        root.Lay(Stack(Axis::Row, 10.0f, Edges(12.0f), Align::Start))
            .Lay(Box(Size::Fill(), Size::Hug()));
        Surface(root, 8.0f, "surfaceAlt");

        Label(d, root, "Icon", "", "accent", 15.0f)
            .Set(doc::Prop::FontFamily, std::string("JetBrainsMono Nerd Font"));

        B text = Frame(d, root, "Text");
        text.Lay(Stack(Axis::Column, 3.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Hug()));
        Label(d, text, "Title", "Heads up", "text", 13.0f).Set(doc::Prop::FontWeight, 600.0f);
        Label(d, text, "Description", "Something worth reading before carrying on.",
              "textMuted", 12.0f).Set(doc::Prop::TextWrap, std::string("word"));
        return d.MakeComponent(root, "Alert");
    }

    // Waiting, with a shape: the block that stands in for content while it loads. Grey rather
    // than animated for now — a shimmer is a behavior, and this is not one yet.
    Uuid BuildSkeleton(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Skeleton");
        root.Lay(Box(Size::Fill(), Size::Px(16.0f)))
            .Set(doc::Prop::CornerRadius, 4.0f)
            .Token(doc::Prop::Fill, "surfaceAlt");
        return d.MakeComponent(root, "Skeleton");
    }

    Uuid BuildAvatar(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Avatar");
        root.Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Center, Justify::Center))
            .Lay(Box(Size::Px(36.0f), Size::Px(36.0f)))
            .Set(doc::Prop::CornerRadius, 999.0f)
            .Set(doc::Prop::ClipContent, true)
            .Token(doc::Prop::Fill, "surfaceAlt");
        // Initials are the fallback that always works; an image dropped into the frame covers it.
        Label(d, root, "Initials", "VA", "textMuted", 13.0f).Set(doc::Prop::FontWeight, 600.0f);
        return d.MakeComponent(root, "Avatar");
    }

    Uuid BuildBreadcrumb(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Breadcrumb");
        root.Lay(Stack(Axis::Row, 6.0f, Edges(0.0f), Align::Center))
            .Lay(Box(Size::Hug(), Size::Hug()));
        for (int i = 1; i <= 3; ++i) {
            if (i > 1) Label(d, root, "Sep " + std::to_string(i), "/", "textMuted", 12.0f);
            B crumb = Frame(d, root, "Crumb " + std::to_string(i));
            crumb.Role(Role::Button)
                 .Lay(Stack(Axis::Row, 0.0f, Edges(4.0f, 2.0f), Align::Center))
                 .Lay(Box(Size::Hug(), Size::Hug()))
                 .Set(doc::Prop::CornerRadius, 4.0f)
                 .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt");
            // The last crumb is where you are, so it is not a link.
            Label(d, crumb, "Label", "Level " + std::to_string(i),
                  i == 3 ? "text" : "textMuted", 12.0f);
        }
        return d.MakeComponent(root, "Breadcrumb");
    }

    Uuid BuildToggle(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Toggle");
        // A button that stays down. Checkbox is the behavior — what it looks like is the
        // document's business, and here it looks like a button.
        root.Role(Role::Checkbox)
            .Lay(Stack(Axis::Row, 6.0f, Edges(10.0f, 6.0f), Align::Center, Justify::Center))
            .Lay(Box(Size::Hug(), Size::Px(32.0f)))
            .Set(doc::Prop::CornerRadius, 6.0f)
            .Token(doc::Prop::Fill, "surfaceAlt")
            .On(StateBit::Checked, doc::Prop::Fill, "accent")
            .OnTint(StateBit::Hovered, 0.08f);
        Label(d, root, "Label", "Toggle", "text", 13.0f)
            .On(StateBit::Checked, doc::Prop::TextColor, "accentText");
        return d.MakeComponent(root, "Toggle");
    }

    // ------------------------------------------------------------------- disclosure

    // One folding section, used on its own and three at a time inside an Accordion. The header
    // is a plain frame: giving it a Button role would make the click a click and never an open.
    B AddCollapsible(doc::Document& d, Uuid parent, const std::string& title,
                     const std::string& body, bool open = false) {
        B root = Frame(d, parent, title);
        root.Role(Role::Collapsible)
            .Lay(Stack(Axis::Column, 0.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Hug()))
            .Set(doc::Prop::Open, open);

        B header = Frame(d, root, "Header");
        header.Lay(Stack(Axis::Row, 8.0f, Edges(10.0f, 9.0f), Align::Center))
              .Lay(Box(Size::Fill(), Size::Hug()))
              .Set(doc::Prop::CornerRadius, 6.0f)
              .On(StateBit::Hovered, doc::Prop::Fill, "surfaceAlt");
        // The arrow turns because the root is open, not because anything wrote to it: state
        // reaches down to the parts of a widget, so this is one text node with two texts.
        Label(d, header, "Chevron", "▸", "textMuted", 13.0f)
            .OnValue(StateBit::Open, doc::Prop::Text, std::string("▾"));
        Label(d, header, "Title", title, "text", 13.0f)
            .Set(doc::Prop::FontWeight, 500.0f)
            .Lay(Box(Size::Fill(), Size::Hug()));

        B content = Frame(d, root, "Content");
        content.Role(Role::Content).Slot()
               .Lay(Stack(Axis::Column, 6.0f, Edges(10.0f, 0.0f, 10.0f, 10.0f), Align::Stretch))
               .Lay(Box(Size::Fill(), Size::Hug()));
        Label(d, content, "Body", body, "textMuted", 12.0f)
            .Set(doc::Prop::TextWrap, std::string("word"));
        return root;
    }

    Uuid BuildCollapsible(doc::Document& d) {
        B root = AddCollapsible(d, Uuid::Invalid(), "Collapsible",
                                "The part that folds away. Anything can go in here.");
        return d.MakeComponent(root, "Collapsible");
    }

    Uuid BuildAccordion(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Accordion");
        root.Role(Role::Accordion)
            .Lay(Stack(Axis::Column, 0.0f, Edges(4.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Hug()));
        Surface(root, 8.0f);

        AddCollapsible(d, root, "First", "Opening another section closes this one.", true);
        AddCollapsible(d, root, "Second", "One at a time is what makes it an accordion.");
        AddCollapsible(d, root, "Third", "Sections that all open at once are just collapsibles.");
        return d.MakeComponent(root, "Accordion");
    }

    // ------------------------------------------------------------------- feedback

    Uuid BuildProgress(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Progress");
        root.Role(Role::Progress)
            .Lay(Stack(Axis::Row, 0.0f, Edges(0.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Px(8.0f)))
            .Set(doc::Prop::CornerRadius, 999.0f)
            // Clipped, because the same bar becomes an indeterminate one the moment a Duration
            // is set on it, and an indeterminate bar travels off both ends of its own track.
            .Set(doc::Prop::ClipContent, true)
            .Token(doc::Prop::Fill, "surfaceAlt")
            .Set(doc::Prop::Value, 0.6f)
            .Set(doc::Prop::MinValue, 0.0f)
            .Set(doc::Prop::MaxValue, 1.0f);

        B bar = Frame(d, root, "Bar");
        bar.Role(Role::Fill)
           .Lay(Box(Size::Percent(0.6f), Size::Fill()))
           .Set(doc::Prop::CornerRadius, 999.0f)
           .Token(doc::Prop::Fill, "accent");
        return d.MakeComponent(root, "Progress");
    }

    // The same widget with no idea how far along it is. Absolute layout, because what moves is
    // the bar's position and only an absolute parent honours an offset.
    // A ring of dots chasing each other, which is what "wait" looks like beside a sentence.
    // The indeterminate *bar* is the same fact drawn for the top of a page, and it is Progress
    // with a Duration on it — the two are one behaviour and two drawings, not two widgets.
    Uuid BuildSpinner(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Spinner");
        root.Role(Role::Progress)
            .Lay([](LayoutStyle& style) { style.mode = LayoutMode::Absolute; })
            .Lay(Box(Size::Px(20.0f), Size::Px(20.0f)))
            // Seconds per lap. Its presence is what says "indeterminate".
            .Set(doc::Prop::Duration, 1.0f);

        // Eight of them. The behaviour puts them round the circle and fades them, because
        // where they go depends on how big the thing ends up.
        for (int i = 1; i <= 8; ++i) {
            B dot = Frame(d, root, "Dot " + std::to_string(i));
            dot.Role(Role::Indicator)
               .Lay(Box(Size::Px(4.0f), Size::Px(4.0f)))
               .Set(doc::Prop::CornerRadius, 999.0f)
               .Token(doc::Prop::Fill, "accent");
        }
        return d.MakeComponent(root, "Spinner");
    }

    // Numbers on the document, so a chart can be laid out before anything runs. Line, area or
    // bars is one property; a script writes the same `series` when there is real data.
    Uuid BuildChart(doc::Document& d) {
        B root = Frame(d, Uuid::Invalid(), "Chart");
        root.Role(Role::Chart)
            .Lay(Stack(Axis::Column, 0.0f, Edges(12.0f), Align::Stretch))
            .Lay(Box(Size::Fill(), Size::Px(180.0f)))
            .Set(doc::Prop::CornerRadius, 8.0f)
            .Set(doc::Prop::ClipContent, true)
            .Token(doc::Prop::Fill, "surface")
            .Token(doc::Prop::Stroke, "border")
            .Set(doc::Prop::StrokeWidth, 1.0f)
            .Set(doc::Prop::ChartKind, std::string("area"))
            .Set(doc::Prop::Series,
                 std::string("18, 24, 21, 33, 29, 41, 38, 52, 47, 61, 58, 72"));

        // Two parts the designer styles and the plot reads: the fill under the line, and the
        // grid behind it. Neither is drawn as a node — a chart is one shape, not eighty.
        B line = Frame(d, root, "Line");
        line.Role(Role::Indicator).Hidden().Token(doc::Prop::Fill, "accent")
            .Set(doc::Prop::StrokeWidth, 2.0f);
        B area = Frame(d, root, "Area");
        area.Role(Role::Fill).Hidden().Token(doc::Prop::Fill, "accent")
            .Set(doc::Prop::FillOpacity, 0.22f);
        B grid = Frame(d, root, "Grid");
        grid.Role(Role::Track).Hidden().Token(doc::Prop::Fill, "border");
        return d.MakeComponent(root, "Chart");
    }

}
