#include "Blocks.h"

#include "vae/base/Log.h"
#include "vae/doc/Document.h"

namespace vae {

    using namespace layout;

    namespace {

        // Authored, not edited: these go straight to the document rather than through EditorState,
        // so opening a block does not land on the undo stack as three hundred steps.
        void Set(doc::Document& d, Uuid node, doc::Prop prop, doc::Value value) {
            d.SetProp(node, prop, std::move(value));
        }
        void Token(doc::Document& d, Uuid node, doc::Prop prop, const char* token) {
            d.SetProp(node, prop, doc::TokenRef{ token });
        }
        LayoutStyle& Lay(doc::Document& d, Uuid node) {
            d.Touch(node);
            return d.Find(node)->layout;
        }

        // A node inside a component, by name — or by a dotted path when the name alone is not
        // unique. Every navigation item in a Navbar has a child called "Label", so "Home.Label" is
        // the difference between renaming the right one and renaming the first one found.
        Uuid Inside(const doc::Document& d, Uuid root, std::string_view path) {
            const std::size_t dot = path.find('.');
            const std::string_view head = path.substr(0, dot);

            Uuid found = Uuid::Invalid();
            std::vector<Uuid> stack{ root };
            while (!stack.empty() && !found.Valid()) {
                const Uuid id = stack.back();
                stack.pop_back();
                const doc::Node* node = d.Find(id);
                if (!node) continue;
                if (node->name == head) { found = id; break; }
                for (auto it = node->children.rbegin(); it != node->children.rend(); ++it)
                    stack.push_back(*it);
            }
            if (!found.Valid() || dot == std::string_view::npos) return found;
            return Inside(d, found, path.substr(dot + 1));
        }

        struct Kit {
            doc::Document& d;
            const ui::Library& library;

            Uuid Place(Uuid parent, std::string_view component, std::string name) const {
                const Uuid master = library.Find(component);
                if (!master.Valid()) {
                    VAE_ERROR("block: the library has no component called '{}'", component);
                    return Uuid::Invalid();
                }
                const Uuid instance = d.CreateInstance(master, parent);
                if (!instance.Valid()) return instance;
                d.Find(instance)->name = std::move(name);
                d.Touch(instance);
                return instance;
            }

            // Overrides a property of a named node inside an instance. Silent when the name is
            // wrong would hide a typo for ever, so it says so.
            Uuid Target(Uuid instance, std::string_view node) const {
                const doc::Node* self = d.Find(instance);
                if (!self) return Uuid::Invalid();
                const Uuid target = node.empty() ? self->componentId
                                                 : Inside(d, self->componentId, node);
                // Silent when the name is wrong would hide a typo for ever.
                if (!target.Valid())
                    VAE_ERROR("block: no node called '{}' in that component", node);
                return target;
            }
            void Over(Uuid instance, std::string_view node, doc::Prop prop, doc::Value value) const {
                if (const Uuid target = Target(instance, node); target.Valid())
                    d.SetOverride(instance, target, prop, std::move(value));
            }
            void Over(Uuid instance, std::string_view node, std::string key, doc::Value value) const {
                if (const Uuid target = Target(instance, node); target.Valid())
                    d.SetOverride(instance, target, std::move(key), std::move(value));
            }
            void Text(Uuid instance, std::string_view node, std::string text) const {
                Over(instance, node, doc::Prop::Text, std::move(text));
            }
            void Hide(Uuid instance, std::string_view node) const {
                Over(instance, node, doc::Prop::Visible, false);
            }
            void Tone(Uuid instance, std::string_view node, doc::Prop prop, const char* token) const {
                Over(instance, node, prop, doc::TokenRef{ token });
            }
            void Tone(Uuid instance, std::string_view node, std::string key, const char* token) const {
                Over(instance, node, std::move(key), doc::TokenRef{ token });
            }

            Uuid Label(Uuid parent, std::string name, std::string text, f32 size,
                       const char* colour, f32 weight = 400.0f) const {
                const Uuid id = d.CreateNode(doc::NodeKind::Text, parent, std::move(name));
                Set(d, id, doc::Prop::Text, std::move(text));
                Set(d, id, doc::Prop::FontSize, size);
                Set(d, id, doc::Prop::FontWeight, weight);
                Set(d, id, doc::Prop::TextWrap, std::string("none"));
                Token(d, id, doc::Prop::TextColor, colour);
                return id;
            }

            // A plain arrangement frame. Not a widget: a row is a fact about where things sit, and
            // the catalog is not short a component for having none called "Row".
            Uuid Stack(Uuid parent, std::string name, Axis axis, f32 gap, Edges padding = {},
                       Align align = Align::Stretch,
                       Justify justify = Justify::Start) const {
                const Uuid id = d.CreateNode(doc::NodeKind::Frame, parent, std::move(name));
                LayoutStyle& style = Lay(d, id);
                style.mode = LayoutMode::Stack;
                style.axis = axis;
                style.gap = gap;
                style.padding = padding;
                style.align = align;
                style.justify = justify;
                style.width = Size::Fill();
                style.height = Size::Hug();
                return id;
            }
        };

        // A button that reads as secondary: the same component, restyled through overrides the way
        // a designer would, rather than a second component nobody would think to look for.
        void Outline(const Kit& kit, Uuid button) {
            kit.Tone(button, "", doc::Prop::Fill, "surface");
            kit.Tone(button, "", doc::Prop::Stroke, "border");
            kit.Over(button, "", doc::Prop::StrokeWidth, 1.0f);
            kit.Tone(button, "Label", doc::Prop::TextColor, "text");
            kit.Over(button, "", std::string("hovered:tint"), 0.0f);
            kit.Tone(button, "", std::string("hovered:fill"), "surfaceAlt");
        }

    }

    void BuildLoginBlock(EditorState& state) {
        state.NewProject();
        doc::Document& d = state.Doc();
        const Kit kit{ d, state.Library() };

        const Uuid screen = state.ActiveScreen();
        d.Find(screen)->name = "Login";
        {
            LayoutStyle& style = Lay(d, screen);
            style.mode = LayoutMode::Stack;
            style.axis = Axis::Column;
            style.gap = 18.0f;
            style.padding = Edges(48.0f);
            style.align = Align::Center;
            style.justify = Justify::Center;
        }
        Token(d, screen, doc::Prop::Fill, "bg");

        const Uuid card = kit.Place(screen, "Card", "Card");
        Lay(d, card).width = Size::Px(420.0f);
        kit.Text(card, "Title", "Welcome back");
        kit.Text(card, "Description", "Login with your Apple or Google account");

        // Everything below is a child of the card in the document, and lands in the card's Body on
        // screen. That is the slot doing the work the whole block was built to test.
        const Uuid apple = kit.Place(card, "Button", "Apple");
        kit.Text(apple, "Label", "Login with Apple");
        Lay(d, apple).width = Size::Fill();
        Outline(kit, apple);

        const Uuid google = kit.Place(card, "Button", "Google");
        kit.Text(google, "Label", "Login with Google");
        Lay(d, google).width = Size::Fill();
        Outline(kit, google);

        const Uuid divider = kit.Stack(card, "Or", Axis::Row, 10.0f, {}, Align::Center);
        {
            const Uuid left = kit.Place(divider, "Separator", "Left");
            Lay(d, left).width = Size::Fill();
            kit.Label(divider, "Text", "Or continue with", 11.0f, "textMuted");
            const Uuid right = kit.Place(divider, "Separator", "Right");
            Lay(d, right).width = Size::Fill();
        }

        const auto field = [&](std::string name, std::string caption, std::string placeholder,
                               bool secret) {
            const Uuid id = kit.Place(card, "Field", std::move(name));
            Lay(d, id).width = Size::Fill();
            kit.Text(id, "Label", std::move(caption));
            kit.Hide(id, "Help");

            const Uuid input = kit.Place(id, "TextInput", "Input");
            Lay(d, input).width = Size::Fill();
            kit.Over(input, "", doc::Prop::Placeholder, std::move(placeholder));
            if (secret) kit.Over(input, "", doc::Prop::Password, true);
            return id;
        };
        field("Email", "Email", "m@example.com", false);
        const Uuid password = field("Password", "Password", "", true);
        kit.Over(password, "Help", doc::Prop::Visible, true);
        kit.Text(password, "Help", "Forgot your password?");

        const Uuid submit = kit.Place(card, "Button", "Login");
        kit.Text(submit, "Label", "Login");
        Lay(d, submit).width = Size::Fill();

        kit.Label(card, "Signup", "Don't have an account? Sign up", 12.0f, "textMuted");

        const Uuid terms = kit.Label(screen, "Terms",
                                     "By clicking continue, you agree to our Terms of Service "
                                     "and Privacy Policy.", 11.0f, "textMuted");
        Set(d, terms, doc::Prop::TextAlign, std::string("center"));
    }

    void BuildDashboardBlock(EditorState& state) {
        state.NewProject();
        doc::Document& d = state.Doc();
        const Kit kit{ d, state.Library() };

        const Uuid screen = state.ActiveScreen();
        d.Find(screen)->name = "Dashboard";
        {
            LayoutStyle& style = Lay(d, screen);
            style.mode = LayoutMode::Stack;
            style.axis = Axis::Row;
            style.gap = 0.0f;
            style.width = Size::Px(1440.0f);
            style.height = Size::Px(900.0f);
            style.align = Align::Stretch;
        }
        Token(d, screen, doc::Prop::Fill, "bg");

        // --- the sidebar ------------------------------------------------------------------------
        const Uuid sidebar = kit.Place(screen, "Sidebar", "Sidebar");
        Lay(d, sidebar).height = Size::Fill();
        kit.Text(sidebar, "Heading", "Acme Inc.");
        static const char* kNav[] = { "Dashboard", "Lifecycle", "Analytics", "Projects" };
        for (int i = 0; i < 4; ++i)
            kit.Text(sidebar, "Item " + std::to_string(i + 1) + ".Label", kNav[i]);

        const Uuid main = kit.Stack(screen, "Main", Axis::Column, 0.0f, {}, Align::Stretch);
        Lay(d, main).height = Size::Fill();

        // --- the header -------------------------------------------------------------------------
        const Uuid header = kit.Place(main, "Navbar", "Header");
        kit.Text(header, "Name", "Documents");
        kit.Text(header, "Home.Label", "Overview");
        kit.Text(header, "Projects.Label", "Analytics");
        kit.Text(header, "Team.Label", "Reports");
        kit.Text(header, "Action.Label", "GitHub");

        const Uuid body = kit.Stack(main, "Body", Axis::Column, 20.0f, Edges(24.0f),
                                    Align::Stretch);
        Lay(d, body).height = Size::Fill();

        // --- the figures ------------------------------------------------------------------------
        const Uuid figures = kit.Place(body, "Grid", "Figures");
        {
            LayoutStyle& style = Lay(d, figures);
            // As many columns as fit at 240 rather than four whatever the width — a hard column
            // count is `repeat(4, ...)`, and it runs off the edge of a narrow window exactly the
            // way it does in a browser. The cards fill their track, so the row stays even.
            style.minColumn = 240.0f;
            style.gap = 16.0f;
        }
        struct Figure { const char* caption; const char* value; const char* delta; const char* note; };
        static const Figure kFigures[] = {
            { "Total Revenue",  "$1,250.00", "+12.5%", "Trending up this month" },
            { "New Customers",  "1,234",     "-20%",   "Acquisition needs attention" },
            { "Active Accounts","45,678",    "+12.5%", "Strong user retention" },
            { "Growth Rate",    "4.5%",      "+4.5%",  "Meets growth projections" },
        };
        for (const Figure& figure : kFigures) {
            const Uuid card = kit.Place(figures, "Card", figure.caption);
            Lay(d, card).width = Size::Fill();
            kit.Text(card, "Title", figure.value);
            kit.Over(card, "Title", doc::Prop::FontSize, 26.0f);
            kit.Text(card, "Description", figure.caption);
            kit.Text(card, "Footer", "");

            // The badge sits in a row of its own: the card's body stretches what it holds, and a
            // pill that spans the whole card is not a pill.
            const Uuid line = kit.Stack(card, "Delta", Axis::Row, 8.0f, {}, Align::Center);
            const Uuid badge = kit.Place(line, "Badge", "Change");
            kit.Text(badge, "Label", figure.delta);
            kit.Tone(badge, "", doc::Prop::Fill, "surfaceAlt");
            kit.Tone(badge, "Label", doc::Prop::TextColor, "text");
            kit.Label(line, "Note", figure.note, 11.0f, "textMuted");
        }

        // --- the chart --------------------------------------------------------------------------
        const Uuid chart = kit.Place(body, "Chart", "Visitors");
        Lay(d, chart).height = Size::Px(240.0f);
        kit.Over(chart, "", doc::Prop::ChartKind, std::string("area"));
        kit.Over(chart, "", doc::Prop::Series,
                 std::string("120, 145, 132, 178, 164, 210, 195, 248, 232, 286, 271, 330, "
                             "312, 368, 349, 402"));

        // --- the table --------------------------------------------------------------------------
        const Uuid section = kit.Place(body, "Section", "Documents");
        kit.Text(section, "Heading", "Documents");

        const Uuid table = kit.Place(section, "Table", "Rows");
        Lay(d, table).width = Size::Fill();
        // Rows the app has not supplied yet. A designer needs to see the shape of the thing they
        // are laying out, and an empty table is indistinguishable from a broken one.
        kit.Over(table, "", doc::Prop::ItemCount, 8.0f);

        const Uuid footer = kit.Stack(section, "Footer", Axis::Row, 12.0f, {}, Align::Center,
                                      Justify::End);
        kit.Place(footer, "Pagination", "Pages");
    }

}
