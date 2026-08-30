#pragma once

#include "vae/doc/Command.h"
#include "vae/doc/Serializer.h"
#include "vae/doc/Strings.h"
#include "vae/svc/Audio.h"
#include "vae/ui/Library.h"

#include <filesystem>

namespace vae {

    // Everything the editor edits, and the only path by which it changes.
    //
    // Selection is a list of ids rather than pointers or view indices: a node stays selected across
    // a rebuild, a reparent, and an undo that recreated it.
    class EditorState {
    public:
        EditorState();

        doc::Document& Doc() { return m_Document; }
        const doc::Document& Doc() const { return m_Document; }
        doc::CommandStack& Commands() { return m_Commands; }
        const ui::Library& Library() const { return m_Library; }

        // --- screens ----------------------------------------------------------------------------
        std::vector<Uuid> Screens() const;
        Uuid ActiveScreen() const { return m_ActiveScreen; }
        void SetActiveScreen(Uuid screen);
        Uuid AddScreen(std::string name, Vec2 size);

        // --- selection --------------------------------------------------------------------------
        const std::vector<Uuid>& Selection() const { return m_Selection; }
        bool IsSelected(Uuid id) const;
        void Select(Uuid id, bool additive = false);
        void SelectMany(std::vector<Uuid> ids);
        void ClearSelection();
        Uuid Primary() const { return m_Selection.empty() ? Uuid::Invalid() : m_Selection.front(); }

        // --- instance context -------------------------------------------------------------------
        // A node authored inside a component is on the screen once per copy of that component, so
        // selecting one only means something relative to a copy. The path is every instance the
        // selection sits inside, outermost first — and the outermost is where an edit is filed, so
        // retitling one card's button does not retitle every card's.
        const std::vector<Uuid>& InstancePath() const { return m_InstancePath; }
        void SelectInside(std::vector<Uuid> instancePath, Uuid node);
        // Steps out one instance, selecting the instance that was left. Empty path does nothing.
        void ExitInstance();
        // The dotted name a script would use for a node, relative to the outermost instance:
        // "Header.Close". Empty when the node is not inside one.
        std::string ScriptPath(Uuid node) const;

        // --- mutation ---------------------------------------------------------------------------
        void Execute(Scope<doc::Command> command);
        void SetProp(Uuid node, doc::Prop prop, doc::Value value);
        // Reads back what SetProp would be changing — through the override table for an instance.
        doc::Value GetProp(Uuid node, doc::Prop prop) const;
        // The same pair for a string-keyed property: the state overlays ("hovered:fill") the widget
        // library writes, and anything a project invents for itself.
        void SetProp(Uuid node, std::string key, doc::Value value);
        doc::Value GetProp(Uuid node, const std::string& key) const;
        void SetLayout(Uuid node, const layout::LayoutStyle& style);
        void Rename(Uuid node, std::string name);
        Uuid CreateChild(doc::NodeKind kind, Uuid parent, std::string name);
        // A piece of artwork, placed as a Vector node of its own. Not a component instance: an
        // icon is not a widget, and wrapping every one in a component would mean a catalog entry
        // nobody would ever open.
        Uuid PlaceArtwork(Uuid asset, Uuid parent, Vec2 at, Vec2 size, bool followsText);
        Uuid PlaceInstance(std::string_view component, Uuid parent, Vec2 at);
        // Turns the selected frame into a reusable component and leaves an instance of it where it
        // was — Figma's "create component", and the thing that makes the library the designer's
        // rather than the engine's. Returns the instance, or Invalid when the selection cannot be
        // one (a screen, an instance, a component).
        Uuid MakeComponentFromSelection(std::string name = {});
        bool CanMakeComponent() const;
        // Marks the frame an instance's own children land in. One per component, so setting it
        // clears whatever held it before.
        void SetSlot(Uuid node, bool slot);
        // Copies an image into the project folder and registers it. Copied rather than referenced:
        // an asset that lives somewhere else is one the project loses the moment it is moved, and
        // a project is a folder you hand to someone. Returns the asset's id, or Invalid — the
        // reason is in AssetError().
        Uuid ImportAsset(const std::filesystem::path& file);
        void RemoveAsset(Uuid asset);
        const std::string& AssetError() const { return m_AssetError; }

        // The editor's own speaker, for hearing a sound before deciding to use it. Deliberately
        // not the app's: a preview that stops when the app stops, or that a running script can
        // turn down, is a preview that behaves differently depending on what else is happening.
        svc::Audio& Preview() { return m_Preview; }
        // Plays an asset by id, or says why it could not. Empty on success.
        std::string PreviewAsset(Uuid asset);
        // Where a project's files live. The document's own folder, or the projects root before it
        // has been saved anywhere.
        std::filesystem::path AssetFolder() const;

        // Puts a component master on the canvas so it can be edited. A component leaves the screen
        // when it is made, and without this it is a definition nobody can ever open again.
        void OpenComponent(Uuid component);
        // The component being edited, or Invalid when a screen is showing.
        Uuid EditingComponent() const;
        // Back to whatever screen was showing before a component was opened.
        void CloseComponent();

        // The component `node` is authored inside, or Invalid if it is on a screen.
        Uuid ComponentOwning(Uuid node) const;
        // Cut, copy and paste. The clipboard carries format-3 markup — the same text a .vae file
        // holds — so a copy survives being pasted into another project, into a second Studio, or
        // through a text editor.
        void CopySelection();
        void CutSelection();
        // Into the selected container, or into the screen. Returns how many roots arrived.
        u32  Paste();
        bool CanPaste() const;

        void DeleteSelection();
        void DuplicateSelection();
        void Undo();
        void Redo();
        // Ends a coalescing run, so the next drag is its own undo entry.
        void EndGesture() { m_Commands.Break(); }

        // --- files ------------------------------------------------------------------------------
        void NewProject();
        bool Save(const std::filesystem::path& path);

        // A recovery copy beside the project, written while there is unsaved work and deleted the
        // moment there is not. Not a save: it never becomes the file the designer opens, it is
        // what is offered back after a crash or a power cut.
        // --- languages ---------------------------------------------------------------------
        // Which locale the canvas is previewing, "" for the authored text. Purely an editor view:
        // it changes what is drawn, never what is saved.
        const std::string& Locale() const { return m_Locale; }
        void SetLocale(std::string locale);
        const doc::StringTable* Strings() const;
        std::vector<std::string> Locales() const;
        // Writes strings/<locale>.json with every key the document uses, keeping the translations
        // that are already in it. This is the file a translator is handed.
        bool WriteStrings(const std::string& locale);

        void Autosave();
        // The recovery file for a project, and whether one is worth offering — it exists and is
        // newer than the project itself.
        static std::filesystem::path RecoveryPathFor(const std::filesystem::path& project);
        static bool HasRecovery(const std::filesystem::path& project);
        void DiscardRecovery();
        bool Load(const std::filesystem::path& path);
        const std::filesystem::path& Path() const { return m_Path; }

        // A A project folder is a document split across files — one per screen, one per forked component.
        // Everything else is a single document. Which one this is decides only how Save and Load
        // touch the disk; the document in memory is the same either way.
        bool IsSplit() const { return doc::Project::IsProjectFile(m_Path); }
        doc::Project& ProjectFile() { return m_Project; }
        bool Dirty() const { return m_SavedRevision != m_Document.Revision(); }
        // Recovery loads the copy and then says which project it belongs to, so a save writes the
        // project rather than the recovery file, and says it is unsaved until it does.
        void SetPath(std::filesystem::path path) { m_Path = std::move(path); }
        void MarkDirty() { m_SavedRevision = m_Document.Revision() - 1; }

    private:
        void PruneSelection();

        doc::Document m_Document;
        doc::CommandStack m_Commands;
        ui::Library m_Library;
        std::vector<Uuid> m_Selection;
        std::vector<Uuid> m_InstancePath;
        Uuid m_ActiveScreen = Uuid::Invalid();
        Uuid m_ScreenBehind = Uuid::Invalid();   // where CloseComponent goes back to
        std::string m_AssetError;
        std::string m_Locale;
        doc::StringTable m_Strings;
        svc::Audio m_Preview;
        std::filesystem::path m_Path;
        doc::Project m_Project;
        u64 m_SavedRevision = 0;
    };

}
