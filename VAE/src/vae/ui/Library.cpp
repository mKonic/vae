#include "vaepch.h"
#include "vae/ui/Library.h"

#include "vae/ui/Widget.h"

namespace vae::ui {

    using namespace vae::layout;

    namespace {

        doc::Token Colours(Color light, Color dark) { return doc::Token{ light, dark, {} }; }

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

        B Frame(doc::Document& d, Uuid parent, std::string name) {
            return B(d, doc::NodeKind::Frame, parent, std::move(name));
        }

        B Label(doc::Document& d, Uuid parent, std::string name, std::string content,
                std::string colour = "text", f32 size = 14.0f) {
            B node(d, doc::NodeKind::Text, parent, std::move(name));
            node.Set(doc::Prop::Text, std::move(content))
                .Token(doc::Prop::TextColor, std::move(colour))
                .Set(doc::Prop::FontSize, size)
                .Set(doc::Prop::TextWrap, std::string("none"));
            return node;
        }

        auto Stack(Axis axis, f32 gap, Edges padding, Align align = Align::Center,
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

        auto Box(Size width, Size height) {
            return [=](LayoutStyle& style) { style.width = width; style.height = height; };
        }

        // Equal columns, filled row by row. `columns` of zero means as many as fit at `minColumn`,
        // which is the version that reflows on a resize instead of overflowing.
        auto Grid(u16 columns, f32 gap, f32 minColumn = 160.0f, Edges padding = {}) {
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
        void Surface(B& node, f32 radius = 8.0f, const char* fill = "surface",
                     const char* border = "border") {
            node.Set(doc::Prop::CornerRadius, radius)
                .Token(doc::Prop::Fill, fill);
            if (border) node.Token(doc::Prop::Stroke, border).Set(doc::Prop::StrokeWidth, 1.0f);
        }

        void Shadow(B& node, f32 blur, f32 alpha) {
            node.Set(doc::Prop::ShadowColor, Color{ 0.0f, 0.0f, 0.0f, alpha })
                .Set(doc::Prop::ShadowBlur, blur)
                .Set(doc::Prop::ShadowOffset, Vec2{ 0.0f, blur * 0.35f });
        }

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

    void InstallDefaultTokens(doc::Document& document) {
        document.SetToken("bg",          Colours({ 0.96f, 0.96f, 0.97f, 1.0f }, { 0.086f, 0.094f, 0.118f, 1.0f }));
        document.SetToken("surface",     Colours({ 1.00f, 1.00f, 1.00f, 1.0f }, { 0.129f, 0.141f, 0.176f, 1.0f }));
        document.SetToken("surfaceAlt",  Colours({ 0.93f, 0.93f, 0.95f, 1.0f }, { 0.176f, 0.192f, 0.235f, 1.0f }));
        document.SetToken("border",      Colours({ 0.84f, 0.85f, 0.87f, 1.0f }, { 0.255f, 0.278f, 0.333f, 1.0f }));
        document.SetToken("text",        Colours({ 0.09f, 0.10f, 0.13f, 1.0f }, { 0.918f, 0.929f, 0.949f, 1.0f }));
        document.SetToken("textMuted",   Colours({ 0.42f, 0.44f, 0.49f, 1.0f }, { 0.576f, 0.612f, 0.671f, 1.0f }));
        document.SetToken("accent",      Colours({ 0.29f, 0.44f, 0.85f, 1.0f }, { 0.365f, 0.510f, 0.894f, 1.0f }));
        document.SetToken("accentHover", Colours({ 0.34f, 0.49f, 0.90f, 1.0f }, { 0.435f, 0.573f, 0.933f, 1.0f }));
        document.SetToken("accentActive",Colours({ 0.22f, 0.36f, 0.76f, 1.0f }, { 0.290f, 0.427f, 0.808f, 1.0f }));
        document.SetToken("accentText",  Colours({ 1.00f, 1.00f, 1.00f, 1.0f }, { 1.000f, 1.000f, 1.000f, 1.0f }));
        document.SetToken("danger",      Colours({ 0.80f, 0.24f, 0.24f, 1.0f }, { 0.902f, 0.353f, 0.353f, 1.0f }));
        document.SetToken("success",     Colours({ 0.16f, 0.60f, 0.36f, 1.0f }, { 0.298f, 0.733f, 0.475f, 1.0f }));
        document.SetToken("scrim",       Colours({ 0.00f, 0.00f, 0.00f, 0.35f }, { 0.000f, 0.000f, 0.000f, 0.55f }));
    }

    Uuid Library::Find(std::string_view name) const {
        auto it = components.find(name);
        return it == components.end() ? Uuid::Invalid() : it->second;
    }

    Library BuildStandardLibrary(doc::Document& document) {
        InstallDefaultTokens(document);

        Library library;
        // Each widget gets its own id scope, so the catalog comes back on the same ids every time
        // it is rebuilt and adding a widget cannot renumber the ones already in people's files.
        auto Add = [&](const char* name, auto&& build) {
            document.PushIdScope(std::string("vae.std/") + name);
            library.components[name] = build(document);
            document.PopIdScope();
        };
        Add("Button", BuildButton);
        Add("TextInput", BuildTextInput);
        Add("Checkbox", [](doc::Document& d) { return BuildCheckLike(d, "Checkbox", Role::Checkbox, 4.0f, 4.0f, 2.0f); });
        Add("Radio", [](doc::Document& d) { return BuildCheckLike(d, "Radio", Role::Radio, 9.0f, 5.0f, 4.0f); });
        Add("Switch", BuildSwitch);
        Add("Slider", BuildSlider);
        Add("Dropdown", BuildDropdown);
        Add("Tabs", BuildTabs);
        Add("Scroll", BuildScroll);
        Add("List", BuildList);
        Add("Table", BuildTable);
        Add("Modal", BuildModal);
        Add("Popover", BuildPopover);
        Add("Toast", BuildToast);
        Add("Router", BuildRouter);
        Add("Icon", BuildIcon);
        Add("Image", BuildImage);

        // Containers and the states an app spends most of its time in. All composition — frames,
        // text and the layout modes — which is what having a layout engine is for.
        Add("Card", BuildCard);
        Add("Section", BuildSection);
        Add("Separator", BuildSeparator);
        Add("AspectRatio", BuildAspectRatio);
        Add("Grid", BuildGridView);
        Add("Sidebar", BuildSidebar);
        Add("Field", BuildField);
        Add("ButtonGroup", BuildButtonGroup);
        Add("InputGroup", BuildInputGroup);
        Add("Item", BuildItem);
        Add("Badge", BuildBadge);
        Add("Kbd", BuildKbd);
        Add("Empty", BuildEmpty);
        Add("Alert", BuildAlert);
        Add("Skeleton", BuildSkeleton);
        Add("Avatar", BuildAvatar);
        Add("Breadcrumb", BuildBreadcrumb);
        Add("Toggle", BuildToggle);

        // Behaviour, not composition: each of these needed a native half before the component
        // could exist. Together they are what a long page, a running task and a right-click are.
        Add("Collapsible", BuildCollapsible);
        Add("Accordion", BuildAccordion);
        Add("Progress", BuildProgress);
        Add("Spinner", BuildSpinner);
        Add("Chart", BuildChart);
        Add("InputOtp", BuildInputOtp);
        Add("Carousel", BuildCarousel);
        Add("Combobox", BuildCombobox);
        Add("Calendar", BuildCalendar);
        Add("Splitter", BuildSplitter);
        Add("Tooltip", BuildTooltip);
        Add("ContextMenu", BuildContextMenu);

        // Menus, navigation, and the two things every page has that no input covers: text you can
        // copy out of, and a place to say more without clicking.
        Add("Menu", BuildMenu);
        Add("Menubar", BuildMenubar);
        Add("Navbar", BuildNavbar);
        Add("Pagination", BuildPagination);
        Add("Command", BuildCommand);
        Add("HoverCard", BuildHoverCard);
        return library;
    }

}
