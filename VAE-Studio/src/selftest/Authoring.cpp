// The panels: what the Inspector, the library and the catalog author.
#include "Harness.h"

namespace vae::selftest {


    void TestInspectorRoundTrip() {
        Section("inspector edit");
        Driver driver;
        EditorState& state = driver.State();

        const Uuid a = PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 200.0f, 48.0f });
        const Uuid b = PlaceFixed(state, "Button", { 500.0f, 300.0f }, { 200.0f, 48.0f });
        const Uuid master = state.Library().Find("Button");
        const doc::Value masterText = state.Doc().GetProp(master, doc::Prop::Text);

        state.SetProp(a, doc::Prop::Text, doc::Value{ std::string("Renamed") });
        state.EndGesture();

        const doc::Value read = state.GetProp(a, doc::Prop::Text);
        Check(std::holds_alternative<std::string>(read)
              && std::get<std::string>(read) == "Renamed",
              "the edit reads back off the instance");

        // The whole point of an override: one card says something different without every
        // other card changing with it.
        Check(state.Doc().GetProp(master, doc::Prop::Text) == masterText,
              "the component master is untouched");
        Check(state.GetProp(b, doc::Prop::Text) == masterText,
              "the sibling instance is untouched");

        state.Undo();
        const doc::Value after = state.GetProp(a, doc::Prop::Text);
        Check(after == masterText, "undo clears an override that did not exist before");

        state.Redo();
        const doc::Value redone = state.GetProp(a, doc::Prop::Text);
        Check(std::holds_alternative<std::string>(redone)
              && std::get<std::string>(redone) == "Renamed", "redo restores the override");

        // A layout edit is the other half of the inspector, and it goes through the same stack.
        layout::LayoutStyle style = state.Doc().Find(a)->layout;
        style.padding = Edges{ 12.0f, 13.0f, 14.0f, 15.0f };
        state.SetLayout(a, style);
        state.EndGesture();
        Check(state.Doc().Find(a)->layout.padding.left == 12.0f, "a layout edit lands");
        state.Undo();
        Check(state.Doc().Find(a)->layout.padding.left != 12.0f, "and undoes");
    }


    // Picking a breakpoint puts the whole Inspector into a mode: every field reads what the
    // design looks like at that width and writes an overlay instead of the base. This is the
    // plumbing under that — the mode itself, what a field is filed under, and the canvas
    // drawing the width being designed for.
    void TestBreakpointAuthoring() {
        Section("designing at a breakpoint");
        Driver driver;
        EditorState& state = driver.State();

        const Uuid card = PlaceFixed(state, "Button", { 40.0f, 40.0f }, { 320.0f, 48.0f });
        state.SetProp(card, doc::Prop::FontSize, 18.0f);
        state.EndGesture();

        Check(state.Breakpoint().empty(), "the Inspector starts on the base");
        Check(state.FieldKey(doc::Prop::FontSize) == "fontSize",
              "and a field there is filed under the property itself");

        state.SetBreakpoint("compact");
        Check(state.Breakpoint() == "compact", "a shipped breakpoint can be picked");
        Check(state.FieldKey(doc::Prop::FontSize) == "compact:fontSize",
              "and now a field writes an overlay");

        // Nothing overlaid yet, so the field shows what the node already is rather than blank —
        // an empty field would read as "unset" when the answer is "18, from the base".
        const doc::Value shown = state.FieldValue(card, doc::Prop::FontSize);
        Check(std::holds_alternative<f32>(shown) && std::get<f32>(shown) == 18.0f,
              "an unset overlay shows the value in effect");

        state.SetProp(card, state.FieldKey(doc::Prop::FontSize), 12.0f);
        state.EndGesture();
        const doc::Value overlaid = state.FieldValue(card, doc::Prop::FontSize);
        Check(std::holds_alternative<f32>(overlaid) && std::get<f32>(overlaid) == 12.0f,
              "and once set, the overlay is what the field shows");
        const doc::Value base = state.GetProp(card, doc::Prop::FontSize);
        Check(std::holds_alternative<f32>(base) && std::get<f32>(base) == 18.0f,
              "with the base left exactly as it was");

        Check(state.PreviewWidth() == 600.0f, "the canvas draws the width being designed for");
        state.SetBreakpoint({});
        Check(state.PreviewWidth() == 0.0f, "and the screen's own size at the base");

        // A name the document does not define is the base. Otherwise renaming a breakpoint
        // leaves the Inspector authoring into a width nothing will ever match.
        state.SetBreakpoint("nonesuch");
        Check(state.Breakpoint().empty(), "a breakpoint that is not defined is the base");

        state.Execute(CreateScope<doc::SetBreakpointsCommand>(
            std::vector<doc::Breakpoint>{ { "phone", 480.0f } }));
        state.EndGesture();
        state.SetBreakpoint("phone");
        Check(state.PreviewWidth() == 480.0f, "a project names its own");
        state.Undo();
        Check(state.Doc().Breakpoints() == doc::DefaultBreakpoints(),
              "and editing the set undoes like any other change");
    }


    // The XML tab is a second writer for a model the canvas and the Inspector already write,
    // so what matters is that an apply is one edit, that the node it edited is still the node
    // everything else is holding, and that a refusal costs nothing.
    void TestMarkupEditing() {
        Section("editing markup");
        Driver driver;
        EditorState& state = driver.State();

        const Uuid card = PlaceFixed(state, "Card", { 40.0f, 40.0f }, { 320.0f, 200.0f });
        state.Select(card);
        state.Commands().Clear();

        const std::string before = doc::Serializer::ToXmlSubtree(state.Doc(), card);
        Check(!before.empty() && before.find("<vae") == std::string::npos,
              "a selection reads as markup without the file around it");
        Check(before.find(card.ToString()) != std::string::npos,
              "and always with its id, so a round trip does not strand the selection");

        state.Execute(CreateScope<doc::ReplaceSubtreeCommand>(
            card, "<instance name=\"Card\" of=\"" + state.Doc().Find(card)->componentId.ToString()
                  + "\" mode=\"stack\" width=\"500\" gap=\"7\"/>"));
        state.EndGesture();

        Check(state.Doc().Contains(card), "an apply edits the node rather than replacing it");
        Check(state.Primary() == card, "so the selection is still on it");
        Check(state.Doc().Find(card)->layout.gap == 7.0f, "and what the markup said has landed");

        state.Undo();
        Check(state.Doc().Find(card)->layout.gap != 7.0f, "one undo puts the whole edit back");
        Check(state.Primary() == card, "with the selection still where it was");

        // A refusal is validated against a copy, so it never reaches the undo stack.
        doc::Document trial;
        doc::Serializer::FromXml(doc::Serializer::ToXml(state.Doc(), false, nullptr, true), trial);
        std::string error;
        Check(!doc::Serializer::FromXmlSubtree("<frame", trial, card, &error),
              "markup that does not parse is refused");
        Check(error.starts_with("line 1:"), "at the line the designer typed it on");
        Check(trial.Contains(card), "and the subtree it was refused over is untouched");
    }


    // --------------------------------------------------------------------------- screens

    void TestStyling() {
        Section("styling");
        Driver driver;
        EditorState& state = driver.State();

        const Uuid button = PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 160.0f, 40.0f });
        const Uuid master = state.Library().Find("Button");
        const doc::Node* component = state.Doc().Find(master);
        Check(component != nullptr, "the button component exists");
        if (!component) return;

        // The library styles widgets with theme tokens. A token that cannot be replaced is a
        // colour the designer cannot change, which is what "hardcoded" felt like.
        const doc::Value fill = state.GetProp(button, doc::Prop::Fill);
        Check(std::holds_alternative<doc::TokenRef>(fill), "a library widget is themed, not literal");

        const doc::Value resolved = state.Doc().ResolveValue(fill);
        const Color* was = std::get_if<Color>(&resolved);
        Check(was != nullptr, "the token resolves to a colour");
        if (!was) return;

        const u32 before = state.Commands().UndoDepth();
        state.SetProp(button, doc::Prop::Fill, doc::Value{ Color{ 0.9f, 0.2f, 0.2f, 1.0f } });
        state.EndGesture();
        Check(state.Commands().UndoDepth() == before + 1,
              "replacing a token with a literal is an undoable edit");

        const doc::Value literal = state.GetProp(button, doc::Prop::Fill);
        Check(std::holds_alternative<Color>(literal), "the fill is a literal now");

        state.Undo();
        Check(std::holds_alternative<doc::TokenRef>(state.GetProp(button, doc::Prop::Fill)),
              "undo puts the token back");
        state.Redo();

        // Hover is a property of its own, so recolouring the base leaves it saying whatever the
        // library authored — a red button that hovers blue. It has to be editable.
        const std::string hoverKey = ui::StateKey(ui::StateBit::Hovered, doc::Prop::Fill);
        state.SetProp(button, hoverKey, doc::Value{ Color{ 1.0f, 0.4f, 0.4f, 1.0f } });
        state.EndGesture();

        const doc::Value hover = state.GetProp(button, hoverKey);
        const Color* hovered = std::get_if<Color>(&hover);
        Check(hovered && hovered->r > 0.9f && hovered->b < 0.5f,
              "a state colour reads back off the instance");

        // And the component every other button shares is untouched by either edit.
        Check(state.Doc().GetProp(master, doc::Prop::Fill) == fill,
              "the component master keeps its token");

        // Clearing an override on an instance falls back to what the component says — which,
        // for a library widget, is a tint rather than a colour. That is the fix for the bug the
        // whole section is about: hover follows the base instead of naming a blue.
        state.SetProp(button, hoverKey, doc::Value{});
        state.EndGesture();
        const doc::Value tint =
            state.GetProp(button, ui::StateTintKey(ui::StateBit::Hovered));
        Check(std::holds_alternative<f32>(tint) && std::get<f32>(tint) > 0.0f,
              "with no colour named, hover is a tint of whatever the fill is");
    }


    void TestAuthoringAComponent() {
        Section("authoring a component");
        Driver driver;
        EditorState& state = driver.State();

        // A frame with two things in it: the shape somebody would want to reuse.
        const Uuid frame = state.CreateChild(doc::NodeKind::Frame, state.ActiveScreen(), "Row");
        {
            layout::LayoutStyle style = state.Doc().Find(frame)->layout;
            style.mode = layout::LayoutMode::Stack;
            style.axis = layout::Axis::Row;
            style.gap = 8.0f;
            style.offsetStart = { 80.0f, 80.0f };
            state.SetLayout(frame, style);
        }
        const Uuid label = state.CreateChild(doc::NodeKind::Text, frame, "Caption");
        state.SetProp(label, doc::Prop::Text, doc::Value{ std::string("Reusable") });
        const Uuid slot = state.CreateChild(doc::NodeKind::Frame, frame, "Body");
        state.EndGesture();
        driver.Frame();

        state.Select(frame);
        const std::size_t before = state.Library().components.size();
        const Uuid instance = state.MakeComponentFromSelection();
        driver.Frame();

        Check(instance.Valid(), "the frame became an instance of a new component");
        if (!instance.Valid()) return;
        Check(state.Library().components.size() == before + 1, "the library gained one entry");
        Check(state.Library().Find("Row") == frame, "and it is indexed under the frame's name");

        const doc::Node* master = state.Doc().Find(frame);
        Check(master && master->IsComponent(), "the frame itself is now the component");
        Check(master && !master->parent.Valid(), "and it left the screen");
        const doc::Node* placed = state.Doc().Find(instance);
        Check(placed && placed->parent == state.ActiveScreen(),
              "the instance took its place on the screen");
        Check(placed && Near(placed->layout.offsetStart.x, 80.0f),
              "in the same spot, so nothing on the screen moved");

        // A slot, and something put in it.
        state.SetSlot(slot, true);
        Check(state.Doc().SlotOf(frame) == slot, "the marked frame is the component's slot");

        const Uuid inner = state.PlaceInstance("Button", instance, { 0.0f, 0.0f });
        state.EndGesture();
        driver.Frame();
        Check(inner.Valid(), "a button dropped into the instance");
        const Rect box = driver.Surface().BoundsOf(state, inner);
        const Rect outer = driver.Surface().BoundsOf(state, instance);
        Check(box.size.x > 0.0f && outer.Contains(box.Center()),
              "and it is drawn inside the component's slot");

        // One slot per component: marking another moves it.
        state.SetSlot(label, true);
        Check(state.Doc().SlotOf(frame) == label, "marking another slot moves it");
        const doc::Node* previous = state.Doc().Find(slot);
        Check(previous && !previous->slot, "and clears the one that held it");

        // And the master can be opened again. A component leaves the screen when it is made,
        // so without a way back it is a definition nobody can ever edit.
        const Uuid screen = state.ActiveScreen();
        state.OpenComponent(frame);
        driver.Frame();
        Check(state.EditingComponent() == frame, "the master opened on the canvas");
        const Rect opened = driver.Surface().BoundsOf(state, frame);
        Check(opened.size.x > 0.0f && opened.size.y > 0.0f,
              "and it has a box, even though it hugs its contents");

        state.CloseComponent();
        driver.Frame();
        Check(state.ActiveScreen() == screen, "closing it goes back to the screen");
        Check(!state.EditingComponent().Valid(), "and nothing is being edited");
    }


    // Component properties, through the editor rather than through the document: declaring one,
    // an instance answering it, and undo putting both back. The model has its own tests; what
    // this covers is that the commands the Inspector fires are the ones that do it.
    void TestComponentProperties() {
        Section("component properties");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        EditorState& state = layer.State();
        doc::Document& d = state.Doc();

        const Uuid component = d.CreateNode(doc::NodeKind::Frame, Uuid::Invalid(), "Badge");
        d.SetProp(component, doc::Prop::Fill, Color{ 0.2f, 0.3f, 0.6f, 1.0f });
        const Uuid label = d.CreateNode(doc::NodeKind::Text, component, "Label");
        d.SetProp(label, doc::Prop::Text, doc::Binding{ "label" });
        d.MakeComponent(component, "Badge");
        const Uuid instance = d.CreateInstance(component, state.ActiveScreen());
        state.Commands().Clear();

        doc::ComponentProperty text;
        text.name = "label";
        text.type = doc::ValueType::Text;
        text.defaultValue = std::string("Badge");
        state.Execute(CreateScope<doc::SetComponentPropertyCommand>(component, text));
        state.EndGesture();
        Check(d.FindProperty(component, "label") != nullptr, "a component declares a property");
        Check(d.ResolvedProps(instance, label).Text(doc::Prop::Text) == "Badge",
              "and an instance that says nothing draws the default");

        state.Execute(CreateScope<doc::SetInstancePropertyCommand>(instance, "label",
                                                                    std::string("Ready")));
        state.EndGesture();
        Check(d.ResolvedProps(instance, label).Text(doc::Prop::Text) == "Ready",
              "an instance answers it and the binding resolves to the answer");

        doc::ComponentProperty tone;
        tone.name = "tone";
        tone.type = doc::ValueType::Text;
        tone.defaultValue = std::string("plain");
        tone.options = { "plain", "loud" };
        state.Execute(CreateScope<doc::SetComponentPropertyCommand>(component, tone));
        d.SetProp(component, doc::VariantOverlayPrefix("tone", "loud")
                             + doc::PropName(doc::Prop::Fill), Color{ 0.9f, 0.2f, 0.2f, 1.0f });
        state.EndGesture();

        state.Execute(CreateScope<doc::SetInstancePropertyCommand>(instance, "tone",
                                                                    std::string("loud")));
        state.EndGesture();
        Check(d.ResolvedProps(std::vector<Uuid>{}, instance).Colour(doc::Prop::Fill).r > 0.8f,
              "a variant switches what it names");

        state.Undo();
        Check(d.ResolvedProps(std::vector<Uuid>{}, instance).Colour(doc::Prop::Fill).r < 0.5f,
              "and undo puts the option back");

        // Removing the property an instance already answered: the answer stays on the instance
        // but stops meaning anything, and undo brings the question back.
        state.Execute(CreateScope<doc::RemoveComponentPropertyCommand>(component, "label"));
        state.EndGesture();
        Check(d.FindProperty(component, "label") == nullptr, "a property can be removed");
        state.Undo();
        Check(d.FindProperty(component, "label") != nullptr, "and undo brings it back");
        Check(d.ResolvedProps(instance, label).Text(doc::Prop::Text) == "Ready",
              "with the answer the instance had given it");
    }


    // The catalog's exam. Two of shadcn's own blocks, rebuilt as VAE screens: if a widget in
    // either one had to be hand-built out of frames, the catalog is short a component and this
    // is where that shows up as a failure instead of as a shrug.
    void TestBlocksComeFromTheCatalog() {
        Section("shadcn blocks");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();

        for (const auto& [example, name, least] :
             { std::tuple{ StudioLayer::Example::Login, "Login block", 8u },
               std::tuple{ StudioLayer::Example::Dashboard, "Dashboard block", 12u } }) {
            layer.OpenExample(example);
            driver.Frame();
            EditorState& state = layer.State();
            const doc::Document& d = state.Doc();

            u32 instances = 0;
            u32 handBuilt = 0;
            std::string offender;
            for (Uuid id : d.Subtree(state.ActiveScreen())) {
                const doc::Node* node = d.Find(id);
                if (!node) continue;
                if (node->IsInstance()) { ++instances; continue; }
                // A frame with a role is a widget somebody built by hand. A frame without one
                // is an arrangement — a row, a column — and arranging is not a component.
                if (node->props.Has(doc::Prop::Role)) {
                    ++handBuilt;
                    if (offender.empty()) offender = node->name;
                }
            }
            Check(instances >= least, std::string(name) + ": " + std::to_string(instances)
                                      + " components from the library");
            Check(handBuilt == 0, std::string(name) + ": no hand-built widgets"
                                  + (offender.empty() ? "" : " (" + offender + ")"));

            // And it lays out: a block that resolves to nothing on screen has proved nothing.
            const Rect box = layer.Surface().BoundsOf(state, state.ActiveScreen());
            Check(box.size.x > 300.0f && box.size.y > 300.0f,
                  std::string(name) + ": the screen has a box");
        }
    }


    void TestAssets() {
        Section("assets");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        EditorState& state = layer.State();

        const std::string name = "Selftest assets";
        const std::filesystem::path folder = FileSystem::ProjectsRoot() / name;
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
        layer.CreateProject(name);

        // A 1x1 PNG, written by hand: the point is the import path, not the decoder.
        static const unsigned char kPng[] = {
            0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A, 0,0,0,0x0D,'I','H','D','R',
            0,0,0,1, 0,0,0,1, 8,6,0,0,0, 0x1F,0x15,0xC4,0x89,
            0,0,0,0x0A,'I','D','A','T', 0x78,0x9C,0x63,0x00,0x01,0x00,0x00,0x05,0x00,0x01,
            0x0D,0x0A,0x2D,0xB4, 0,0,0,0,'I','E','N','D',0xAE,0x42,0x60,0x82 };
        const std::filesystem::path source = folder / "elsewhere.png";
        {
            std::ofstream out(source, std::ios::binary);
            out.write(reinterpret_cast<const char*>(kPng), sizeof kPng);
        }

        const Uuid asset = state.ImportAsset(source);
        Check(asset.Valid(), "an image imports: " + state.AssetError());
        Check(std::filesystem::exists(folder / "assets" / "elsewhere.png", ec),
              "and the file is copied into the project, not linked from where it was");
        const doc::Document::Asset* entry = state.Doc().FindAsset(asset);
        Check(entry && entry->path == "assets/elsewhere.png",
              "recorded by a relative path, so moving the project keeps it");

        // The same file twice is two assets, not one silently overwriting the other's file.
        const Uuid again = state.ImportAsset(source);
        Check(again.Valid() && again != asset, "importing it again is a second asset");
        Check(std::filesystem::exists(folder / "assets" / "elsewhere 2.png", ec),
              "with a name of its own");

        state.ImportAsset(folder / "nothing-here.png");
        Check(!state.AssetError().empty(), "a missing file says so rather than failing quietly");

        // And the reference survives a save and a load, which is the only thing that makes an
        // id better than a path.
        const Uuid picture = state.CreateChild(doc::NodeKind::Image, state.ActiveScreen(),
                                               "Picture");
        state.SetProp(picture, doc::Prop::Image, doc::AssetRef{ asset });
        state.EndGesture();
        layer.SaveProject(state.Path());

        EditorState reopened;
        Check(reopened.Load(folder / doc::Project::kFileName), "the project reopens");
        Check(reopened.Doc().FindAsset(asset) != nullptr, "with its assets");
        const doc::Node* back = FindByName(reopened.Doc(), "Picture");
        Check(back != nullptr, "and the picture that used it");
        const doc::Value ref = back ? reopened.Doc().GetProp(back->id, doc::Prop::Image)
                                    : doc::Value{};
        Check(std::holds_alternative<doc::AssetRef>(ref)
              && std::get<doc::AssetRef>(ref).id == asset,
              "and the node still points at the one it was given");

        std::filesystem::remove_all(folder, ec);
    }


    // Artwork takes the same route as a picture and comes out a different thing at the end of
    // it: a Vector node, sized from the file, coloured by a token rather than by whatever the
    // author had open. Parsing needs no GPU, so all of that is checkable here.
    void TestArtwork() {
        Section("artwork");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        EditorState& state = layer.State();
        Canvas& canvas = layer.Surface();

        const std::string name = "Selftest artwork";
        const std::filesystem::path folder = FileSystem::ProjectsRoot() / name;
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
        layer.CreateProject(name);

        const std::filesystem::path source = folder / "mark.svg";
        {
            std::ofstream out(source);
            out << R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"
                    width="24" height="24" fill="none" stroke="currentColor" stroke-width="2">
                  <path d="M4 12h6l2 6 4-12 2 6h2"/>
                </svg>)SVG";
        }

        const Uuid asset = state.ImportAsset(source);
        if (!Check(asset.Valid(), "an SVG imports: " + state.AssetError())) return;
        canvas.Assets().Rebind(state.Doc());
        Check(canvas.Assets().IsVector(asset), "and is known to be artwork, not a picture");
        Check(canvas.Assets().ProblemWith(asset).empty(),
              "it reads: " + canvas.Assets().ProblemWith(asset));
        Check(canvas.Assets().SizeOf(asset).x == 24.0f, "at the size the file states");
        Check(canvas.Assets().FollowsText(asset),
              "and it asked to be told what colour to be");

        const Uuid placed = state.PlaceArtwork(asset, state.ActiveScreen(), { 40.0f, 40.0f },
                                               canvas.Assets().SizeOf(asset),
                                               canvas.Assets().FollowsText(asset));
        driver.Frame();
        const doc::Node* node = state.Doc().Find(placed);
        if (!Check(node != nullptr, "it places")) return;
        Check(node->kind == doc::NodeKind::Vector,
              "as a Vector node — not an Image, and not an instance of anything");
        // A 24-pixel icon dropped at 24 pixels is a speck nobody can grab; the aspect survives.
        Check(node->layout.width.value >= 48.0f && node->layout.width.value <= 320.0f,
              "at a size you can see: " + std::to_string(node->layout.width.value));
        Check(node->layout.width.value == node->layout.height.value, "keeping its shape");

        const doc::Value ink = state.Doc().GetProp(placed, doc::Prop::Fill);
        Check(std::holds_alternative<doc::TokenRef>(ink)
              && std::get<doc::TokenRef>(ink).name == "text",
              "wearing the theme's text colour, as a token so it follows the theme");

        std::filesystem::remove_all(folder, ec);
    }


    // Translations: the file a translator is handed, and the canvas previewing one.
    void TestLanguages() {
        Section("languages");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        layer.OpenExample();
        driver.Frame();

        EditorState& state = layer.State();
        doc::Document& d = state.Doc();
        const std::filesystem::path project =
            FileSystem::ProjectsRoot() / "Selftest languages" / "Selftest languages.vae";
        std::error_code ec;
        std::filesystem::remove_all(project.parent_path(), ec);
        std::filesystem::create_directories(project.parent_path(), ec);

        const Uuid label = d.CreateNode(doc::NodeKind::Text, state.ActiveScreen(), "Greeting");
        d.SetProp(label, doc::Prop::Text, std::string("Good morning"));
        d.SetProp(label, doc::Prop::TextKey, std::string("home.greeting"));
        if (!Check(state.Save(project), "the project saves")) return;

        // The extraction: every key the document uses, with the authored text to translate.
        Check(state.WriteStrings("pt-BR"), "writing a translation file");
        const std::filesystem::path file =
            doc::StringsDirFor(project) / "pt-BR.json";
        Check(std::filesystem::exists(file), "strings/pt-BR.json is there");

        doc::StringTable table;
        std::string error;
        Check(table.Load(file, &error), "and it loads: " + error);
        Check(std::string(table.Find("home.greeting")) == "Good morning",
              "with the authored text as the starting point");

        // Translate it, and preview it: the canvas draws the translation, the document keeps
        // what was authored.
        table.Set("home.greeting", "Bom dia");
        Check(table.Save(file), "the translation saves");
        state.SetLocale("pt-BR");
        driver.Frame();

        const ui::ViewTree& tree = layer.Surface().Host().Tree();
        const auto drawn = [&] {
            for (u32 i = 0; i < tree.ViewCount(); ++i)
                if (tree.At(i).name == "Greeting") return tree.Str(i, doc::Prop::Text);
            return std::string{};
        };
        Check(tree.Strings() != nullptr, "the canvas is previewing a language");
        Check(d.GetProp(label, doc::Prop::Text) == doc::Value{ std::string("Good morning") },
              "and the document still says what the designer wrote");

        // Back to the authored text.
        state.SetLocale({});
        driver.Frame();
        Check(layer.Surface().Host().Tree().Strings() == nullptr, "and back to as authored");
        Check(drawn() == "Good morning", "which is what it draws again");

        // A locale with no file is not an error: the app draws what was authored.
        state.SetLocale("xx-YY");
        driver.Frame();
        Check(state.Locale() == "xx-YY", "a missing locale is still selected");

        std::filesystem::remove_all(project.parent_path(), ec);
    }


    // A repeated container is a template until something fills it. On the canvas that
    // something is the sample rows the designer typed, and the moment the app runs it is the
    // app.
    void TestSampleRows() {
        Section("sample rows");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        layer.OpenExample();
        driver.Frame();

        EditorState& state = layer.State();
        doc::Document& d = state.Doc();
        const Uuid screen = state.ActiveScreen();

        const Uuid list = d.CreateNode(doc::NodeKind::Frame, screen, "Messages");
        {
            doc::Node* node = d.Find(list);
            node->layout.mode = layout::LayoutMode::Stack;
            node->layout.axis = layout::Axis::Column;
            node->layout.offsetStart = { 24.0f, 24.0f };
            node->layout.width = layout::Size::Px(240.0f);
            node->layout.height = layout::Size::Hug();
            node->layout.gap = 4.0f;
        }
        d.SetProp(list, doc::Prop::Repeat, 2.0f);
        d.SetProp(list, doc::Prop::Sample, std::string("author\nAda\nGrace\nAlan\n"));

        const Uuid row = d.CreateNode(doc::NodeKind::Frame, list, "Message");
        {
            doc::Node* node = d.Find(row);
            node->layout.mode = layout::LayoutMode::Stack;
            node->layout.width = layout::Size::Fill();
            node->layout.height = layout::Size::Px(20.0f);
        }
        const Uuid label = d.CreateNode(doc::NodeKind::Text, row, "Author");
        d.SetProp(label, doc::Prop::Field, std::string("author"));
        driver.Frame();

        ui::UiHost& host = layer.Surface().Host();
        const auto copies = [&] {
            const ui::ViewTree& tree = host.Tree();
            u32 count = 0;
            for (u32 i = 0; i < tree.ViewCount(); ++i)
                if (tree.At(i).name.rfind("Message ", 0) == 0) ++count;
            return count;
        };
        const auto drew = [&](i32 which) {
            const ui::ViewTree& tree = host.Tree();
            for (u32 i = 0; i < tree.ViewCount(); ++i)
                if (tree.At(i).name == "Author" && tree.At(i).row == which)
                    return tree.Str(i, doc::Prop::Text);
            return std::string{};
        };

        Check(copies() == 3, "the canvas draws one copy per sample row, not the placeholder count");
        Check(drew(0) == "Ada" && drew(2) == "Alan",
              "and every copy drew its own row, so the bindings are visible while they are drawn");

        // Half a table is the state the field spends most of its life in.
        d.SetProp(list, doc::Prop::Sample, std::string("author"));
        driver.Frame();
        Check(copies() == 2, "column names with nothing under them leave the placeholder alone");

        d.SetProp(list, doc::Prop::Sample, std::string("author\nAda\nGrace\nAlan\n"));
        driver.Frame();

        // And it is a drawing aid, not content: pressing Play must not leave invented people
        // in a running app.
        ScriptSession& scripts = layer.Scripts();
        driver.Press(ImGuiKey_F5);
        driver.Frame();
        if (Check(scripts.Playing(), "F5 starts the app")) {
            Check(copies() == 2, "the sample rows are gone the moment it runs");
            Check(drew(0).empty(), "and nothing invented is left on screen");
        }

        driver.Press(ImGuiKey_F5, false, true);
        driver.Frame();
        Check(!scripts.Playing(), "stopped");
        Check(copies() == 3, "and the designer gets their sample rows back");
    }


    // A container that fills from its far edge, which is what a chat log is. Checked on the
    // canvas because the failure it replaces was a visual one: `justify: end` pushing the
    // newest message out of the top of the box the moment the conversation overflowed.
    void TestFillFromTheEnd() {
        Section("fills from the end");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        layer.OpenExample();
        driver.Frame();

        EditorState& state = layer.State();
        doc::Document& d = state.Doc();

        const Uuid log = d.CreateNode(doc::NodeKind::Frame, state.ActiveScreen(), "Log");
        {
            doc::Node* node = d.Find(log);
            node->layout.mode = layout::LayoutMode::Stack;
            node->layout.axis = layout::Axis::Column;
            node->layout.offsetStart = { 24.0f, 24.0f };
            node->layout.width = layout::Size::Px(240.0f);
            node->layout.height = layout::Size::Px(120.0f);
        }
        d.SetProp(log, doc::Prop::ClipContent, true);
        d.SetProp(log, doc::Prop::Role, std::string("scroll"));
        d.SetProp(log, doc::Prop::StickToEnd, true);

        const auto message = [&](const std::string& name) {
            const Uuid id = d.CreateNode(doc::NodeKind::Frame, log, name);
            doc::Node* node = d.Find(id);
            node->layout.width = layout::Size::Fill();
            node->layout.height = layout::Size::Px(30.0f);
            d.Touch(id);
            return id;
        };
        const Uuid first = message("One");
        message("Two");
        const Uuid third = message("Three");
        driver.Frame();

        ui::UiHost& host = layer.Surface().Host();
        const auto box = [&](Uuid id) {
            const ui::ViewTree& tree = host.Tree();
            return tree.Bounds(tree.ViewOf(ui::WidgetId{ id, Uuid::Invalid() }));
        };
        const auto scroll = [&] {
            const ui::ViewTree& tree = host.Tree();
            return tree.At(tree.ViewOf(ui::WidgetId{ log, Uuid::Invalid() })).scroll.y;
        };

        Check(Near(box(third).Bottom(), box(log).Bottom(), 1.0f),
              "three short messages sit against the bottom of the box");
        Check(box(first).pos.y > box(log).pos.y,
              "a short conversation is held down, not floating under the title");
        Check(Near(scroll(), 0.0f), "and nothing scrolled to do it");

        // Past the box: the same property becomes scrolling, still showing the newest.
        for (int i = 4; i <= 8; ++i) message("Message " + std::to_string(i));
        driver.Frame();
        Check(scroll() > 0.0f, "a conversation longer than the box scrolls instead");
        Check(Near(box(d.Find(log)->children.back()).Bottom(), box(log).Bottom(), 1.0f),
              "and the newest message is the one on screen");
    }

    void RunAuthoring() {
        TestInspectorRoundTrip();
        TestBreakpointAuthoring();
        TestMarkupEditing();
        TestLanguages();
        TestSampleRows();
        TestFillFromTheEnd();
        TestBlocksComeFromTheCatalog();
        TestStyling();
        TestAssets();
        TestArtwork();
        TestComponentProperties();
        TestAuthoringAComponent();
    }

}
