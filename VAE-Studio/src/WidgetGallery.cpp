#include "WidgetGallery.h"

#include "vae/base/Log.h"
#include "vae/core/Application.h"
#include "vae/text/FontDB.h"

#include <string>

namespace vae {

    using namespace vae::layout;

    namespace {

        // The gallery's own scaffolding — sections and captions — is plain document nodes, built
        // the same way a designer's screen would be.
        Uuid Frame(doc::Document& d, Uuid parent, std::string name, Axis axis, f32 gap,
                   Edges padding = {}, Align align = Align::Center) {
            const Uuid id = d.CreateNode(doc::NodeKind::Frame, parent, std::move(name));
            LayoutStyle& style = d.Find(id)->layout;
            style.mode = LayoutMode::Stack;
            style.axis = axis;
            style.gap = gap;
            style.padding = padding;
            style.align = align;
            return id;
        }

        Uuid Text(doc::Document& d, Uuid parent, std::string name, std::string content,
                  const char* token, f32 size, f32 weight = 400.0f) {
            const Uuid id = d.CreateNode(doc::NodeKind::Text, parent, std::move(name));
            d.SetProp(id, doc::Prop::Text, std::move(content));
            d.SetProp(id, doc::Prop::TextColor, doc::TokenRef{ token });
            d.SetProp(id, doc::Prop::FontSize, size);
            d.SetProp(id, doc::Prop::FontWeight, weight);
            d.SetProp(id, doc::Prop::TextWrap, std::string("none"));
            return id;
        }

        Uuid Instance(doc::Document& d, const ui::Library& library, Uuid parent,
                      std::string_view widget, std::string name) {
            const Uuid id = d.CreateInstance(library.Find(widget), parent);
            if (id.Valid()) { d.Find(id)->name = std::move(name); d.Touch(id); }
            return id;
        }

        // Filenames down the left, so the list and the table have something real to show.
        struct SampleRows final : ui::UiHost::ListDataSource {
            u32 Count() const override { return 500; }
            std::string Cell(u32 row, u32 column) const override {
                switch (column) {
                    case 0:  return "asset_" + std::to_string(row) + ".png";
                    case 1:  return row % 3 == 0 ? "texture" : (row % 3 == 1 ? "mesh" : "audio");
                    default: return std::to_string(12 + (row * 37) % 900) + " KB";
                }
            }
        };

    }

    void WidgetGalleryLayer::OnAttach() {
        auto& app = Application::Get();
        if (!app.HasDevice()) return;
        auto& device = app.GetDevice();

        const gpu::Format format = device.GetSwapchain() ? device.GetSwapchain()->ColorFormat()
                                                         : gpu::Format::BGRA8_UNORM;
        if (!m_Renderer.Init(device, format)) {
            VAE_ERROR("draw renderer failed to initialise");
            return;
        }
        text::FontDB::Get().LoadDefaults();
        m_Atlas.Init(device);
        m_Ready = true;

        BuildScreen();
    }

    void WidgetGalleryLayer::OnDetach() {
        if (!m_Ready) return;
        m_Atlas.Shutdown();
        m_Renderer.Shutdown();
    }

    void WidgetGalleryLayer::BuildScreen() {
        m_Library = ui::BuildStandardLibrary(m_Document);

        m_Screen = m_Document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Gallery");
        {
            LayoutStyle& style = m_Document.Find(m_Screen)->layout;
            style.width = Size::Fill();
            style.height = Size::Fill();
        }

        const Uuid page = Frame(m_Document, m_Screen, "Page", Axis::Column, 22.0f,
                                Edges(32.0f), Align::Start);
        {
            LayoutStyle& style = m_Document.Find(page)->layout;
            style.width = Size::Fill();
            style.height = Size::Fill();
            m_Document.SetProp(page, doc::Prop::Fill, doc::TokenRef{ "bg" });
        }

        Text(m_Document, page, "Title", "Widget gallery", "text", 28.0f, 600.0f);

        auto Section = [&](const char* title) {
            const Uuid section = Frame(m_Document, page, title, Axis::Column, 10.0f, {}, Align::Start);
            m_Document.Find(section)->layout.width = Size::Fill();
            Text(m_Document, section, "Caption", title, "textMuted", 12.0f, 600.0f);
            const Uuid row = Frame(m_Document, section, "Row", Axis::Row, 16.0f, {}, Align::Center);
            m_Document.Find(row)->layout.width = Size::Fill();
            return row;
        };

        // --- buttons, one per state so the overlays are all visible at once -------------------
        {
            const Uuid row = Section("Button");
            for (const char* state : { "Normal", "Hovered", "Pressed", "Focused", "Disabled" }) {
                const Uuid button = Instance(m_Document, m_Library, row, "Button",
                                             std::string("Button ") + state);
                const Uuid component = m_Library.Find("Button");
                m_Document.SetOverride(button, component, doc::Prop::Enabled,
                                       std::string_view(state) != "Disabled");
                // The label is a node inside the component, so this is an ordinary override.
                const doc::Node* master = m_Document.Find(component);
                if (!master->children.empty())
                    m_Document.SetOverride(button, master->children.front(), doc::Prop::Text,
                                           std::string(state));
            }
        }

        // --- toggles ---------------------------------------------------------------------------
        {
            const Uuid row = Section("Checkbox · Radio · Switch");
            const Uuid checkbox = Instance(m_Document, m_Library, row, "Checkbox", "Checkbox off");
            const Uuid checked = Instance(m_Document, m_Library, row, "Checkbox", "Checkbox on");
            m_Document.SetOverride(checked, m_Library.Find("Checkbox"), doc::Prop::Checked, true);

            for (int i = 1; i <= 2; ++i) {
                const Uuid radio = Instance(m_Document, m_Library, row, "Radio",
                                            "Radio " + std::to_string(i));
                m_Document.SetOverride(radio, m_Library.Find("Radio"), doc::Prop::Group,
                                       std::string("demo"));
                m_Document.SetOverride(radio, m_Library.Find("Radio"), doc::Prop::Checked, i == 1);
            }

            Instance(m_Document, m_Library, row, "Switch", "Switch off");
            const Uuid on = Instance(m_Document, m_Library, row, "Switch", "Switch on");
            m_Document.SetOverride(on, m_Library.Find("Switch"), doc::Prop::Checked, true);
            (void)checkbox;
        }

        // --- text fields -------------------------------------------------------------------------
        {
            const Uuid row = Section("TextInput");
            const Uuid component = m_Library.Find("TextInput");
            Instance(m_Document, m_Library, row, "TextInput", "Field empty");
            const Uuid filled = Instance(m_Document, m_Library, row, "TextInput", "Field filled");
            m_Document.SetOverride(filled, component, doc::Prop::Text, std::string("Hello, VAE"));
            const Uuid secret = Instance(m_Document, m_Library, row, "TextInput", "Field password");
            m_Document.SetOverride(secret, component, doc::Prop::Text, std::string("hunter2"));
            m_Document.SetOverride(secret, component, doc::Prop::Password, true);
        }

        // --- slider, dropdown ---------------------------------------------------------------------
        {
            const Uuid row = Section("Slider · Dropdown");
            const Uuid slider = Instance(m_Document, m_Library, row, "Slider", "Slider");
            m_Document.SetOverride(slider, m_Library.Find("Slider"), doc::Prop::Value, 0.35f);
            Instance(m_Document, m_Library, row, "Dropdown", "Dropdown");
        }

        // --- tabs --------------------------------------------------------------------------------
        {
            const Uuid row = Section("Tabs");
            const Uuid tabs = Instance(m_Document, m_Library, row, "Tabs", "Tabs");
            m_Document.SetOverride(tabs, m_Library.Find("Tabs"), doc::Prop::SelectedIndex, 1.0f);
        }

        // --- data views --------------------------------------------------------------------------
        {
            const Uuid row = Section("List · Table");
            const Uuid list = Instance(m_Document, m_Library, row, "List", "List");
            const Uuid table = Instance(m_Document, m_Library, row, "Table", "Table");
            m_Document.Find(list)->layout.width = Size::Px(240.0f);
            m_Document.Find(table)->layout.width = Size::Px(440.0f);
            m_Document.Touch(list);
            m_Document.Touch(table);
            m_Document.SetOverride(list, m_Library.Find("List"), doc::Prop::SelectedIndex, 3.0f);

            const auto rows = CreateRef<SampleRows>();
            m_Host.SetDataSource({ m_Library.Find("List"), list }, rows);
            m_Host.SetDataSource({ m_Library.Find("Table"), table }, rows);
        }

        // --- floating surfaces, shown inline so the screenshot covers them ------------------------
        {
            const Uuid row = Section("Popover · Toast");
            Instance(m_Document, m_Library, row, "Popover", "Popover");
            Instance(m_Document, m_Library, row, "Toast", "Toast");
        }

        // --- containers, which is most of what an app is around its inputs ----------------------
        {
            const Uuid row = Section("Card · Item · Alert");
            Instance(m_Document, m_Library, row, "Card", "Card");
            Instance(m_Document, m_Library, row, "Item", "Item");
            Instance(m_Document, m_Library, row, "Alert", "Alert");
        }
        {
            const Uuid row = Section("Field · InputGroup · ButtonGroup");
            Instance(m_Document, m_Library, row, "Field", "Field");
            Instance(m_Document, m_Library, row, "InputGroup", "InputGroup");
            Instance(m_Document, m_Library, row, "ButtonGroup", "ButtonGroup");
        }
        {
            const Uuid row = Section("Badge · Kbd · Avatar · Toggle · Breadcrumb · Separator");
            Instance(m_Document, m_Library, row, "Badge", "Badge");
            Instance(m_Document, m_Library, row, "Kbd", "Kbd");
            Instance(m_Document, m_Library, row, "Avatar", "Avatar");
            Instance(m_Document, m_Library, row, "Toggle", "Toggle");
            const Uuid on = Instance(m_Document, m_Library, row, "Toggle", "Toggle on");
            m_Document.SetOverride(on, m_Library.Find("Toggle"), doc::Prop::Checked, true);
            Instance(m_Document, m_Library, row, "Breadcrumb", "Breadcrumb");
            Instance(m_Document, m_Library, row, "Separator", "Separator");
        }
        {
            // The grid is the reason grid is a layout mode: it reflows on a resize rather than
            // overflowing, which a stack of stacks cannot do.
            const Uuid row = Section("Grid · AspectRatio · Skeleton · Empty");
            Instance(m_Document, m_Library, row, "Grid", "Grid");
            Instance(m_Document, m_Library, row, "AspectRatio", "AspectRatio");
            Instance(m_Document, m_Library, row, "Skeleton", "Skeleton");
            Instance(m_Document, m_Library, row, "Empty", "Empty");
        }

        {
            // Behaviour, not composition. The accordion is shown with its first section open so
            // the screenshot proves the fold actually holds a body.
            const Uuid row = Section("Collapsible · Accordion · Progress · Spinner");
            const Uuid open = Instance(m_Document, m_Library, row, "Collapsible", "Collapsible open");
            m_Document.SetOverride(open, m_Library.Find("Collapsible"), doc::Prop::Open, true);
            Instance(m_Document, m_Library, row, "Collapsible", "Collapsible closed");
            Instance(m_Document, m_Library, row, "Accordion", "Accordion");

            const Uuid bar = Instance(m_Document, m_Library, row, "Progress", "Progress");
            m_Document.Find(bar)->layout.width = Size::Px(200.0f);
            m_Document.Touch(bar);
            m_Document.SetOverride(bar, m_Library.Find("Progress"), doc::Prop::Value, 0.35f);

            const Uuid spinner = Instance(m_Document, m_Library, row, "Spinner", "Spinner");
            m_Document.Find(spinner)->layout.width = Size::Px(200.0f);
            m_Document.Touch(spinner);
        }
        {
            const Uuid row = Section("Splitter · Tooltip · ContextMenu");
            const Uuid split = Instance(m_Document, m_Library, row, "Splitter", "Splitter");
            m_Document.Find(split)->layout.width = Size::Px(360.0f);
            m_Document.Touch(split);
            Instance(m_Document, m_Library, row, "Tooltip", "Tooltip");
            const Uuid menu = Instance(m_Document, m_Library, row, "ContextMenu", "ContextMenu");
            m_Document.Find(menu)->layout.width = Size::Px(240.0f);
            m_Document.Touch(menu);
        }

        {
            const Uuid row = Section("Chart");
            const Uuid area = Instance(m_Document, m_Library, row, "Chart", "Area chart");
            m_Document.Find(area)->layout.width = Size::Px(420.0f);
            m_Document.Touch(area);
            const Uuid bars = Instance(m_Document, m_Library, row, "Chart", "Bar chart");
            m_Document.Find(bars)->layout.width = Size::Px(420.0f);
            m_Document.Touch(bars);
            m_Document.SetOverride(bars, m_Library.Find("Chart"), doc::Prop::ChartKind,
                                   std::string("bars"));
            m_Document.SetOverride(bars, m_Library.Find("Chart"), doc::Prop::Series,
                                   std::string("14, 22, 9, 31, 26, 18, 35"));
        }
        {
            const Uuid row = Section("Menu · Menubar · Pagination");
            Instance(m_Document, m_Library, row, "Menu", "Menu");
            const Uuid bar = Instance(m_Document, m_Library, row, "Menubar", "Menubar");
            m_Document.Find(bar)->layout.width = Size::Px(320.0f);
            m_Document.Touch(bar);
            const Uuid pager = Instance(m_Document, m_Library, row, "Pagination", "Pagination");
            m_Document.SetOverride(pager, m_Library.Find("Pagination"), doc::Prop::Value, 2.0f);
        }
        {
            const Uuid row = Section("Navbar");
            Instance(m_Document, m_Library, row, "Navbar", "Navbar");
        }
        {
            const Uuid row = Section("Command · HoverCard");
            Instance(m_Document, m_Library, row, "Command", "Command");
            Instance(m_Document, m_Library, row, "HoverCard", "HoverCard");
        }

        {
            const Uuid row = Section("Combobox · InputOtp · Calendar");
            Instance(m_Document, m_Library, row, "Combobox", "Combobox");
            Instance(m_Document, m_Library, row, "InputOtp", "InputOtp");
            const Uuid calendar = Instance(m_Document, m_Library, row, "Calendar", "Calendar");
            m_Document.SetOverride(calendar, m_Library.Find("Calendar"), doc::Prop::Text,
                                   std::string("2026-08-22"));
        }
        {
            const Uuid row = Section("Carousel");
            const Uuid carousel = Instance(m_Document, m_Library, row, "Carousel", "Carousel");
            m_Document.Find(carousel)->layout.width = Size::Px(560.0f);
            m_Document.Touch(carousel);
        }

        m_Host.SetDocument(m_Document, m_Screen);

        // The desktop clipboard, so copy and paste in the gallery are the real thing rather than
        // the in-process stand-in the tests use.
        class WindowClipboard final : public ui::Clipboard {
        public:
            void SetText(const std::string& text) override {
                Application::Get().GetWindow().SetClipboardText(text);
            }
            std::string GetText() const override {
                return Application::Get().GetWindow().ClipboardText();
            }
        };
        m_Host.SetClipboard(CreateScope<WindowClipboard>());
    }

    // Hover and press are transient by nature, so the gallery pins them on the instances named for
    // them. Everything else about those widgets — including how the pinned state looks — still
    // comes from the component's own overlays.
    void WidgetGalleryLayer::ForceShowcaseStates() {
        ui::ViewTree& tree = m_Host.Tree();
        struct Pin { const char* name; ui::StateBit state; };
        static constexpr Pin kPins[] = {
            { "Button Hovered", ui::StateBit::Hovered },
            { "Button Pressed", ui::StateBit::Pressed },
            { "Button Focused", ui::StateBit::Focused },
        };
        for (const Pin& pin : kPins) {
            const u32 view = tree.FindByName(pin.name);
            if (view != ui::ViewTree::kInvalid) tree.SetState(view, pin.state, true);
        }
    }

    void WidgetGalleryLayer::OnUpdate(Timestep ts) { m_Delta = ts; }

    void WidgetGalleryLayer::OnUiRender(gpu::CommandList& cmd) {
        if (!m_Ready) return;

        auto& window = Application::Get().GetWindow();
        m_Viewport = { static_cast<f32>(window.Width()), static_cast<f32>(window.Height()) };

        m_Host.Update(m_Viewport, m_Delta);
        ForceShowcaseStates();
        window.SetCursor(static_cast<Cursor>(m_Host.Cursor()));

        m_List.Reset();
        ui::PaintContext paint;
        paint.list = &m_List;
        paint.atlas = &m_Atlas;
        m_Host.Paint(paint);
        m_Host.ClearActions();

        m_Renderer.NewFrame();
        m_Renderer.Render(cmd, m_List, m_Viewport);
    }

    void WidgetGalleryLayer::OnEvent(Event& e) {
        if (e.type == EventType::KeyPressed && e.key.code == Key::Escape
            && m_Host.OverlayCount() == 0) {
            Application::Get().Close();
            return;
        }
        // A caret blink and a hover highlight are both invisible if the frame never redraws, and
        // the loop is idle-driven.
        if (m_Host.Dispatch(e) || e.IsMouse() || e.IsKeyboard()) {
            e.handled = e.handled || false;
            Application::Get().RequestFrame();
        }
    }

}
