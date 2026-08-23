#include "EditorState.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"
#include "vae/core/Input.h"

#include <algorithm>

namespace vae {

    EditorState::EditorState() { NewProject(); }

    void EditorState::NewProject() {
        m_Document.Clear();
        m_Commands.Clear();
        m_Selection.clear();
        m_Path.clear();

        m_Library = ui::BuildStandardLibrary(m_Document);
        m_ActiveScreen = AddScreen("Home", { 1280.0f, 800.0f });
        m_SavedRevision = m_Document.Revision();
    }

    Uuid EditorState::AddScreen(std::string name, Vec2 size) {
        const Uuid screen = m_Document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(),
                                                  std::move(name));
        doc::Node* node = m_Document.Find(screen);
        node->layout.mode = layout::LayoutMode::Absolute;
        node->layout.width = layout::Size::Px(size.x);
        node->layout.height = layout::Size::Px(size.y);
        m_Document.SetProp(screen, doc::Prop::Fill, doc::TokenRef{ "bg" });
        m_Document.Touch(screen);
        return screen;
    }

    std::vector<Uuid> EditorState::Screens() const {
        std::vector<Uuid> out;
        for (Uuid root : m_Document.Roots()) {
            const doc::Node* node = m_Document.Find(root);
            if (node && node->kind == doc::NodeKind::Screen) out.push_back(root);
        }
        return out;
    }

    void EditorState::SetActiveScreen(Uuid screen) {
        if (screen == m_ActiveScreen) return;
        m_ActiveScreen = screen;
        ClearSelection();
    }

    bool EditorState::IsSelected(Uuid id) const {
        return std::find(m_Selection.begin(), m_Selection.end(), id) != m_Selection.end();
    }

    void EditorState::Select(Uuid id, bool additive) {
        if (!id.Valid()) { if (!additive) ClearSelection(); return; }
        // Selecting something on the screen leaves whatever instance we were inside. Keeping the
        // path would file the next edit under a component the selection is not in.
        m_InstancePath.clear();
        if (!additive) { m_Selection.assign(1, id); return; }

        auto it = std::find(m_Selection.begin(), m_Selection.end(), id);
        if (it == m_Selection.end()) m_Selection.push_back(id);
        else m_Selection.erase(it);
    }

    void EditorState::SelectMany(std::vector<Uuid> ids) {
        m_InstancePath.clear();
        m_Selection = std::move(ids);
    }

    void EditorState::ClearSelection() {
        m_Selection.clear();
        m_InstancePath.clear();
    }

    void EditorState::SelectInside(std::vector<Uuid> instancePath, Uuid node) {
        m_InstancePath = std::move(instancePath);
        m_Selection.assign(1, node);
    }

    void EditorState::ExitInstance() {
        if (m_InstancePath.empty()) return;
        const Uuid leaving = m_InstancePath.back();
        m_InstancePath.pop_back();
        m_Selection.assign(1, leaving);
    }

    std::string EditorState::ScriptPath(Uuid node) const {
        if (m_InstancePath.empty() || !m_Document.Contains(node)) return {};

        // Exactly the steps a script's own lookup takes: names between the outermost instance's root
        // and this node, outermost first. A component root is not one of them — the instance that
        // placed it is, because that is the name the designer gave and the view carries.
        std::vector<std::string> names;
        for (Uuid at = node; at.Valid(); ) {
            const doc::Node* current = m_Document.Find(at);
            if (!current) break;

            if (current->parent.Valid()) {
                if (!current->name.empty()) names.push_back(current->name);
                at = current->parent;
                continue;
            }

            const auto it = std::find_if(m_InstancePath.begin(), m_InstancePath.end(),
                                         [&](Uuid instance) {
                                             const doc::Node* n = m_Document.Find(instance);
                                             return n && n->componentId == current->id;
                                         });
            if (it == m_InstancePath.end()) break;
            // The outermost instance is the frame of reference, not a step inside it.
            if (*it == m_InstancePath.front()) break;

            const doc::Node* instance = m_Document.Find(*it);
            if (!instance) break;
            if (!instance->name.empty()) names.push_back(instance->name);
            at = instance->parent;
        }

        std::string path;
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            if (!path.empty()) path += '.';
            path += *it;
        }
        return path;
    }

    void EditorState::PruneSelection() {
        std::erase_if(m_Selection, [this](Uuid id) { return !m_Document.Contains(id); });
        std::erase_if(m_InstancePath, [this](Uuid id) { return !m_Document.Contains(id); });
    }

    void EditorState::Execute(Scope<doc::Command> command) {
        if (!command) return;
        m_Commands.Execute(m_Document, std::move(command));
    }

    void EditorState::SetProp(Uuid node, doc::Prop prop, doc::Value value) {
        const doc::Node* target = m_Document.Find(node);
        if (!target) return;

        // Inside an instance, the edit is about this copy. It is filed on the outermost instance —
        // the one actually on the screen — keyed by the node it is about, which is the same rule the
        // flattener reads back with.
        if (!m_InstancePath.empty() && node != m_InstancePath.front()) {
            Execute(CreateScope<doc::SetOverrideCommand>(m_InstancePath.front(), node, prop,
                                                         std::move(value)));
            return;
        }

        // Editing an instance edits that instance, never the component every other copy shares.
        if (target->IsInstance()) {
            Execute(CreateScope<doc::SetOverrideCommand>(node, target->componentId, prop,
                                                         std::move(value)));
            return;
        }
        Execute(CreateScope<doc::SetPropCommand>(node, prop, std::move(value)));
    }

    void EditorState::SetProp(Uuid node, std::string key, doc::Value value) {
        const doc::Node* target = m_Document.Find(node);
        if (!target) return;

        // "fill" and Prop::Fill are the same property stored two ways, and a field that spells it as
        // a string must not write a second, invisible copy of it.
        if (const auto prop = doc::PropFromName(key)) {
            SetProp(node, *prop, std::move(value));
            return;
        }

        if (!m_InstancePath.empty() && node != m_InstancePath.front()) {
            Execute(CreateScope<doc::SetKeyedOverrideCommand>(m_InstancePath.front(), node,
                                                              std::move(key), std::move(value)));
            return;
        }
        if (target->IsInstance()) {
            Execute(CreateScope<doc::SetKeyedOverrideCommand>(node, target->componentId,
                                                              std::move(key), std::move(value)));
            return;
        }
        Execute(CreateScope<doc::SetKeyedPropCommand>(node, std::move(key), std::move(value)));
    }

    doc::Value EditorState::GetProp(Uuid node, const std::string& key) const {
        if (const auto prop = doc::PropFromName(key)) return GetProp(node, *prop);
        if (!m_Document.Contains(node)) return {};
        const doc::PropBag bag = m_Document.ResolvedProps(m_InstancePath, node);
        const doc::Value* found = bag.Find(key);
        return found ? *found : doc::Value{};
    }

    doc::Value EditorState::GetProp(Uuid node, doc::Prop prop) const {
        if (!m_Document.Contains(node)) return {};
        const doc::PropBag bag = m_Document.ResolvedProps(m_InstancePath, node);
        const doc::Value* found = bag.Find(prop);
        return found ? *found : doc::Value{};
    }

    void EditorState::SetLayout(Uuid node, const layout::LayoutStyle& style) {
        Execute(CreateScope<doc::SetLayoutCommand>(node, style));
    }

    void EditorState::Rename(Uuid node, std::string name) {
        Execute(CreateScope<doc::RenameCommand>(node, std::move(name)));
    }

    Uuid EditorState::CreateChild(doc::NodeKind kind, Uuid parent, std::string name) {
        auto command = CreateScope<doc::CreateNodeCommand>(kind, parent, std::move(name));
        doc::CreateNodeCommand* raw = command.get();
        Execute(std::move(command));
        const Uuid created = raw->Created();
        if (created.Valid()) Select(created);
        return created;
    }

    Uuid EditorState::PlaceArtwork(Uuid asset, Uuid parent, Vec2 at, Vec2 size, bool followsText) {
        m_Commands.BeginTransaction("Place artwork");
        auto create = CreateScope<doc::CreateNodeCommand>(doc::NodeKind::Vector, parent, "Artwork");
        doc::CreateNodeCommand* raw = create.get();
        Execute(std::move(create));
        const Uuid node = raw->Created();
        if (node.Valid()) {
            layout::LayoutStyle style = m_Document.Find(node)->layout;
            style.offsetStart = at;
            // At the size the file says it is, brought into a range you can see and work with:
            // an icon exported at 24 arrives as a speck otherwise, and an illustration exported at
            // 2048 arrives covering the screen. Aspect is kept either way.
            const Vec2 natural = size.x > 0.0f && size.y > 0.0f ? size : Vec2{ 64.0f, 64.0f };
            const f32 longest = std::max({ natural.x, natural.y, 1.0f });
            const f32 scale = longest < 48.0f ? 48.0f / longest
                            : longest > 320.0f ? 320.0f / longest : 1.0f;
            style.width = layout::Size::Px(std::round(natural.x * scale));
            style.height = layout::Size::Px(std::round(natural.y * scale));
            SetLayout(node, style);
            SetProp(node, doc::Prop::Image, doc::AssetRef{ asset });
            // Artwork drawn in `currentColor` is asking to be told; the theme's text colour is
            // the answer that makes it visible on both themes, and it is a token so it stays that
            // way when the theme changes.
            if (followsText) SetProp(node, doc::Prop::Fill, doc::TokenRef{ "text" });
        }
        m_Commands.EndTransaction(m_Document);
        m_Commands.Break();
        if (node.Valid()) Select(node);
        return node;
    }

    Uuid EditorState::PlaceInstance(std::string_view component, Uuid parent, Vec2 at) {
        const Uuid master = m_Library.Find(component);
        if (!master.Valid()) return Uuid::Invalid();

        // One undo entry for the whole placement: the node and the position it was dropped at.
        m_Commands.BeginTransaction("Place " + std::string(component));
        auto create = CreateScope<doc::CreateInstanceCommand>(master, parent);
        doc::CreateInstanceCommand* raw = create.get();
        Execute(std::move(create));
        const Uuid instance = raw->Created();
        if (instance.Valid()) {
            layout::LayoutStyle style = m_Document.Find(instance)->layout;
            style.offsetStart = at;
            SetLayout(instance, style);
        }
        m_Commands.EndTransaction(m_Document);
        m_Commands.Break();
        if (instance.Valid()) Select(instance);
        return instance;
    }

    std::filesystem::path EditorState::AssetFolder() const {
        return m_Path.empty() ? FileSystem::ProjectsRoot() : m_Path.parent_path();
    }

    Uuid EditorState::ImportAsset(const std::filesystem::path& file) {
        m_AssetError.clear();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            m_AssetError = "no file at " + file.string();
            return Uuid::Invalid();
        }

        const std::filesystem::path folder = AssetFolder() / "assets";
        std::filesystem::create_directories(folder, ec);

        // A name that is already taken gets a number rather than silently replacing the file
        // somebody else's node is pointing at.
        std::filesystem::path target = folder / file.filename();
        for (int n = 2; std::filesystem::exists(target, ec); ++n)
            target = folder / (file.stem().string() + " " + std::to_string(n)
                               + file.extension().string());

        std::filesystem::copy_file(file, target, ec);
        if (ec) {
            m_AssetError = "could not copy: " + ec.message();
            return Uuid::Invalid();
        }

        const std::string relative = ("assets/" + target.filename().string());
        // Through the stack: importing the wrong file is an edit like any other, and Ctrl+Z is
        // what anyone reaches for. The copied file stays on disk — undo takes it out of the
        // project, not off the user's machine.
        auto command = CreateScope<doc::AddAssetCommand>(target.stem().string(), relative);
        doc::AddAssetCommand* added = command.get();
        Execute(std::move(command));
        VAE_INFO("imported {} as {}", file.string(), relative);
        return added->Created();
    }

    std::string EditorState::PreviewAsset(Uuid asset) {
        const doc::Document::Asset* found = m_Document.FindAsset(asset);
        if (!found) return "no such asset";

        // One at a time: clicking through a folder of sounds should let you compare them, not
        // stack them.
        m_Preview.StopAll();
        const std::filesystem::path file = AssetFolder() / found->path;
        if (m_Preview.Play(file) != 0) return {};
        return m_Preview.Problem().empty() ? "could not play it" : m_Preview.Problem();
    }

    void EditorState::RemoveAsset(Uuid asset) {
        m_AssetError.clear();
        // Undoable, and it comes back with the same id: nodes point at assets by id, so restoring
        // one under a fresh id would leave every picture that used it blank.
        Execute(CreateScope<doc::RemoveAssetCommand>(asset));
    }

    void EditorState::OpenComponent(Uuid component) {
        const doc::Node* node = m_Document.Find(component);
        if (!node || !node->IsComponent() || component == m_ActiveScreen) return;
        if (!EditingComponent().Valid()) m_ScreenBehind = m_ActiveScreen;
        SetActiveScreen(component);
    }

    Uuid EditorState::EditingComponent() const {
        const doc::Node* node = m_Document.Find(m_ActiveScreen);
        return node && node->IsComponent() ? m_ActiveScreen : Uuid::Invalid();
    }

    void EditorState::CloseComponent() {
        if (!EditingComponent().Valid()) return;
        const auto screens = Screens();
        Uuid back = m_ScreenBehind;
        if (!m_Document.Contains(back) || m_Document.Find(back)->kind != doc::NodeKind::Screen)
            back = screens.empty() ? Uuid::Invalid() : screens.front();
        if (back.Valid()) SetActiveScreen(back);
    }

    Uuid EditorState::ComponentOwning(Uuid node) const {
        for (Uuid at = node; at.Valid(); ) {
            const doc::Node* current = m_Document.Find(at);
            if (!current) break;
            if (current->IsComponent()) return at;
            at = current->parent;
        }
        return Uuid::Invalid();
    }

    void EditorState::SetSlot(Uuid node, bool slot) {
        const Uuid component = ComponentOwning(node);
        if (!component.Valid() || component == node) return;

        // One slot per component: two would need a rule for which children go where, and there is
        // no such rule that a designer would guess right.
        if (slot)
            for (Uuid id : m_Document.Subtree(component))
                if (doc::Node* other = m_Document.Find(id); other && other->slot && id != node) {
                    other->slot = false;
                    m_Document.Touch(id);
                }

        doc::Node* target = m_Document.Find(node);
        if (!target || target->slot == slot) return;
        target->slot = slot;
        m_Document.Touch(node);
    }

    bool EditorState::CanMakeComponent() const {
        const doc::Node* node = m_Document.Find(Primary());
        return node && node->kind != doc::NodeKind::Screen && !node->IsInstance()
            && !node->IsComponent() && node->parent.Valid();
    }

    Uuid EditorState::MakeComponentFromSelection(std::string name) {
        if (!CanMakeComponent()) return Uuid::Invalid();
        const Uuid node = Primary();
        const doc::Node* source = m_Document.Find(node);

        const Uuid parent = source->parent;
        const u32 index = m_Document.IndexInParent(node);
        const layout::LayoutStyle style = source->layout;
        if (name.empty()) name = source->name.empty() ? "Component" : source->name;
        // A second component called "Card" would shadow the first in the library index, and the
        // one that lost would be unplaceable with no sign of why.
        if (m_Library.Find(name).Valid()) {
            const std::string base = name;
            for (int n = 2; m_Library.Find(name).Valid(); ++n) name = base + " " + std::to_string(n);
        }

        // Out of the screen and into the document's roots: a component is a definition, and what a
        // screen holds is instances of it.
        m_Commands.BeginTransaction("Make component");
        m_Document.Reparent(node, Uuid::Invalid());
        const Uuid component = m_Document.MakeComponent(node, name);
        const Uuid instance = m_Document.CreateInstance(component, parent);
        if (instance.Valid()) {
            m_Document.Reorder(instance, index);
            // The instance keeps the box the frame had, so making a component changes nothing about
            // what is on the screen.
            m_Document.Find(instance)->layout = style;
            m_Document.Touch(instance);
        }
        m_Commands.EndTransaction(m_Document);
        m_Commands.Break();

        m_Library.components[name] = component;
        if (instance.Valid()) Select(instance);
        VAE_INFO("made a component called '{}'", name);
        return instance;
    }

    void EditorState::CopySelection() {
        if (m_Selection.empty()) return;

        // The clipboard holds a document, not a private blob, so there is one format to keep
        // working rather than two — and a copy can be pasted into a project that has never seen
        // this one.
        doc::Document scratch;
        u32 copied = 0;
        for (Uuid id : m_Selection) {
            const doc::Node* node = m_Document.Find(id);
            if (!node || node->kind == doc::NodeKind::Screen) continue;

            // A component the selection instances travels with it, unless it is one of the
            // standard catalog's — every VAE rebuilds those from the library reference.
            if (node->IsInstance() && m_Document.Contains(node->componentId)
                && !scratch.Contains(node->componentId)) {
                const bool stock = std::any_of(
                    m_Library.components.begin(), m_Library.components.end(),
                    [&](const auto& entry) { return entry.second == node->componentId; });
                if (!stock)
                    doc::CopySubtreeInto(m_Document, node->componentId, scratch, Uuid::Invalid(), false);
            }
            doc::CopySubtreeInto(m_Document, id, scratch, Uuid::Invalid(), false);
            ++copied;
        }
        if (copied == 0) return;

        // keepIds: an instance in here points at a component in here, and its overrides are keyed
        // by the ids of nodes inside that component.
        Input::SetClipboardText(doc::Serializer::ToXml(scratch, true, &ui::StandardLibrary(), true));
        VAE_INFO("copied {} node(s)", copied);
    }

    void EditorState::CutSelection() {
        if (m_Selection.empty()) return;
        CopySelection();
        DeleteSelection();
    }

    bool EditorState::CanPaste() const {
        return Input::ClipboardText().find("<vae") != std::string::npos;
    }

    u32 EditorState::Paste() {
        const std::string text = Input::ClipboardText();
        if (text.find("<vae") == std::string::npos) return 0;

        doc::Document incoming;
        std::string error;
        if (!doc::Serializer::FromText(text, incoming, &error, &ui::StandardLibrary())) {
            VAE_WARN("paste: {}", error);
            return 0;
        }

        // Into the selected container when there is one, otherwise into the screen: pasting into a
        // label would be pasting into something that cannot hold it.
        Uuid target = m_ActiveScreen;
        if (!m_Selection.empty()) {
            const doc::Node* node = m_Document.Find(Primary());
            if (node && !node->IsInstance()
                && (node->kind == doc::NodeKind::Frame || node->kind == doc::NodeKind::Screen))
                target = Primary();
        }
        if (!m_Document.Contains(target)) return 0;

        std::vector<Uuid> arrived;
        m_Commands.BeginTransaction("Paste");
        for (Uuid root : incoming.Roots()) {
            const doc::Node* node = incoming.Find(root);
            if (!node) continue;

            if (node->IsComponent()) {
                // A component the clipboard brought with it, under the id it had — an instance
                // that arrives beside it has to still find it. Already here means already here.
                if (!m_Document.Contains(root))
                    doc::CopySubtreeInto(incoming, root, m_Document, Uuid::Invalid(), false);
                continue;
            }
            const Uuid landed = doc::CopySubtreeInto(incoming, root, m_Document, target, true);
            if (landed.Valid()) arrived.push_back(landed);
        }
        m_Commands.EndTransaction(m_Document);
        m_Commands.Break();

        if (!arrived.empty()) SelectMany(arrived);
        VAE_INFO("pasted {} node(s)", arrived.size());
        return static_cast<u32>(arrived.size());
    }

    void EditorState::DeleteSelection() {
        if (m_Selection.empty()) return;
        m_Commands.BeginTransaction(m_Selection.size() == 1 ? "Delete" : "Delete selection");
        for (Uuid id : m_Selection) {
            // A node inside a component master is not the designer's to delete from a screen, and
            // a screen root is removed from the Screens panel rather than the canvas.
            const doc::Node* node = m_Document.Find(id);
            if (!node || node->kind == doc::NodeKind::Screen) continue;
            Execute(CreateScope<doc::DeleteNodeCommand>(id));
        }
        m_Commands.EndTransaction(m_Document);
        m_Commands.Break();
        ClearSelection();
    }

    void EditorState::DuplicateSelection() {
        if (m_Selection.empty()) return;
        std::vector<Uuid> copies;
        m_Commands.BeginTransaction("Duplicate");
        for (Uuid id : m_Selection) {
            const doc::Node* node = m_Document.Find(id);
            if (!node || node->kind == doc::NodeKind::Screen) continue;

            // Deep, and beside the original. A frame duplicated without its children is an empty
            // frame, which is what this produced for anything that was not a leaf.
            auto command = CreateScope<doc::CloneCommand>(id, node->parent,
                                                          m_Document.IndexInParent(id) + 1);
            doc::CloneCommand* raw = command.get();
            Execute(std::move(command));
            const Uuid copy = raw->Created();
            if (!copy.Valid()) continue;

            layout::LayoutStyle style = m_Document.Find(copy)->layout;
            style.offsetStart += Vec2{ 16.0f, 16.0f };
            SetLayout(copy, style);
            copies.push_back(copy);
        }
        m_Commands.EndTransaction(m_Document);
        m_Commands.Break();
        if (!copies.empty()) SelectMany(std::move(copies));
    }

    void EditorState::Undo() {
        m_Commands.Break();
        if (m_Commands.Undo(m_Document)) PruneSelection();
    }

    void EditorState::Redo() {
        m_Commands.Break();
        if (m_Commands.Redo(m_Document)) PruneSelection();
    }

    bool EditorState::Save(const std::filesystem::path& path) {
        if (doc::Project::IsProjectFile(path)) {
            if (m_Project.name == "Untitled") m_Project.name = path.stem().string();
            if (!doc::Project::SaveDocument(m_Document, m_Project, path, &ui::StandardLibrary()))
                return false;
        } else if (!doc::Serializer::Save(m_Document, path, &ui::StandardLibrary())) {
            return false;
        }
        m_Path = path;
        m_SavedRevision = m_Document.Revision();
        // There is nothing to recover any more, and a stale recovery file is worse than none: it
        // offers to put back work that has already been saved over.
        DiscardRecovery();
        VAE_INFO("saved {}", path.string());
        return true;
    }

    std::filesystem::path EditorState::RecoveryPathFor(const std::filesystem::path& project) {
        if (project.empty()) return {};
        return std::filesystem::path(project) += ".recovery";
    }

    bool EditorState::HasRecovery(const std::filesystem::path& project) {
        const std::filesystem::path recovery = RecoveryPathFor(project);
        std::error_code ec;
        if (recovery.empty() || !std::filesystem::exists(recovery, ec)) return false;
        if (!std::filesystem::exists(project, ec)) return true;
        // Older than the file it shadows means the project was saved after it was written, by a
        // build that never got to clean it up.
        return std::filesystem::last_write_time(recovery, ec)
             > std::filesystem::last_write_time(project, ec);
    }

    void EditorState::DiscardRecovery() {
        std::error_code ec;
        const std::filesystem::path recovery = RecoveryPathFor(m_Path);
        if (!recovery.empty()) std::filesystem::remove(recovery, ec);
    }

    std::vector<std::string> EditorState::Locales() const {
        return doc::LocalesIn(doc::StringsDirFor(m_Path));
    }

    const doc::StringTable* EditorState::Strings() const {
        return m_Locale.empty() ? nullptr : &m_Strings;
    }

    void EditorState::SetLocale(std::string locale) {
        if (locale == m_Locale) return;
        m_Locale = std::move(locale);
        m_Strings.Clear();
        if (m_Locale.empty()) return;

        std::string error;
        const std::filesystem::path file = doc::StringsDirFor(m_Path) / (m_Locale + ".json");
        m_Strings.SetLocale(m_Locale);
        if (!m_Strings.Load(file, &error))
            VAE_WARN("locale {}: {} — showing the authored text", m_Locale, error);
        else
            VAE_INFO("previewing {} ({} strings)", m_Locale, m_Strings.Count());
    }

    bool EditorState::WriteStrings(const std::string& locale) {
        if (locale.empty() || m_Path.empty()) return false;

        // Load what is there first, so re-running after adding a screen adds the new keys and
        // leaves every translation already in the file alone.
        doc::StringTable table;
        table.SetLocale(locale);
        const std::filesystem::path file = doc::StringsDirFor(m_Path) / (locale + ".json");
        std::string ignored;
        table.Load(file, &ignored);

        const std::size_t before = table.Count();
        table.CollectFrom(m_Document);
        if (!table.Save(file)) {
            VAE_ERROR("could not write {}", file.string());
            return false;
        }
        VAE_INFO("wrote {} ({} strings, {} new)", file.string(), table.Count(),
                 table.Count() - before);
        // Previewing what was just written is what anyone wants next.
        if (locale == m_Locale) { m_Locale.clear(); SetLocale(locale); }
        return true;
    }

    void EditorState::Autosave() {
        if (m_Path.empty() || !Dirty()) return;
        const std::filesystem::path recovery = RecoveryPathFor(m_Path);
        if (recovery.empty()) return;
        if (doc::Serializer::Save(m_Document, recovery, &ui::StandardLibrary()))
            VAE_CORE_INFO("autosaved to {}", recovery.string());
    }

    bool EditorState::Load(const std::filesystem::path& path) {
        doc::Document loaded;
        doc::Project project;
        std::string error;
        const bool ok = doc::Project::IsProjectFile(path)
            ? doc::Project::LoadDocument(path, loaded, project, &error, &ui::StandardLibrary())
            : doc::Serializer::Load(path, loaded, &error, &ui::StandardLibrary());
        if (!ok) {
            VAE_ERROR("could not open {}: {}", path.string(), error);
            return false;
        }

        m_Document = std::move(loaded);
        m_Project = std::move(project);
        m_Commands.Clear();
        m_Selection.clear();
        m_Path = path;
        m_SavedRevision = m_Document.Revision();

        // A loaded document brings its own components; rebuild the library index from what is
        // actually in it rather than assuming the standard set.
        m_Library.components.clear();
        for (Uuid id : m_Document.Roots()) {
            const doc::Node* node = m_Document.Find(id);
            if (node && node->IsComponent()) m_Library.components[node->name] = id;
        }
        const auto screens = Screens();
        m_ActiveScreen = screens.empty() ? Uuid::Invalid() : screens.front();
        VAE_INFO("opened {} ({} nodes)", path.string(), m_Document.NodeCount());
        return true;
    }

}
