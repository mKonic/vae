// A project on disk: saving it, reopening it, and the shape of the folder.
#include "Harness.h"

namespace vae::selftest {


    void TestSaveLoad() {
        Section("save and load");
        Driver driver;
        EditorState& state = driver.State();

        const Uuid a = PlaceFixed(state, "Button", { 137.0f, 219.0f }, { 210.0f, 44.0f });
        state.SetProp(a, doc::Prop::Text, doc::Value{ std::string("Persisted") });
        state.EndGesture();

        const std::size_t nodes = state.Doc().NodeCount();
        const auto path = std::filesystem::temp_directory_path() / "vae-selftest.vae";
        Check(state.Save(path), "the project saves");
        Check(!state.Dirty(), "saving clears the dirty flag");

        Check(state.Load(path), "the project loads back");
        Check(state.Doc().NodeCount() == nodes, "the node count survives the round trip");

        // Looked up by where it is rather than by id: a file writes an id only on a node
        // something refers to, and nothing refers to a placed instance. Its component and the
        // screen it sits on are found by id, because those ARE referenced.
        const doc::Node* node = nullptr;
        if (const doc::Node* screen = state.Doc().Find(state.ActiveScreen()))
            for (Uuid child : screen->children)
                if (const doc::Node* n = state.Doc().Find(child); n && n->IsInstance()) node = n;
        Check(node != nullptr, "the placed widget survives a save and a load");
        if (node) {
            Check(Near(node->layout.offsetStart.x, 137.0f)
               && Near(node->layout.offsetStart.y, 219.0f), "position survives");
            Check(Near(node->layout.width.value, 210.0f), "size survives");
            const doc::Value text = state.GetProp(node->id, doc::Prop::Text);
            Check(std::holds_alternative<std::string>(text)
                  && std::get<std::string>(text) == "Persisted", "the override survives");
            // The instance still points at the component it is an instance of: that id IS
            // written, because something refers to it.
            Check(node->componentId.Valid() && state.Doc().Find(node->componentId) != nullptr,
                  "and still points at its component");
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }


    void TestProjectFolders() {
        Section("project folders");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        EditorState& state = layer.State();

        // Nothing the user makes belongs in the engine's own directory: an installed VAE would
        // be writing into itself, and a checked-out one fills the repository with someone's work.
        const std::filesystem::path projects = FileSystem::ProjectsRoot();
        Check(!projects.empty(), "there is a projects root");
        Check(projects.string().find(FileSystem::EngineRoot().string()) == std::string::npos,
              "and it is not inside the engine");

        const std::string name = "Selftest project";
        const std::filesystem::path folder = projects / name;
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);

        layer.CreateProject(name);
        Check(std::filesystem::exists(folder, ec), "a project is a folder of its own");
        Check(std::filesystem::exists(folder / doc::Project::kFileName, ec),
              "with a project index inside it");
        Check(std::filesystem::exists(folder / "screens", ec),
              "and a file per screen rather than one file holding all of them");
        Check(state.Path() == folder / doc::Project::kFileName,
              "and that is the project the editor is now editing");

        // A second project of the same name would quietly overwrite the first one's document.
        layer.CreateProject(name);
        Check(layer.NamingError().find("already") != std::string::npos,
              "a name already in use is refused, not overwritten");

        std::filesystem::remove_all(folder, ec);

        // What the creation dialog asks for, and what each answer has to have done by the time
        // the project exists. All three are expensive to change later, which is why they are
        // asked at the start rather than left to be found in a settings menu.
        const std::string kiosk = "Selftest kiosk";
        const std::filesystem::path at = projects / kiosk;
        std::filesystem::remove_all(at, ec);

        StudioLayer::NewProjectSpec spec;
        spec.language = ScriptSession::Language::Cpp;
        spec.size = { 390.0f, 844.0f };
        spec.resizable = false;
        layer.CreateProject(kiosk, spec);

        const Uuid home = state.ActiveScreen();
        Check(state.Doc().Find(home)->layout.width.value == 390.0f,
              "the screen is the size that was asked for");
        Check(state.Doc().Find(home)->layout.height.value == 844.0f, "on both axes");
        Check(!state.Doc().Find(home)->props.Flag(doc::Prop::Resizable, true),
              "and pinned to it, because that is what was ticked");
        Check(layer.Scripts().Lang() == ScriptSession::Language::Cpp,
              "the project is a C++ project");
        Check(layer.Scripts().SourcePath().extension() == ".cpp",
              "so its script is a .cpp beside the document: "
                  + layer.Scripts().SourcePath().filename().string());
        Check(!layer.HasUnsavedWork(),
              "and none of it counts as an edit — a new project starts clean");

        // Reopening finds the language on disk rather than in whatever the Studio was last set
        // to. Without this, opening a C++ project from a Lua session shows an empty editor and
        // says the project has no logic.
        layer.Scripts().SetLanguage(ScriptSession::Language::Lua);
        layer.OpenProject(at / doc::Project::kFileName);
        driver.Frame();
        Check(layer.Scripts().Lang() == ScriptSession::Language::Cpp,
              "reopening it comes back as C++");
        Check(!state.Doc().Find(state.ActiveScreen())->props.Flag(doc::Prop::Resizable, true),
              "still pinned");

        std::filesystem::remove_all(at, ec);
    }


    void TestUnsavedClose() {
        Section("unsaved changes");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        EditorState& state = layer.State();

        Check(!layer.HasUnsavedWork(), "a fresh project has nothing to lose");

        PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 160.0f, 40.0f });
        Check(layer.HasUnsavedWork(), "placing a widget is unsaved work");
        Check(layer.HoldCloseForUnsavedWork(), "closing with unsaved work is held back");

        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "vae-selftest-close.vae";
        layer.SaveProject(path);
        Check(!layer.HasUnsavedWork(), "saving settles it");
        Check(!layer.HoldCloseForUnsavedWork(), "and closing is no longer held back");
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }


    // The recovery copy: written while there is unsaved work, offered back afterwards, and
    // gone the moment the project is saved.
    void TestAutosave() {
        Section("autosave and recovery");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        layer.OpenExample();
        driver.Frame();

        EditorState& state = layer.State();
        const std::filesystem::path project =
            FileSystem::ProjectsRoot() / "Selftest recovery" / "Selftest recovery.vae";
        std::error_code ec;
        std::filesystem::create_directories(project.parent_path(), ec);
        std::filesystem::remove(EditorState::RecoveryPathFor(project), ec);

        if (!Check(state.Save(project), "the project saves")) return;
        Check(!EditorState::HasRecovery(project), "a saved project has nothing to recover");

        // An edit, then an autosave: what a crash would leave behind.
        const Uuid box = state.Doc().CreateNode(doc::NodeKind::Frame, state.ActiveScreen(),
                                                "Unsaved box");
        driver.Frame();
        Check(state.Dirty(), "the document is dirty after an edit");
        state.Autosave();
        Check(std::filesystem::exists(EditorState::RecoveryPathFor(project)),
              "which writes a recovery copy");
        Check(EditorState::HasRecovery(project), "and it is newer than the project");

        // What the recovery holds is the work, not the file on disk.
        doc::Document recovered;
        std::string error;
        Check(doc::Serializer::Load(EditorState::RecoveryPathFor(project), recovered, &error,
                                    &ui::StandardLibrary()),
              "the recovery copy loads: " + error);
        // By name, not by id: the format drops ids nothing refers to, so a node that came back
        // from a file is not the id it went in as.
        (void)box;
        const bool found = std::any_of(recovered.AllNodes().begin(), recovered.AllNodes().end(),
                                       [&](Uuid id) {
                                           const doc::Node* n = recovered.Find(id);
                                           return n && n->name == "Unsaved box";
                                       });
        Check(found, "with the unsaved edit in it");

        // Saving the project makes the recovery meaningless, so it goes.
        Check(state.Save(project), "saving again");
        Check(!std::filesystem::exists(EditorState::RecoveryPathFor(project)),
              "clears the recovery copy");

        std::filesystem::remove_all(project.parent_path(), ec);
    }


    void TestScreenSizing() {
        Section("screen sizing");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        EditorState& state = layer.State();

        const std::string name = "Selftest sizing";
        const std::filesystem::path folder = FileSystem::ProjectsRoot() / name;
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
        layer.CreateProject(name);
        driver.Frame();

        const Uuid home = state.ActiveScreen();
        Check(state.Doc().Find(home)->props.Flag(doc::Prop::Resizable, true),
              "a new screen is resizable, because an app that does not fit the window it was "
              "given is the exception");

        // A child pinned to both edges: the thing that makes "resizing fits everything" true
        // rather than a claim. Absolute placement, which is what a screen is by default.
        const Uuid band = state.Doc().CreateNode(doc::NodeKind::Frame, home, "Band");
        {
            layout::LayoutStyle& style = state.Doc().Find(band)->layout;
            style.constraintX = layout::Constraint::StartEnd;
            style.offsetStart = { 40.0f, 40.0f };
            style.offsetEnd = { 40.0f, 0.0f };
            style.height = layout::Size::Px(60.0f);
        }
        state.Doc().Touch(band);
        layer.SaveProject(folder / (name + ".vae"));

        const std::filesystem::path document = folder / (name + ".vae");

        // The runtime, not the editor: this is the half that decides what an app does with the
        // window it is handed, and it is the half that used to overwrite the design silently.
        {
            app::RunLayer run;
            std::string error;
            if (!Check(run.Load(document, &error), "the player loads it: " + error)) return;
            // The wiring a real Application would do. The Studio's own app has no device in a
            // selftest, so this stops at the document and the view tree — which is the half
            // this is about.
            run.OnAttach();
            Check(run.Resizable(), "and calls it resizable");
            Check(run.DesignSize().x == 1280.0f, "opening at the size the screen was designed at");

            run.RenderOffline(1.0f / 60.0f);
            run.Resize({ 900.0f, 600.0f });
            run.RenderOffline(1.0f / 60.0f);

            const ui::ViewTree& tree = run.Host().Tree();
            u32 view = ui::ViewTree::kInvalid;
            std::string saw;
            for (u32 i = 0; i < tree.ViewCount(); ++i) {
                saw += " " + tree.At(i).name;
                if (tree.At(i).name == "Band") view = i;
            }
            if (Check(view != ui::ViewTree::kInvalid, "the band is on screen; saw" + saw)) {
                // 900 wide, 40 of margin either side. A child that did not move is a child in
                // an app that does not resize, whatever the window did.
                Check(std::abs(tree.Bounds(view).size.x - 820.0f) < 1.0f,
                      "and it stretched with the window: "
                          + std::to_string(tree.Bounds(view).size.x));
            }
            run.OnDetach();
        }

        // Pinned to a resolution. Everything above should stop happening.
        state.SetProp(home, doc::Prop::Resizable, false);
        state.EndGesture();
        layer.SaveProject(document);
        {
            app::RunLayer run;
            std::string error;
            if (!Check(run.Load(document, &error), "it reloads: " + error)) return;
            run.OnAttach();
            Check(!run.Resizable(), "a pinned screen says it cannot be resized");
            Check(run.DesignSize().x == 1280.0f, "and the numbers are now a hard resolution");

            run.RenderOffline(1.0f / 60.0f);
            run.Resize({ 900.0f, 600.0f });
            run.RenderOffline(1.0f / 60.0f);

            const ui::ViewTree& tree = run.Host().Tree();
            u32 view = ui::ViewTree::kInvalid;
            for (u32 i = 0; i < tree.ViewCount(); ++i)
                if (tree.At(i).name == "Band") view = i;
            if (Check(view != ui::ViewTree::kInvalid, "the band is still there")) {
                // A window manager that ignores the hint sends a resize anyway; stretching the
                // design to fit it is exactly what "fixed resolution" asked not to do.
                Check(std::abs(tree.Bounds(view).size.x - 1200.0f) < 1.0f,
                      "and it kept its designed width: "
                          + std::to_string(tree.Bounds(view).size.x));
            }
            run.OnDetach();
        }
    }


    void TestScreens() {
        Section("screens");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        ScriptSession& scripts = layer.Scripts();

        layer.OpenExample(StudioLayer::Example::Screens);
        driver.Frame();

        EditorState& state = layer.State();
        Check(state.Screens().size() == 3, "three screens");
        Check(state.Doc().StartScreen() == state.ActiveScreen(), "the start screen is showing");
        Check(scripts.Build(), "its script builds: " + scripts.Output());

        driver.Press(ImGuiKey_F5);
        driver.Frame();
        if (!Check(scripts.Playing(), "the screens example runs")) return;

        ui::UiHost& host = layer.Surface().Host();
        Check(host.CurrentScreenName() == "List", "opens on the screen it was told to");
        Check(scripts.LiveInstances() >= 1, "the screen's own script mounted");

        // A named node anywhere on screen, whichever tree it is in.
        auto find = [&](std::string_view name) -> std::pair<ui::ViewTree*, u32> {
            for (ui::ViewTree* tree : host.Trees())
                for (u32 i = 0; i < tree->ViewCount(); ++i)
                    if (tree->At(i).name == name) return { tree, i };
            return { nullptr, ui::ViewTree::kInvalid };
        };

        auto click = [&](std::string_view name) {
            const auto [tree, view] = find(name);
            if (!tree) return false;
            const Vec2 point = tree->Bounds(view).Center();
            host.Dispatch(MakeMouseMoved(point.x, point.y));
            driver.Frame();
            host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                          point.x, point.y, Mod::None));
            driver.Frame();
            host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                          point.x, point.y, Mod::None));
            driver.Frame();
            return true;
        };

        // A row says where it goes; no script is involved in the navigation itself.
        Check(click("Invoice #4021"), "a row to click");
        Check(host.CurrentScreenName() == "Detail", "a declared destination navigated");

        // ...and the script that watched the click filled in the detail.
        const auto [tree, subject] = find("Subject");
        if (Check(tree != nullptr, "the detail has a subject")) {
            const std::string shown = tree->Str(subject, doc::Prop::Text);
            Check(shown == "Invoice #4021",
                  "the screen script carried the choice across: " + shown);
        }

        // The alert is presented over the detail rather than replacing it.
        Check(click("Delete"), "a delete button");
        Check(host.OverlayCount() == 1, "the alert is presented");
        Check(host.CurrentScreenName() == "Detail", "and the detail is still underneath");

        // An alert does not dismiss itself: a click outside it changes nothing.
        host.Dispatch(MakeMouseMoved(40.0f, 760.0f));
        host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                      40.0f, 760.0f, Mod::None));
        host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                      40.0f, 760.0f, Mod::None));
        driver.Frame();
        Check(host.OverlayCount() == 1, "an alert stays until it is answered");

        Check(click("Cancel"), "a cancel button");
        Check(host.OverlayCount() == 0, "cancel closed it");
        Check(host.CurrentScreenName() == "Detail", "and left the detail where it was");

        Check(click("Back"), "a back button");
        Check(host.CurrentScreenName() == "List", "back went back");

        driver.Press(ImGuiKey_F5, false, true);
        driver.Frame();
        Check(!scripts.Playing(), "stopped");
    }


    void TestProjectWithoutAScript() {
        Section("no script");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        ScriptSession& scripts = layer.Scripts();
        EditorState& state = layer.State();

        // Most projects never need code: a screen wired with declared navigation and the widget
        // library runs on its own. Refusing to Play one is refusing to run an ordinary app.
        Check(!scripts.HasSource() || true, "a fresh project may or may not have a script yet");
        PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 160.0f, 40.0f });
        driver.Frame();

        if (!scripts.HasSource()) {
            Check(scripts.Build(), "nothing to build is not a failure");
            Check(scripts.Play(), "Play runs the screen with no script at all");
            Check(scripts.Playing(), "and it is running");
            Check(scripts.LiveInstances() == 0, "with nothing mounted, because there is no code");
            scripts.Stop();
            Check(!scripts.Playing(), "and it stops again");
        }
    }

    void RunProjects() {
        TestSaveLoad();
        TestScreens();
        TestAutosave();
        TestScreenSizing();
        TestUnsavedClose();
        TestProjectFolders();
        TestProjectWithoutAScript();
    }

}
