#pragma once

#include "Canvas.h"
#include "EditorState.h"
#include "ScriptSession.h"

#include "vae/base/Platform.h"
#include "vae/core/Layer.h"

#include <filesystem>
#include <string>
#include <vector>

namespace vae {

    // The editor: a dock space, a menu bar, the panels, and the keyboard shortcuts that tie them
    // together. Everything it changes goes through EditorState, so undo covers the whole surface.
    class StudioLayer final : public Layer {
    public:
        StudioLayer() : Layer("Studio") {}

        void OnAttach() override;
        void OnDetach() override;
        void OnRender(gpu::CommandList& cmd) override;
        void OnImGuiRender() override;
        void OnEvent(Event& e) override;

        // The editor's state and its view, for anything that drives the layer rather than being
        // driven by it — the headless --selftest, and later the player's live-preview handoff.
        EditorState& State() { return m_State; }
        Canvas& Surface() { return m_Canvas; }
        const Canvas& Surface() const { return m_Canvas; }
        ScriptSession& Scripts() { return m_Scripts; }
        // The worked example, reachable from the menu, the launcher and the selftest alike.
        // Which worked example. Both are ordinary projects written out to disk, not demos.
        // The last two carry no script: they are the catalog's exam, two of shadcn's own blocks
        // rebuilt out of library components to find out what the catalog is still missing.
        enum class Example { Counter, Screens, Feed, Login, Dashboard };
        void OpenExample(Example which = Example::Counter);

        void SaveProject(const std::filesystem::path& path);
        // What a new project is, before there is one. Everything here has to be decided at
        // creation because each answer is expensive to change afterwards: the language names the
        // script file, and the screen size is what every position on the canvas was chosen against.
        struct NewProjectSpec {
            ScriptSession::Language language = ScriptSession::Language::Lua;
            Vec2 size{ 1280.0f, 800.0f };
            bool resizable = true;

        };

        void CreateProject(std::string_view name, const NewProjectSpec& spec);
        // The defaults, for a caller that only has a name: a Lua project at 1280x800 that fills
        // whatever window it is given.
        void CreateProject(std::string_view name) { CreateProject(name, NewProjectSpec{}); }
        // Loads a project that is already on disk. Public alongside CreateProject because they are
        // the two ways a project gets onto the canvas, and the selftest drives both.
        void OpenProject(const std::filesystem::path& path);
        // What the window does with files dragged in from the desktop. Public for the same reason:
        // the selftest drops files at it, since nothing else can simulate a drag from a file
        // manager.
        void OnFilesDropped(const std::vector<std::filesystem::path>& files);
        // Why the last CreateProject refused, or empty. The dialog shows it; the selftest reads it.
        const std::string& NamingError() const { return m_NamingError; }
        // True when there is something to lose. The script is a separate file with its own dirty
        // flag, and losing either without being asked is the same surprise.
        bool HasUnsavedWork() const;
        // Asks before closing, once. Returns true when the close should be held back.
        bool HoldCloseForUnsavedWork();

        // Runs the project the way a user will: its own process, its own window, no editor
        // anywhere in it. Play on the canvas answers "does this work?"; this answers "is this the
        // app?" — the window is the size the design says, the chrome is the desktop's, and a
        // script that crashes takes down nothing but itself. `screen` empty runs the project from
        // its start screen. Public because the selftest drives it.
        void RunInWindow(const std::string& screen = {});
        // Where VAE-Player is, or empty. Public because the selftest checks that an install can
        // find its own player before anything asks it to launch one.
        static std::filesystem::path PlayerPath();
        void StopWindow();
        bool RunningInWindow();
        // Why the last RunInWindow refused, or empty.
        const std::string& RunError() const { return m_RunError; }

    private:
        void DrawMenuBar();
        void DrawDockSpace();
        void DrawLauncher();
        void BuildDefaultLayout(unsigned dockspace);
        bool HandleShortcuts();

        void NewProject();
        // Asks for a name, then makes a folder of that name under the projects root and puts the
        // document in it. A project is a folder: its script, its store and its assets sit beside
        // the document, and scattering those across a shared directory is how they get lost.
        void AskForNewProject();
        void DrawNewProjectDialog();
        void ExportProject();
        void DrawUnsavedChangesDialog();
        void LoadRecents();
        // Bumped whenever the default layout gains a panel, so an existing layout is rebuilt
        // once instead of hiding the new one.
        static constexpr int kLayoutVersion = 3;   // 3: the XML tab
        int  m_LayoutVersion = 0;

        void TickAutosave();
        void DrawRecoveryDialog();
        bool m_RecoveryAsked = false;
        // The project a recovery file was found for, or empty. Drives the prompt.
        std::filesystem::path m_Recovery;
        f32  m_LastEdit = 0.0f;
        f32  m_LastAutosave = 0.0f;
        u64  m_LastRevision = 0;

        void LoadSettings();
        void SaveSettings();
        void RememberProject(const std::filesystem::path& path);
        std::filesystem::path DefaultProjectPath() const;

        EditorState m_State;
        Canvas m_Canvas;
        ScriptSession m_Scripts;
        bool m_LayoutBuilt = false;
        // The app running in its own window, if there is one, and why the last attempt failed.
        platform::Process m_Windowed = 0;
        std::string m_RunError;
        bool m_FrameNext = true;
        bool m_ShowLauncher = true;
        // A close the user has been asked about but not yet answered.
        bool m_ClosePending = false;
        bool m_CloseAsked = false;
        bool m_NamingProject = false;
        bool m_NamingAsked = false;
        char m_NewName[96] = "Untitled";
        std::string m_NamingError;
        NewProjectSpec m_NewSpec;
        int m_NewPreset = 0;
        bool m_LauncherOpen = false;
        Vec2 m_LastViewport{ 0.0f, 0.0f };
        std::vector<std::string> m_Recent;
        char m_OpenPath[512] = {};
    };

}
