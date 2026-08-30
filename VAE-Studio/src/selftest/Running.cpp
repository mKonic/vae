// Play, and the tools that watch it.
#include "Harness.h"

namespace vae::selftest {


    void TestPlayMode() {
        Section("play mode");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();

        layer.OpenExample();
        driver.Frame();
        driver.Frame();

        EditorState& state = layer.State();
        const Uuid left  = InstanceNamed(state.Doc(), state.ActiveScreen(), "Left");
        const Uuid right = InstanceNamed(state.Doc(), state.ActiveScreen(), "Right");
        if (!Check(left.Valid() && right.Valid(), "the example puts two counters on screen"))
            return;

        ScriptSession& scripts = layer.Scripts();
        Check(scripts.Lang() == ScriptSession::Language::Lua, "Lua by default");
        Check(scripts.Build(), "the example's script builds: " + scripts.Output());

        // F5 is the whole gesture: build if needed, snapshot, start, switch the canvas over.
        driver.Press(ImGuiKey_F5);
        if (!Check(scripts.Playing(), "F5 starts the app")) return;
        Check(layer.Surface().Preview(), "and the canvas stops looking like an editor");
        driver.Frame();
        Check(scripts.LiveInstances() == 2, "both copies mounted, not one shared script");

        const ui::ViewTree& tree = layer.Surface().Host().Tree();
        Check(TextIn(tree, left, "Count") == "0", "on_mount wrote through the designer's name");

        // Clicks go in as real pointer events at the coordinates the layout produced.
        auto click = [&](Uuid owner, std::string_view name) {
            const Rect box = BoundsIn(layer.Surface().Host().Tree(), owner, name);
            const Vec2 point = box.Center();
            ui::UiHost& host = layer.Surface().Host();
            host.Dispatch(MakeMouseMoved(point.x, point.y));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                          point.x, point.y, Mod::None));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                          point.x, point.y, Mod::None));
            driver.Frame();
        };

        click(left, "Increment");
        click(left, "Increment");
        click(right, "Increment");
        Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "2",
              "the left counter counted its own clicks: "
                  + TextIn(layer.Surface().Host().Tree(), left, "Count"));
        Check(TextIn(layer.Surface().Host().Tree(), right, "Count") == "1",
              "and the right one counted separately: "
                  + TextIn(layer.Surface().Host().Tree(), right, "Count"));

        // Hot reload: the code is swapped, the screen keeps saying what it said.
        driver.Press(ImGuiKey_F6);
        driver.Frame();
        Check(scripts.Playing(), "F6 leaves the app running");
        Check(scripts.LiveInstances() == 2, "and everything still mounted");
        Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "2",
              "live state survived the reload");

        click(left, "Reset");
        Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "0",
              "reload left a working script behind, not a dead one");
        Check(TextIn(layer.Surface().Host().Tree(), right, "Count") == "1",
              "and reset stayed inside the copy it was clicked in");

        // Stopping puts the design back: what the script wrote is not what the designer drew.
        driver.Press(ImGuiKey_F5, false, true);
        driver.Frame();
        Check(!scripts.Playing(), "Shift+F5 stops");
        Check(!layer.Surface().Preview(), "and the editor comes back");
        Check(TextIn(layer.Surface().Host().Tree(), right, "Count") == "0",
              "the document is the design again, not the last frame of the app: "
                  + TextIn(layer.Surface().Host().Tree(), right, "Count"));
        Check(state.Doc().Find(left) != nullptr && state.Doc().Find(right) != nullptr,
              "restored in place, ids and all");

        // The same example in the other language, through the compiler this time. It is the
        // only thing that catches a C++ template that stopped compiling against the header.
        scripts.SetLanguage(ScriptSession::Language::Cpp);
        layer.OpenExample();
        driver.Frame();
        Check(scripts.Build(), "the example's C++ builds too: " + scripts.Output());

        driver.Press(ImGuiKey_F5);
        driver.Frame();
        if (Check(scripts.Playing(), "and runs")) {
            const Uuid a = InstanceNamed(state.Doc(), state.ActiveScreen(), "Left");
            Check(scripts.LiveInstances() == 2, "with both copies mounted");
            Check(TextIn(layer.Surface().Host().Tree(), a, "Count") == "0",
                  "showing what the C++ on_mount wrote");
            driver.Press(ImGuiKey_F5, false, true);
        }
    }


    // ------------------------------------------------------------------------ debug tools

    void TestDebugger() {
        Section("debug tools");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        ScriptSession& scripts = layer.Scripts();
        Debugger& debugger = scripts.Debug();

        layer.OpenExample();
        driver.Frame();
        EditorState& state = layer.State();
        const Uuid left = InstanceNamed(state.Doc(), state.ActiveScreen(), "Left");
        if (!Check(left.Valid(), "a counter to debug")) return;

        Check(scripts.Build(), "example builds: " + scripts.Output());
        driver.Press(ImGuiKey_F5);
        driver.Frame();
        if (!Check(scripts.Playing(), "running")) return;
        Check(debugger.Watches().empty(), "a run starts with nothing pinned");
        Check(debugger.Log().empty(), "and nothing recorded");

        // A press and a release on separate frames, which is what a real click is. It matters
        // here: anything that rebuilds the view tree between the two — a debugger writing a
        // frozen value into the document every frame, say — would drop the click, and a click
        // delivered inside one frame would never notice.
        auto click = [&](std::string_view name) {
            ui::UiHost& host = layer.Surface().Host();
            const Vec2 point = BoundsIn(host.Tree(), left, name).Center();
            host.Dispatch(MakeMouseMoved(point.x, point.y));
            driver.Frame();
            host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                          point.x, point.y, Mod::None));
            driver.Frame();
            host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                          point.x, point.y, Mod::None));
            driver.Frame();
        };

        // The event log records what reached which script, not merely that something happened.
        click("Increment");
        if (Check(!debugger.Log().empty(), "the click was recorded")) {
            const Debugger::Traced& entry = debugger.Log().back();
            Check(entry.kind == "clicked", "as a click: " + entry.kind);
            Check(entry.source == "Increment", "from the node the designer named: " + entry.source);
            Check(entry.instance == left, "delivered to the copy that was clicked");
        }
        Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "1", "and the script ran");

        // Reading a state key the way the panel does.
        debugger.Add({ left, "Left", {}, "count", true });
        Check(debugger.Watches().size() == 1, "pinned");
        debugger.Add({ left, "Left", {}, "count", true });
        Check(debugger.Watches().size() == 1, "and pinning it twice does not stack up");

        const doc::Value seen = debugger.Read(debugger.Watches().front(), scripts.Runtime(),
                                              layer.Surface().Host().Tree());
        Check(doc::TypeOf(seen) == doc::ValueType::Number && std::get<f32>(seen) == 1.0f,
              "the watch reads what the script is holding");

        // Writing one back: the debugger reaches into a live script, not a copy of it.
        debugger.Write(debugger.Watches().front(), scripts.Runtime(),
                       layer.Surface().Host().Tree(), doc::Value{ 41.0f });
        click("Increment");
        Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "42",
              "an edited state value is the one the script carries on from: "
                  + TextIn(layer.Surface().Host().Tree(), left, "Count"));

        // Freezing a property the script writes on every click. The script keeps winning until
        // the freeze is on, and then it stops winning — which is the whole feature.
        debugger.Add({ left, "Left", "Count", "text", false });
        Debugger::Watch& frozen = debugger.Watches().back();
        frozen.frozen = true;
        frozen.hold = std::string("held");
        driver.Frame();
        Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "held",
              "a frozen property is held: "
                  + TextIn(layer.Surface().Host().Tree(), left, "Count"));

        click("Increment");
        Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "held",
              "and stays held while the script writes it every click: "
                  + TextIn(layer.Surface().Host().Tree(), left, "Count"));
        const auto* counting = scripts.Runtime().StateOf(left);
        Check(counting && std::get<f32>(counting->at("count")) > 42.0f,
              "while the script itself kept counting underneath");

        debugger.Watches().back().frozen = false;
        click("Increment");
        Check(TextIn(layer.Surface().Host().Tree(), left, "Count") != "held",
              "unfreezing hands it back");

        // Stopping clears the debugger: a watch pinned to a dead instance is a trap.
        driver.Press(ImGuiKey_F5, false, true);
        driver.Frame();
        Check(debugger.Watches().empty(), "stopping forgets the watches");
        Check(debugger.Log().empty(), "and the log");
    }


    // Running the design in its own process, which is the only way to see what the app is
    // actually like: real window size, the desktop's chrome, and a script that cannot take the
    // editor down with it. Checked here without a window, by running the same player the same
    // way with --headless — the launch, the arguments and the project on disk are the parts
    // that break, and none of them need a screen to be wrong.
    void TestRunInAWindow() {
        Section("run in a window");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();

        const std::filesystem::path player = StudioLayer::PlayerPath();
        if (!Check(!player.empty(), "an install can find its own VAE-Player")) return;

        layer.OpenExample(StudioLayer::Example::Screens);
        driver.Frame();

        const std::filesystem::path project = FileSystem::ProjectsRoot() / "Screens example"
                                            / doc::Project::kFileName;
        layer.SaveProject(project);
        Check(std::filesystem::exists(project), "the project the window will open is on disk");

        // Exactly what RunInWindow launches, minus the window. A wrong path, a missing script
        // or a project the player cannot parse all come back as a non-zero exit here.
        const std::string command = platform::Quote(player) + " " + platform::Quote(project)
                                  + " --headless";
        const platform::Ran ran = platform::Run(command);
        Check(ran.Ok(), "the player runs the saved project: " + ran.output);

        // And the screen argument the "run this screen" item passes is one the player honours,
        // rather than one it warns about and ignores.
        const platform::Ran detail = platform::Run(command + " --screen Detail");
        Check(detail.Ok() && detail.output.find("no screen called") == std::string::npos,
              "and starts on the screen it was told to: " + detail.output);

        // The launch path itself: a process that starts, is seen to be running, and is asked
        // to close. --headless so this opens nothing on whatever display the tests are on.
        platform::Process process = platform::Launch(player, { project.string(), "--headless",
                                                               "--frames", "600" });
        if (Check(process != 0, "the player launches detached")) {
            Check(platform::Running(process), "and is running once it has");
            platform::AskToClose(process);
            // Reaped, so a Studio that launches many runs does not leave zombies behind it.
            for (int i = 0; i < 200 && platform::Running(process); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            Check(!platform::Running(process), "and closes when asked");
        }
    }


    // The three things a screen that talks to something needs, checked where they live: in
    // the document, so the canvas can show each of them with nothing running.
    void TestFeedExample() {
        Section("feed example");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        ScriptSession& scripts = layer.Scripts();

        layer.OpenExample(StudioLayer::Example::Feed);
        driver.Frame();
        EditorState& state = layer.State();
        doc::Document& d = state.Doc();

        const auto named = [&](std::string_view name) {
            for (Uuid id : d.Subtree(state.ActiveScreen()))
                if (const doc::Node* node = d.Find(id); node && node->name == name) return id;
            return Uuid::Invalid();
        };

        const Uuid panel = named("Panel");
        if (!Check(panel.Valid(), "the panel is there")) return;
        for (const char* state_ : { "Loading", "Failed", "Empty", "Content" })
            Check(named(state_).Valid(), std::string("and its ") + state_ + " state");

        const doc::Value shown = d.GetProp(panel, doc::Prop::Shown);
        Check(std::holds_alternative<std::string>(shown), "one of them is named as the one shown");

        // Which one is showing is a property, so the canvas draws exactly that one and a
        // designer can look at the failure without breaking anything.
        ui::UiHost& host = layer.Surface().Host();
        const auto visible = [&](std::string_view name) {
            const ui::ViewTree& tree = host.Tree();
            for (u32 i = 0; i < tree.ViewCount(); ++i)
                if (tree.At(i).name == name) return tree.At(i).visible;
            return false;
        };
        Check(visible("Loading"), "the canvas shows it");
        Check(!visible("Failed"), "and only it");

        d.SetProp(panel, doc::Prop::Shown, std::string("Failed"));
        driver.Frame();
        Check(visible("Failed") && !visible("Loading"),
              "switching is one property, which is what makes it something a script can do");

        Check(named("Rows").Valid(), "there is a table for the answer to go into");
        Check(scripts.Build(), "its script builds: " + scripts.Output());
    }


    void TestExampleSound() {
        Section("sound");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        EditorState& state = layer.State();
        Canvas& canvas = layer.Surface();
        ScriptSession& scripts = layer.Scripts();

        layer.OpenExample(StudioLayer::Example::Counter);
        driver.Frame();

        // The example generates its own click rather than shipping one, so the first thing to
        // know is that the file the document names is actually on disk.
        const std::filesystem::path folder = FileSystem::ProjectsRoot() / "Counter example";
        std::error_code ec;
        const std::filesystem::path wav = folder / "assets" / "click.wav";
        if (!Check(std::filesystem::exists(wav, ec), "the example wrote its click sound"))
            return;
        Check(std::filesystem::file_size(wav, ec) > 44,
              "and it is a wav with samples in it, not just a header");

        Uuid click = Uuid::Invalid();
        for (const doc::Document::Asset& asset : state.Doc().Assets())
            if (asset.name == "click") click = asset.id;
        if (!Check(click.Valid(), "registered under the name the script plays it by")) return;

        canvas.Assets().Rebind(state.Doc());
        Check(canvas.Assets().IsSound(click), "the asset store knows it is a sound");
        Check(canvas.Assets().ProblemWith(click).empty(),
              "and finds it: " + canvas.Assets().ProblemWith(click));
        // Nothing about a sound is drawn, so the panel must not go looking for pixels.
        Check(!canvas.Assets().IsVector(click), "it is not artwork");
        Check(canvas.Assets().Image(click) == nullptr, "and there is no texture behind it");

        Check(scripts.Buffer().find("play_sound") != std::string::npos,
              "the example's script plays it");
        Check(scripts.Build(), "which builds: " + scripts.Output());

        // The editor's own speaker, which is what the Assets panel's play button uses. Opened
        // silently, because a selftest that makes a noise on a build machine is a selftest
        // nobody runs twice — and because there may be no sound card at all.
        std::string trouble;
        if (Check(state.Preview().OpenSilent(&trouble), "the preview speaker opens: " + trouble)) {
            Check(state.PreviewAsset(click).empty(), "and plays the asset by id");
            Check(state.Preview().Voices() == 1, "one sound, not a pile of them");
            state.PreviewAsset(click);
            Check(state.Preview().Voices() == 1, "playing again replaces it rather than layering");
            state.Preview().StopAll();
            Check(state.Preview().Voices() == 0, "and stops");
        }
    }

    void RunRunning() {
        TestPlayMode();
        TestDebugger();
        TestFeedExample();
        TestExampleSound();
        TestRunInAWindow();
    }

}
