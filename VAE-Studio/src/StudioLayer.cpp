#include "StudioLayer.h"

#include "Blocks.h"
#include "Example.h"

#include "vae/gen/Emit.h"
#include "panels/Panels.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"
#include "vae/base/Version.h"
#include "vae/core/Application.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <sstream>

namespace vae {

    void StudioLayer::OnAttach() {
        InitConsolePanel();
        LoadRecents();
        LoadSettings();
        std::snprintf(m_OpenPath, sizeof m_OpenPath, "%s", DefaultProjectPath().string().c_str());
        auto& app = Application::Get();
        if (app.HasDevice()) m_Canvas.Init(app.GetDevice());
        m_Scripts.Attach(m_Canvas, m_State);
        VAE_INFO("VAE Studio ready — {} components in the library",
                 m_State.Library().components.size());

        // Dropped from the desktop: a document opens, anything else is imported as an asset. This
        // is the shortest path from "I have a PNG" to "it is in the project", and every design tool
        // has it.
        if (app.HasWindow()) {
            app.GetWindow().SetFileDropCallback(
                [this](const std::vector<std::filesystem::path>& files) { OnFilesDropped(files); });
        }

        // A project named on the command line opens it. `vae-studio thing.vaescreen` is what
        // anyone types, and it is what a file manager hands over on "open with" — being shown the
        // launcher instead is the editor ignoring what it was asked for.
        for (int i = 1; i < app.Spec().args.count; ++i) {
            const std::string_view arg = app.Spec().args[i];
            if (arg.empty() || arg.starts_with("-")) continue;
            const std::filesystem::path path{ arg };
            if (!std::filesystem::exists(path)) {
                VAE_WARN("studio: no project at {}", path.string());
                continue;
            }
            OpenProject(path);
            m_ShowLauncher = false;
            break;
        }
    }

    void StudioLayer::OnDetach() { m_Canvas.Shutdown(); }

    void StudioLayer::OnRender(gpu::CommandList& cmd) { m_Canvas.OnRender(cmd, m_State); }

    void StudioLayer::DrawDockSpace() {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
                               | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
                               | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus
                               | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##StudioRoot", nullptr, flags);
        ImGui::PopStyleVar(3);

        const ImGuiID dockspace = ImGui::GetID("StudioDockSpace");
        if (!m_LayoutBuilt && !ImGui::DockBuilderGetNode(dockspace)) BuildDefaultLayout(dockspace);
        m_LayoutBuilt = true;
        ImGui::DockSpace(dockspace, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

        DrawMenuBar();
        ImGui::End();
    }

    void StudioLayer::BuildDefaultLayout(unsigned dockspace) {
        // Only runs when imgui.ini has no saved layout: once a user has arranged their panels, the
        // editor must never rearrange them behind their back.
        ImGui::DockBuilderRemoveNode(dockspace);
        ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);

        ImGuiID centre = dockspace;
        const ImGuiID left  = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.20f, nullptr, &centre);
        const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.26f, nullptr, &centre);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.24f, nullptr, &centre);
        ImGuiID leftBottom = left;
        const ImGuiID leftTop = ImGui::DockBuilderSplitNode(leftBottom, ImGuiDir_Up, 0.55f,
                                                            nullptr, &leftBottom);

        ImGui::DockBuilderDockWindow("Layers###Layers", leftTop);
        ImGui::DockBuilderDockWindow("Screens###Screens", leftBottom);
        ImGui::DockBuilderDockWindow("Components###Components", leftBottom);
        // The script editor is a tab on the canvas, not a strip under it: it is a place you work,
        // the same kind of place the canvas is, and a code editor squeezed in beside a log is a
        // code editor nobody can read what they are typing in.
        ImGui::DockBuilderDockWindow("Canvas###Canvas", centre);
        ImGui::DockBuilderDockWindow("Script###Script", centre);
        ImGui::DockBuilderDockWindow("Inspector###Inspector", right);
        ImGui::DockBuilderDockWindow("Assets###Assets", right);
        ImGui::DockBuilderDockWindow("Console###Console", bottom);
        ImGui::DockBuilderDockWindow("Files###Files", bottom);
        ImGui::DockBuilderDockWindow("Runtime###Runtime", bottom);
        ImGui::DockBuilderFinish(dockspace);
    }

    // Where an unnamed project would go if it were saved right now. A folder of its own under the
    // projects root, never the engine's directory: an installed VAE has no business writing into
    // itself, and a checked-out one has no business filling the repository with someone's work.
    std::filesystem::path StudioLayer::DefaultProjectPath() const {
        return FileSystem::ProjectsRoot() / "Untitled" / "Untitled.vaescreen";
    }

    // Recent projects are the launcher's whole reason to exist, so they are persisted next to the
    // window layout rather than kept in memory and forgotten on exit.
    void StudioLayer::LoadRecents() {
        m_Recent.clear();
        const auto text = FileSystem::ReadText(FileSystem::ConfigRoot() / "recent-projects.txt");
        if (!text) return;
        std::istringstream stream(*text);
        std::string line;
        while (std::getline(stream, line) && m_Recent.size() < 8)
            if (!line.empty() && std::filesystem::exists(line)) m_Recent.push_back(line);
    }

    // Editor preferences — not document facts, and not the window layout ImGui already persists in
    // imgui.ini. Rulers, transitions and preview mode are the three the View menu offers, and an
    // editor that forgets them every launch is an editor you re-configure every launch.
    void StudioLayer::LoadSettings() {
        const auto text = FileSystem::ReadText(FileSystem::ConfigRoot() / "settings.txt");
        if (!text) return;

        ui::ViewTree::Motion motion = m_Canvas.Host().Tree().MotionSettings();
        std::istringstream stream(*text);
        std::string line;
        while (std::getline(stream, line)) {
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq);
            const bool on = line.substr(eq + 1) == "1";
            if (key == "rulers")      m_Canvas.SetRulers(on);
            else if (key == "preview") m_Canvas.SetPreview(on);
            else if (key == "transitions") motion.enabled = on;
        }
        m_Canvas.Host().Tree().SetMotion(motion);
    }

    void StudioLayer::SaveSettings() {
        std::string text;
        const auto write = [&text](const char* key, bool on) {
            text += key;
            text += on ? "=1\n" : "=0\n";
        };
        write("rulers", m_Canvas.Rulers());
        write("preview", m_Canvas.Preview());
        write("transitions", m_Canvas.Host().Tree().MotionSettings().enabled);
        FileSystem::WriteText(FileSystem::ConfigRoot() / "settings.txt", text);
    }

    void StudioLayer::RememberProject(const std::filesystem::path& path) {
        const std::string entry = path.string();
        std::erase(m_Recent, entry);
        m_Recent.insert(m_Recent.begin(), entry);
        if (m_Recent.size() > 8) m_Recent.resize(8);

        std::string text;
        for (const std::string& line : m_Recent) { text += line; text += '\n'; }
        FileSystem::WriteText(FileSystem::ConfigRoot() / "recent-projects.txt", text);
    }

    void StudioLayer::OnFilesDropped(const std::vector<std::filesystem::path>& files) {
        if (files.empty()) return;

        // A document wins over everything else in the drop: dropping a project and three pictures
        // together means "open this", not "import a .vaescreen as a picture".
        for (const std::filesystem::path& file : files) {
            const std::string ext = file.extension().string();
            if (ext == ".vaescreen" || ext == ".vaecomp" || ext == ".vaeproj") {
                if (HoldCloseForUnsavedWork()) return;
                OpenProject(file);
                m_ShowLauncher = false;
                return;
            }
        }

        u32 imported = 0;
        for (const std::filesystem::path& file : files) {
            if (!m_State.ImportAsset(file).Valid()) {
                VAE_WARN("dropped {}: {}", file.filename().string(),
                         m_State.AssetError().empty() ? "not imported" : m_State.AssetError());
                continue;
            }
            ++imported;
        }
        if (imported > 0) VAE_INFO("imported {} dropped file(s)", imported);
    }

    // A project and its logic are one thing: opening, saving or starting a new one moves the
    // script with it, and none of that may happen underneath a running app.
    void StudioLayer::OpenProject(const std::filesystem::path& path) {
        m_Scripts.Stop();
        if (!m_State.Load(path)) return;
        RememberProject(path);
        // Before SetProjectPath, which names the script file after the language.
        m_Scripts.AdoptLanguageFor(path);
        m_Scripts.SetProjectPath(path);
        // Images are stored relative to the project folder, so the folder has to be known before
        // anything tries to draw one.
        m_Canvas.Assets().SetRoot(path.parent_path());
        m_Canvas.Assets().Rebind(m_State.Doc());
        m_FrameNext = true;
    }

    void StudioLayer::SaveProject(const std::filesystem::path& path) {
        if (!m_State.Save(path)) return;
        RememberProject(path);
        m_Canvas.Assets().SetRoot(path.parent_path());
        if (m_Scripts.Dirty()) m_Scripts.SaveSource();
        m_Scripts.SetProjectPath(path);
    }

    bool StudioLayer::HasUnsavedWork() const {
        return m_State.Dirty() || m_Scripts.Dirty();
    }

    bool StudioLayer::HoldCloseForUnsavedWork() {
        if (!HasUnsavedWork()) return false;
        m_ClosePending = true;
        m_CloseAsked = false;   // the dialog opens itself on the next frame
        return true;
    }

    void StudioLayer::DrawUnsavedChangesDialog() {
        if (!m_ClosePending) return;
        if (!m_CloseAsked) {
            ImGui::OpenPopup("Unsaved changes###Unsaved");
            m_CloseAsked = true;
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Unsaved changes###Unsaved", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;

        const std::filesystem::path path = m_State.Path().empty() ? DefaultProjectPath()
                                                                  : m_State.Path();
        ImGui::TextUnformatted("This project has changes that are not on disk.");
        ImGui::TextDisabled("%s", path.c_str());
        ImGui::Separator();

        if (ImGui::Button("Save and quit")) {
            SaveProject(path);
            m_ClosePending = false;
            ImGui::CloseCurrentPopup();
            Application::Get().Close();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            m_ClosePending = false;
            ImGui::CloseCurrentPopup();
            Application::Get().Close();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            m_ClosePending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // The example is authored in code and written out on the way in, script included, so that
    // "show me what this thing does" is one menu item and not a tutorial.
    void StudioLayer::OpenExample(Example which) {
        m_Scripts.Stop();

        const bool lua = m_Scripts.Lang() == ScriptSession::Language::Lua;
        const char* name = "Counter example";
        switch (which) {
            case Example::Screens:   name = "Screens example";   BuildScreensExample(m_State); break;
            case Example::Feed:      name = "Feed example";      BuildFeedExample(m_State);    break;
            case Example::Login:     name = "Login block";       BuildLoginBlock(m_State);     break;
            case Example::Dashboard: name = "Dashboard block";   BuildDashboardBlock(m_State); break;
            case Example::Counter:                               BuildCounterExample(m_State); break;
        }

        // An example is an ordinary project, so it goes where projects go — a folder of its own
        // under the projects root, and not into the engine's directory.
        const std::filesystem::path folder = FileSystem::ProjectsRoot() / name;
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        const std::filesystem::path document = folder / (std::string(name) + ".vaescreen");

        // The Counter's buttons make a noise, which needs a file to make it with. Written before
        // the project is saved, so the asset the document names is already there when it loads.
        if (which == Example::Counter) WriteExampleClick(m_State, folder);

        m_Scripts.SetProjectPath(document);
        // The blocks have no logic in them: they are a layout exam, and inventing a script for one
        // would only mean Play had something to run that nobody asked for.
        if (which == Example::Counter || which == Example::Screens || which == Example::Feed) {
            m_Scripts.SetSource(
                which == Example::Screens ? (lua ? ScreensExampleLua() : ScreensExampleCpp())
              : which == Example::Feed    ? (lua ? FeedExampleLua()    : FeedExampleCpp())
                                          : (lua ? CounterExampleLua() : CounterExampleCpp()));
            m_Scripts.SaveSource();
        }

        // Written out, not just built in memory: an example you cannot hand to the player is a
        // demo, and the point of this one is that it is an ordinary project.
        SaveProject(document);
        m_FrameNext = true;
        VAE_INFO("opened '{}' — press F5 to run it", name);
    }

    // The escape hatch: whatever is on the canvas, written out as C++ against the same public
    // builder API a hand-written app would use, plus a main and a premake file that build it.
    void StudioLayer::ExportProject() {
        const std::filesystem::path project = m_State.Path().empty() ? DefaultProjectPath()
                                                                     : m_State.Path();
        gen::Options options;
        options.appName = project.stem().string();
        if (options.appName.empty()) options.appName = "App";
        // The exported app runs the same logic the Studio was running, from a file beside it.
        if (!m_Scripts.SourcePath().empty())
            options.script = m_Scripts.SourcePath().filename().string();
        options.assetRoot = m_State.AssetFolder();

        const std::filesystem::path out = project.parent_path() / (options.appName + "-export");
        std::string error;
        if (!gen::EmitProject(m_State.Doc(), out, options, &error)) {
            VAE_ERROR("export: {}", error);
            return;
        }
        // The script goes with it: the exported app loads the same module the Studio was running.
        if (!m_Scripts.SourcePath().empty()) {
            std::error_code ec;
            std::filesystem::copy_file(m_Scripts.SourcePath(),
                                       out / m_Scripts.SourcePath().filename(),
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
        VAE_INFO("export: {} — run `premake5 gmake && make` in there", out.string());
    }

    void StudioLayer::NewProject() {
        m_Scripts.Stop();
        m_State.NewProject();
        m_Scripts.SetProjectPath({});
        m_FrameNext = true;
    }

    void StudioLayer::AskForNewProject() {
        m_NamingProject = true;
        m_NamingAsked = false;
        m_NamingError.clear();
        std::snprintf(m_NewName, sizeof m_NewName, "Untitled");
        // Whatever the Studio is set to right now, because that is almost always what the next
        // project is too — and the dialog is where it becomes the project's rather than the app's.
        m_NewSpec = {};
        m_NewSpec.language = m_Scripts.Lang();
    }

    void StudioLayer::CreateProject(std::string_view name, const NewProjectSpec& spec) {
        std::string clean(name);
        // A project name is a folder name, so the characters a folder name cannot have are not part
        // of it. Silently rewriting them beats refusing a name someone reasonably typed.
        for (char& c : clean)
            if (c == '/' || c == '\\' || c == ':' || c == '\0') c = '-';
        while (!clean.empty() && (clean.front() == ' ' || clean.front() == '.')) clean.erase(0, 1);
        while (!clean.empty() && clean.back() == ' ') clean.pop_back();
        if (clean.empty()) { m_NamingError = "That name is empty."; return; }

        const std::filesystem::path folder = FileSystem::ProjectsRoot() / clean;
        std::error_code ec;
        if (std::filesystem::exists(folder, ec)) {
            m_NamingError = "A project called '" + clean + "' is already there.";
            return;
        }
        std::filesystem::create_directories(folder, ec);
        if (ec) { m_NamingError = ec.message(); return; }

        // The language before anything else: it decides what the script file beside the document
        // is called, and SaveProject writes that file.
        m_Scripts.SetLanguage(spec.language);

        NewProject();
        if (const Uuid home = m_State.ActiveScreen(); home.Valid()) {
            doc::Node* node = m_State.Doc().Find(home);
            node->layout.width = layout::Size::Px(spec.size.x);
            node->layout.height = layout::Size::Px(spec.size.y);
            m_State.Doc().SetProp(home, doc::Prop::Resizable, spec.resizable);
            m_State.Doc().Touch(home);
        }
        // The size was chosen, not edited: undoing back past the shape of the project someone just
        // asked for is not an undo anyone wants.
        m_State.Commands().Clear();

        SaveProject(folder / (clean + ".vaescreen"));
        // The script file, written now rather than on the first edit. Two reasons, and the second
        // is the load-bearing one: it gives the author a template to start from, and it is the
        // only record on disk of which language the project is written in — reopening reads it
        // back off the extension.
        m_Scripts.CreateSource();
        m_Canvas.FrameAll(m_State);
        m_NamingProject = false;
    }

    void StudioLayer::DrawNewProjectDialog() {
        if (!m_NamingProject) return;
        if (!m_NamingAsked) {
            ImGui::OpenPopup("New project###NewProject");
            m_NamingAsked = true;
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("New project###NewProject", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;

        constexpr f32 kWidth = 380.0f;

        ImGui::TextDisabled("A folder of this name, under");
        ImGui::TextDisabled("%s", FileSystem::ProjectsRoot().c_str());
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(kWidth);
        const bool entered = ImGui::InputText("##name", m_NewName, sizeof m_NewName,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_NamingError.empty())
            ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.45f, 1.0f), "%s", m_NamingError.c_str());

        // Asked here and only here, because a project is written in one language and changing it
        // later means rewriting every component script it has.
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("Logic");
        int language = m_NewSpec.language == ScriptSession::Language::Lua ? 0 : 1;
        ImGui::RadioButton("Lua", &language, 0);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::RadioButton("C++", &language, 1);
        m_NewSpec.language = language == 0 ? ScriptSession::Language::Lua
                                           : ScriptSession::Language::Cpp;
        ImGui::TextDisabled(language == 0
            ? "Reloads while the app is running. Nothing to install."
            : "Compiled to a module. Needs a C++ compiler on this machine.");

        // The screen size is not a setting so much as the thing every position on the canvas will
        // be chosen against, which is why it is decided before anything is placed.
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("First screen");

        struct Preset { const char* name; f32 width; f32 height; };
        static constexpr Preset kPresets[] = {
            { "Desktop · 1280 x 800",  1280.0f,  800.0f },
            { "Laptop · 1440 x 900",   1440.0f,  900.0f },
            { "Full HD · 1920 x 1080", 1920.0f, 1080.0f },
            { "Tablet · 1024 x 768",   1024.0f,  768.0f },
            { "Phone · 390 x 844",      390.0f,  844.0f },
            { "Custom",                   0.0f,    0.0f },
        };
        constexpr int kCustom = static_cast<int>(std::size(kPresets)) - 1;

        ImGui::SetNextItemWidth(kWidth);
        if (ImGui::BeginCombo("##preset", kPresets[m_NewPreset].name)) {
            for (int i = 0; i < static_cast<int>(std::size(kPresets)); ++i) {
                if (!ImGui::Selectable(kPresets[i].name, i == m_NewPreset)) continue;
                m_NewPreset = i;
                if (i != kCustom) m_NewSpec.size = { kPresets[i].width, kPresets[i].height };
            }
            ImGui::EndCombo();
        }

        if (m_NewPreset == kCustom) {
            ImGui::SetNextItemWidth(kWidth);
            f32 size[2] = { m_NewSpec.size.x, m_NewSpec.size.y };
            if (ImGui::DragFloat2("##size", size, 1.0f, 64.0f, 8192.0f, "%.0f")) {
                m_NewSpec.size = { std::clamp(size[0], 64.0f, 8192.0f),
                                   std::clamp(size[1], 64.0f, 8192.0f) };
            }
        }

        ImGui::Checkbox("Resizable", &m_NewSpec.resizable);
        ImGui::TextDisabled(m_NewSpec.resizable
            ? "The app fills its window. The size above is where the design starts."
            : "A hard resolution: the window opens at that size and cannot be dragged.");

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::Separator();
        if (ImGui::Button("Create") || entered) {
            CreateProject(m_NewName, m_NewSpec);
            if (!m_NamingProject) {
                ImGui::CloseCurrentPopup();
                m_ShowLauncher = false;
                m_LauncherOpen = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            m_NamingProject = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void StudioLayer::DrawLauncher() {
        if (!m_ShowLauncher) return;
        // Opened once, not every frame: re-opening a popup that is already open churns focus, and
        // a button that loses its active id between press and release never fires.
        if (!m_LauncherOpen) {
            ImGui::OpenPopup("VAE Studio###Launcher");
            m_LauncherOpen = true;
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(720.0f, 0.0f), ImGuiCond_Always);
        if (!ImGui::BeginPopupModal("VAE Studio###Launcher", nullptr,
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                                    | ImGuiWindowFlags_NoSavedSettings))
            return;

        ImGui::TextDisabled("Start a project, or pick up where you left off.");
        // Which build this is, where someone would look for it. The log carries the same line, but
        // a bug report is written by whoever is looking at this window.
        ImGui::SameLine();
        const std::string version = Version::String();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize(version.c_str()).x - 16.0f);
        ImGui::TextDisabled("%s", version.c_str());
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        if (ImGui::Button("New project", ImVec2(160.0f, 32.0f))) {
            AskForNewProject();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Counter example", ImVec2(160.0f, 32.0f))) {
            OpenExample(Example::Counter);
            m_ShowLauncher = false;
            m_LauncherOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Screens example", ImVec2(160.0f, 32.0f))) {
            OpenExample(Example::Screens);
            m_ShowLauncher = false;
            m_LauncherOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Feed example", ImVec2(160.0f, 32.0f))) {
            OpenExample(Example::Feed);
            m_ShowLauncher = false;
            m_LauncherOpen = false;
            ImGui::CloseCurrentPopup();
        }

        // A second row rather than a sixth button: five across ran off the edge of the dialog, and
        // a button you cannot see is a button that does not exist.
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::TextDisabled("Built from the component library alone:");
        if (ImGui::Button("Login block", ImVec2(160.0f, 32.0f))) {
            OpenExample(Example::Login);
            m_ShowLauncher = false;
            m_LauncherOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Dashboard block", ImVec2(160.0f, 32.0f))) {
            OpenExample(Example::Dashboard);
            m_ShowLauncher = false;
            m_LauncherOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Continue", ImVec2(120.0f, 32.0f))) {
            m_ShowLauncher = false;
            m_LauncherOpen = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SeparatorText("Recent");
        if (m_Recent.empty()) {
            ImGui::TextDisabled("Nothing yet — anything you save shows up here.");
        } else {
            for (const std::string& path : m_Recent) {
                ImGui::PushID(path.c_str());
                if (ImGui::Selectable(std::filesystem::path(path).filename().string().c_str())) {
                    OpenProject(path);
                    m_ShowLauncher = false;
                    m_LauncherOpen = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", path.c_str());
                ImGui::PopID();
            }
        }

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SeparatorText("Open");
        ImGui::SetNextItemWidth(-90.0f);
        const bool entered = ImGui::InputText("##path", m_OpenPath, sizeof m_OpenPath,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Open", ImVec2(80.0f, 0.0f)) || entered) && m_OpenPath[0]) {
            OpenProject(m_OpenPath);
            m_ShowLauncher = false;
            m_LauncherOpen = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    void StudioLayer::DrawMenuBar() {
        if (!ImGui::BeginMenuBar()) return;

        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New…", "Ctrl+N")) AskForNewProject();
            if (ImGui::MenuItem("Open…", "Ctrl+O")) m_ShowLauncher = true;
            if (ImGui::BeginMenu("Examples")) {
                if (ImGui::MenuItem("Counter — one component, twice"))
                    OpenExample(Example::Counter);
                if (ImGui::MenuItem("Screens — a list, a detail and an alert"))
                    OpenExample(Example::Screens);
                if (ImGui::MenuItem("Feed — waiting, failed, empty and the answer"))
                    OpenExample(Example::Feed);
                ImGui::Separator();
                if (ImGui::MenuItem("Login — a card of fields, from the catalog"))
                    OpenExample(Example::Login);
                if (ImGui::MenuItem("Dashboard — sidebar, figures, chart and table"))
                    OpenExample(Example::Dashboard);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Save", "Ctrl+S"))
                SaveProject(m_State.Path().empty() ? DefaultProjectPath() : m_State.Path());
            ImGui::Separator();
            if (ImGui::MenuItem("Export C++…")) ExportProject();
            ImGui::Separator();
            if (ImGui::MenuItem("Quit") && !HoldCloseForUnsavedWork()) Application::Get().Close();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            const bool canUndo = m_State.Commands().CanUndo();
            const bool canRedo = m_State.Commands().CanRedo();
            if (ImGui::MenuItem(canUndo ? std::string("Undo " + std::string(m_State.Commands().UndoName())).c_str()
                                        : "Undo", "Ctrl+Z", false, canUndo))
                m_State.Undo();
            if (ImGui::MenuItem(canRedo ? std::string("Redo " + std::string(m_State.Commands().RedoName())).c_str()
                                        : "Redo", "Ctrl+Shift+Z", false, canRedo))
                m_State.Redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !m_State.Selection().empty()))
                m_State.DuplicateSelection();
            if (ImGui::MenuItem("Delete", "Del", false, !m_State.Selection().empty()))
                m_State.DeleteSelection();
            ImGui::Separator();
            // The library stops being the engine's the moment a designer can add to it.
            if (ImGui::MenuItem("Group", "Ctrl+G", false, m_Canvas.CanGroup(m_State)))
                m_Canvas.GroupSelection(m_State);
            if (ImGui::MenuItem("Ungroup", "Ctrl+Shift+G", false, m_Canvas.CanUngroup(m_State)))
                m_Canvas.UngroupSelection(m_State);
            ImGui::Separator();
            if (ImGui::MenuItem("Make component", "Ctrl+Alt+K", false, m_State.CanMakeComponent()))
                m_State.MakeComponentFromSelection();
            ImGui::Separator();

            const bool any = !m_State.Selection().empty();
            if (ImGui::BeginMenu("Align", any)) {
                using Edge = Canvas::Edge;
                if (ImGui::MenuItem("Left"))              m_Canvas.AlignSelection(m_State, Edge::Left);
                if (ImGui::MenuItem("Horizontal centres")) m_Canvas.AlignSelection(m_State, Edge::CentreX);
                if (ImGui::MenuItem("Right"))             m_Canvas.AlignSelection(m_State, Edge::Right);
                ImGui::Separator();
                if (ImGui::MenuItem("Top"))               m_Canvas.AlignSelection(m_State, Edge::Top);
                if (ImGui::MenuItem("Vertical centres"))  m_Canvas.AlignSelection(m_State, Edge::CentreY);
                if (ImGui::MenuItem("Bottom"))            m_Canvas.AlignSelection(m_State, Edge::Bottom);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Distribute", m_State.Selection().size() >= 3)) {
                if (ImGui::MenuItem("Horizontally")) m_Canvas.DistributeSelection(m_State, true);
                if (ImGui::MenuItem("Vertically"))   m_Canvas.DistributeSelection(m_State, false);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Frame all", "Shift+F")) m_FrameNext = true;
            if (ImGui::MenuItem("Zoom 100%", "Ctrl+0")) m_Canvas.ZoomTo(1.0f);
            ImGui::Separator();
            bool rulers = m_Canvas.Rulers();
            if (ImGui::MenuItem("Rulers", "Ctrl+R", &rulers)) { m_Canvas.SetRulers(rulers); SaveSettings(); }
            ImGui::Separator();
            // The theme is a document fact, so switching it here is switching it for the app —
            // every token resolves through it and every widget repaints from the tokens.
            const bool dark = m_State.Doc().ActiveTheme() == doc::Theme::Dark;
            if (ImGui::MenuItem("Dark theme", nullptr, dark, !dark)) {
                m_State.Execute(CreateScope<doc::SetThemeCommand>(doc::Theme::Dark));
                m_State.Doc().Touch(m_State.ActiveScreen());
            }
            if (ImGui::MenuItem("Light theme", nullptr, !dark, dark)) {
                m_State.Execute(CreateScope<doc::SetThemeCommand>(doc::Theme::Light));
                m_State.Doc().Touch(m_State.ActiveScreen());
            }
            ImGui::Separator();
            // Reduced motion is an accessibility setting, not a preference — and a designer looking
            // at a transition frame by frame wants it off too.
            ui::ViewTree::Motion motion = m_Canvas.Host().Tree().MotionSettings();
            if (ImGui::MenuItem("Transitions", nullptr, &motion.enabled)) {
                m_Canvas.Host().SetMotion(motion);
                SaveSettings();
            }
            ImGui::Separator();
            bool preview = m_Canvas.Preview();
            if (ImGui::MenuItem("Preview", "Ctrl+P", &preview)) { m_Canvas.SetPreview(preview); SaveSettings(); }
            ImGui::Separator();
            ImGui::MenuItem("ImGui demo", nullptr, &m_ShowDemo);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Run")) {
            if (ImGui::MenuItem(m_Scripts.Playing() ? "Stop" : "Play", "F5")) m_Scripts.Toggle();
            if (ImGui::MenuItem("Build script", "Ctrl+B")) m_Scripts.Build();
            if (ImGui::MenuItem("Hot reload", "F6", false, m_Scripts.Playing()))
                m_Scripts.HotReload();
            ImGui::Separator();
            const bool lua = m_Scripts.Lang() == ScriptSession::Language::Lua;
            if (ImGui::MenuItem("Lua", nullptr, lua, !m_Scripts.Playing()))
                m_Scripts.SetLanguage(ScriptSession::Language::Lua);
            if (ImGui::MenuItem("C++", nullptr, !lua, !m_Scripts.Playing()))
                m_Scripts.SetLanguage(ScriptSession::Language::Cpp);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Insert")) {
            const Uuid parent = m_State.Selection().empty() ? m_State.ActiveScreen()
                                                            : m_State.Primary();
            if (ImGui::MenuItem("Frame"))
                m_State.CreateChild(doc::NodeKind::Frame, parent, "Frame");
            if (ImGui::MenuItem("Text"))
                m_State.CreateChild(doc::NodeKind::Text, parent, "Text");
            ImGui::EndMenu();
        }

        // Right-aligned status: the zoom level and whether there is unsaved work.
        char status[128];
        std::snprintf(status, sizeof status, "%s%.0f%%   %s",
                      m_State.Dirty() ? "• " : "", m_Canvas.Zoom() * 100.0f,
                      m_Scripts.Playing() ? "running"
                                          : m_Canvas.Preview() ? "preview" : "design");
        const f32 width = ImGui::CalcTextSize(status).x;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - width - 16.0f);
        ImGui::TextDisabled("%s", status);

        ImGui::EndMenuBar();
    }

    // Returns whether anything happened, because a shortcut is the one kind of input that changes
    // what is on screen without the pointer moving, and the idle loop has to be told to redraw.
    bool StudioLayer::HandleShortcuts() {
        ImGuiIO& io = ImGui::GetIO();

        // The run keys come first and are not gated on focus: F5 is not something anyone can type,
        // and stopping a running app must work no matter what happens to have the keyboard.
        bool acted = false;
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
            if (io.KeyShift) m_Scripts.Stop(); else m_Scripts.Toggle();
            acted = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) { m_Scripts.HotReload(); acted = true; }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B, false)) { m_Scripts.Build(); acted = true; }

        if (io.WantTextInput) return acted;   // a field has the keyboard; the rest is not ours

        const bool ctrl = io.KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (io.KeyShift) m_State.Redo(); else m_State.Undo();
            acted = true;
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) { m_State.Redo(); acted = true; }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) { m_State.DuplicateSelection(); acted = true; }
        // Shift first: Ctrl+Shift+G is not a Ctrl+G that happens to have shift held.
        if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_G, false)) {
            m_Canvas.UngroupSelection(m_State);
            acted = true;
        } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_G, false)) {
            m_Canvas.GroupSelection(m_State);
            acted = true;
        }
        // Figma's, because it is the one everybody's fingers already know.
        if (ctrl && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_K, false)) {
            m_State.MakeComponentFromSelection();
            acted = true;
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            SaveProject(m_State.Path().empty() ? DefaultProjectPath() : m_State.Path());
            acted = true;
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) { AskForNewProject(); acted = true; }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) { m_ShowLauncher = true; acted = true; }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            m_Canvas.SetRulers(!m_Canvas.Rulers());
            acted = true;
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
            m_Canvas.SetPreview(!m_Canvas.Preview());
            acted = true;
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_0, false)) { m_Canvas.ZoomTo(1.0f); acted = true; }

        // Everything below edits the design, which is not what is on screen while an app runs.
        if (m_Scripts.Playing()) return acted;

        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            m_State.DeleteSelection();
            acted = true;
        }
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false)) { m_FrameNext = true; acted = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { m_State.ClearSelection(); acted = true; }
        return acted;
    }

    void StudioLayer::OnImGuiRender() {
        // Shortcuts first: they change what the panels are about to draw, and the canvas has
        // already rendered this frame's image, so the result needs one more frame either way.
        if (HandleShortcuts()) Application::Get().RequestFrame();

        DrawDockSpace();

        DrawLayersPanel(m_State);
        DrawScreensPanel(m_State, m_Canvas);
        DrawComponentsPanel(m_State, m_Canvas);
        DrawInspectorPanel(m_State);
        DrawAssetsPanel(m_State, m_Canvas);
        DrawConsolePanel();
        if (const std::filesystem::path open = DrawFilesPanel(m_Scripts, m_State); !open.empty())
            OpenProject(open);
        DrawRuntimePanel(m_Scripts, m_Canvas);
        DrawScriptPanel(m_Scripts, m_State);
        m_Canvas.OnImGuiRender(m_State);

        // A running app has timers and on_update; it is the one thing in the editor that changes
        // without anyone touching the mouse, so the idle loop has to be held open for it.
        if (m_Scripts.Playing()) Application::Get().RequestFrame();

        // Framing needs the canvas's viewport, which only exists after it has drawn once — and on
        // startup the first draw happens before the dock layout has been applied, so the canvas is
        // briefly window-sized. Framing against that scale is wrong and, because the request is
        // consumed, never corrects itself. So: wait for the viewport to stop changing, and keep
        // the idle loop awake until it has, since framing itself produces no input event.
        if (m_FrameNext) {
            Application::Get().RequestFrame();
            if (m_Canvas.HasViewport() && m_Canvas.ViewportSize() == m_LastViewport) {
                m_Canvas.FrameAll(m_State);
                m_FrameNext = false;
            }
        }
        m_LastViewport = m_Canvas.ViewportSize();

        DrawLauncher();
        DrawUnsavedChangesDialog();
        DrawNewProjectDialog();
        if (m_ShowDemo) ImGui::ShowDemoWindow(&m_ShowDemo);
    }

    void StudioLayer::OnEvent(Event& e) {
        if (e.type == EventType::WindowClose && HoldCloseForUnsavedWork()) {
            e.handled = true;
            return;
        }
        // The editor animates carets and hover states, so a frame is wanted after any input.
        if (e.IsMouse() || e.IsKeyboard()) Application::Get().RequestFrame();
    }

}
