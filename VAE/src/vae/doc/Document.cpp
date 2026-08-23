#include "vaepch.h"
#include "vae/doc/Document.h"

#include <algorithm>

namespace vae::doc {

    // A typo in a bound number should not try to build a hundred thousand nodes.
    static constexpr u32 kMaxRepeat = 2000;

    namespace {
        // A component that (transitively) instantiates itself would expand forever. The depth cap
        // is the backstop; MakeComponent/CreateInstance also refuse the direct cases.
        constexpr u32 kMaxInstanceDepth = 32;
    }

    const char* NodeKindName(NodeKind kind) {
        switch (kind) {
            case NodeKind::Frame:     return "frame";
            case NodeKind::Text:      return "text";
            case NodeKind::Image:     return "image";
            case NodeKind::Vector:    return "vector";
            case NodeKind::Instance:  return "instance";
            case NodeKind::Component: return "component";
            case NodeKind::Screen:    return "screen";
        }
        return "frame";
    }

    std::optional<NodeKind> NodeKindFromName(std::string_view name) {
        for (u8 i = 0; i <= static_cast<u8>(NodeKind::Screen); ++i)
            if (name == NodeKindName(static_cast<NodeKind>(i))) return static_cast<NodeKind>(i);
        return std::nullopt;
    }

    Document::Document() = default;

    void Document::Clear() {
        PopIdScope();
        m_Nodes.clear();
        m_Roots.clear();
        m_Tokens.clear();
        m_Assets.clear();
        ++m_Revision;
    }

    u32 Document::AddObserver(Observer observer) {
        const u32 handle = m_NextObserver++;
        m_Observers.emplace_back(handle, std::move(observer));
        return handle;
    }

    void Document::RemoveObserver(u32 handle) {
        std::erase_if(m_Observers, [handle](const auto& entry) { return entry.first == handle; });
    }

    void Document::Notify(Uuid changed) {
        ++m_Revision;
        // Copy first: an observer is allowed to add or remove observers in response.
        auto observers = m_Observers;
        for (auto& [handle, observer] : observers) observer(changed);
    }

    Node* Document::Find(Uuid id) {
        auto it = m_Nodes.find(id);
        return it == m_Nodes.end() ? nullptr : &it->second;
    }

    const Node* Document::Find(Uuid id) const {
        auto it = m_Nodes.find(id);
        return it == m_Nodes.end() ? nullptr : &it->second;
    }

    void Document::PushIdScope(std::string scope) {
        m_IdScope = std::move(scope);
        m_IdCounter = 0;
    }

    void Document::PopIdScope() {
        m_IdScope.clear();
        m_IdCounter = 0;
    }

    Uuid Document::CreateNode(NodeKind kind, Uuid parent, std::string name) {
        Node node;
        node.id = m_IdScope.empty()
                    ? Uuid{}
                    : Uuid::FromName(m_IdScope + "#" + std::to_string(m_IdCounter++));
        node.kind = kind;
        node.name = name.empty() ? NodeKindName(kind) : std::move(name);
        node.parent = parent;

        const Uuid id = node.id;
        m_Nodes.emplace(id, std::move(node));

        if (Node* parentNode = Find(parent)) parentNode->children.push_back(id);
        else                                 m_Roots.push_back(id);

        Notify(id);
        return id;
    }

    void Document::InsertNode(Node node, u32 index) {
        const Uuid id = node.id;
        const Uuid parent = node.parent;
        m_Nodes[id] = std::move(node);

        auto& siblings = Find(parent) ? Find(parent)->children : m_Roots;
        const u32 at = std::min(index, static_cast<u32>(siblings.size()));
        siblings.insert(siblings.begin() + at, id);

        Notify(id);
    }

    void Document::DetachFromParent(Uuid id) {
        const Node* node = Find(id);
        if (!node) return;
        auto& siblings = Find(node->parent) ? Find(node->parent)->children : m_Roots;
        std::erase(siblings, id);
    }

    void Document::DeleteNode(Uuid id) {
        if (!Contains(id)) return;

        // Collect first, then erase: erasing while walking invalidates the children we still need.
        const std::vector<Uuid> subtree = Subtree(id);
        DetachFromParent(id);
        for (Uuid node : subtree) m_Nodes.erase(node);

        Notify(id);
    }

    void Document::Reparent(Uuid id, Uuid newParent, u32 index) {
        Node* node = Find(id);
        if (!node) return;
        // Reparenting a node into its own subtree would orphan the whole branch from the roots.
        if (newParent.Valid() && (id == newParent || IsAncestor(id, newParent))) {
            VAE_CORE_WARN("refusing to reparent a node into its own subtree");
            return;
        }

        DetachFromParent(id);
        node->parent = newParent;

        auto& siblings = Find(newParent) ? Find(newParent)->children : m_Roots;
        const u32 at = std::min(index, static_cast<u32>(siblings.size()));
        siblings.insert(siblings.begin() + at, id);

        Notify(id);
    }

    void Document::Reorder(Uuid id, u32 index) {
        const Node* node = Find(id);
        if (!node) return;
        auto& siblings = Find(node->parent) ? Find(node->parent)->children : m_Roots;
        std::erase(siblings, id);
        siblings.insert(siblings.begin() + std::min(index, static_cast<u32>(siblings.size())), id);
        Notify(id);
    }

    u32 Document::IndexInParent(Uuid id) const {
        const Node* node = Find(id);
        if (!node) return 0;
        const auto& siblings = Find(node->parent) ? Find(node->parent)->children : m_Roots;
        const auto it = std::find(siblings.begin(), siblings.end(), id);
        return it == siblings.end() ? 0 : static_cast<u32>(std::distance(siblings.begin(), it));
    }

    std::vector<Uuid> Document::AllNodes() const {
        std::vector<Uuid> out;
        out.reserve(m_Nodes.size());
        for (const auto& [id, node] : m_Nodes) { (void)node; out.push_back(id); }
        return out;
    }

    std::vector<Uuid> Document::Subtree(Uuid root) const {
        std::vector<Uuid> out;
        if (!Contains(root)) return out;

        std::vector<Uuid> stack{ root };
        while (!stack.empty()) {
            const Uuid id = stack.back();
            stack.pop_back();
            out.push_back(id);
            if (const Node* node = Find(id))
                for (auto it = node->children.rbegin(); it != node->children.rend(); ++it)
                    stack.push_back(*it);
        }
        return out;
    }

    bool Document::IsAncestor(Uuid ancestor, Uuid descendant) const {
        for (const Node* node = Find(descendant); node; node = Find(node->parent)) {
            if (node->parent == ancestor) return true;
            if (!node->parent.Valid()) break;
        }
        return false;
    }

    void Document::SetProp(Uuid id, Prop prop, Value value) {
        Node* node = Find(id);
        if (!node) return;
        // Writing the value it already has is not a change. Saying it is costs a view-tree rebuild,
        // and a rebuild between a press and a release is a click that never happens.
        const Value* existing = node->props.Find(prop);
        if (existing ? *existing == value : !IsSet(value)) return;
        node->props.Set(prop, std::move(value));
        Notify(id);
    }

    void Document::SetProp(Uuid id, std::string key, Value value) {
        Node* node = Find(id);
        if (!node) return;
        const Value* existing = node->props.Find(key);
        if (existing ? *existing == value : !IsSet(value)) return;
        node->props.Set(std::move(key), std::move(value));
        Notify(id);
    }

    std::vector<Uuid> Document::Screens() const {
        std::vector<Uuid> screens;
        for (const Uuid root : m_Roots)
            if (const Node* node = Find(root); node && node->kind == NodeKind::Screen)
                screens.push_back(root);
        return screens;
    }

    Uuid Document::StartScreen() const {
        if (const Node* chosen = Find(m_StartScreen); chosen && chosen->kind == NodeKind::Screen)
            return m_StartScreen;
        const std::vector<Uuid> screens = Screens();
        return screens.empty() ? Uuid::Invalid() : screens.front();
    }

    ScreenKind Document::KindOf(Uuid screen) const {
        const Node* node = Find(screen);
        if (!node) return ScreenKind::Page;
        const Value* value = node->props.Find(Prop::ScreenKind);
        if (!value || TypeOf(*value) != ValueType::Text) return ScreenKind::Page;
        return ScreenKindFromName(std::get<std::string>(*value)).value_or(ScreenKind::Page);
    }

    void Document::Touch(Uuid id) { Notify(id); }

    Value Document::GetProp(Uuid id, Prop prop) const {
        const Node* node = Find(id);
        if (!node) return {};
        const Value* value = node->props.Find(prop);
        return value ? *value : Value{};
    }

    Value Document::ResolveValue(const Value& value) const {
        const TokenRef* ref = std::get_if<TokenRef>(&value);
        if (!ref) return value;

        const Token* token = FindToken(ref->name);
        if (!token) {
            VAE_CORE_WARN("token '{}' is not defined", ref->name);
            return {};
        }
        const Value& picked = m_Theme == Theme::Dark ? token->dark : token->light;
        // One level of indirection: a token may alias another token, but not endlessly.
        return std::holds_alternative<TokenRef>(picked) ? ResolveValue(picked) : picked;
    }

    void Document::SetToken(const std::string& name, Token token) {
        m_Tokens[name] = std::move(token);
        Notify(Uuid::Invalid());
    }

    void Document::RemoveToken(const std::string& name) {
        m_Tokens.erase(name);
        Notify(Uuid::Invalid());
    }

    const Token* Document::FindToken(std::string_view name) const {
        auto it = m_Tokens.find(std::string(name));
        return it == m_Tokens.end() ? nullptr : &it->second;
    }

    Uuid Document::AddAsset(std::string name, std::string path, Uuid id) {
        if (!id.Valid()) id = Uuid{};   // default-constructed is random
        for (Asset& asset : m_Assets)
            if (asset.id == id) { asset.name = std::move(name); asset.path = std::move(path); 
                                  Notify(Uuid::Invalid()); return id; }
        m_Assets.push_back({ id, std::move(name), std::move(path) });
        Notify(Uuid::Invalid());
        return id;
    }

    void Document::RemoveAsset(Uuid id) {
        const auto it = std::find_if(m_Assets.begin(), m_Assets.end(),
                                     [&](const Asset& asset) { return asset.id == id; });
        if (it == m_Assets.end()) return;
        m_Assets.erase(it);
        Notify(Uuid::Invalid());
    }

    const Document::Asset* Document::FindAsset(Uuid id) const {
        const auto it = std::find_if(m_Assets.begin(), m_Assets.end(),
                                     [&](const Asset& asset) { return asset.id == id; });
        return it == m_Assets.end() ? nullptr : &*it;
    }

    const Document::Asset* Document::FindAssetNamed(std::string_view name) const {
        if (name.empty()) return nullptr;
        for (const Asset& asset : m_Assets)
            if (asset.name == name) return &asset;
        return nullptr;
    }

    Uuid Document::MakeComponent(Uuid subtreeRoot, std::string name) {
        Node* node = Find(subtreeRoot);
        if (!node) return Uuid::Invalid();

        node->kind = NodeKind::Component;
        if (!name.empty()) node->name = std::move(name);
        Notify(subtreeRoot);
        return subtreeRoot;
    }

    Uuid Document::CreateInstance(Uuid componentId, Uuid parent) {
        const Node* component = Find(componentId);
        if (!component || !component->IsComponent()) {
            VAE_CORE_WARN("cannot instantiate {}: not a component", componentId.ToString());
            return Uuid::Invalid();
        }
        // An instance placed inside its own component is the direct self-reference case.
        if (parent == componentId || IsAncestor(componentId, parent)) {
            VAE_CORE_WARN("refusing to place an instance of a component inside itself");
            return Uuid::Invalid();
        }

        const Uuid id = CreateNode(NodeKind::Instance, parent, component->name);
        Node* instance = Find(id);
        instance->componentId = componentId;
        // Inherit the component's shape once, at creation. From here the instance owns it, so it
        // can be resized and re-stacked without detaching. The cost is that a later change to the
        // master's own layout does not follow into instances that already exist.
        instance->layout = component->layout;
        Notify(id);
        return id;
    }

    void Document::SetOverride(Uuid instance, Uuid nodeInComponent, Prop prop, Value value) {
        Node* node = Find(instance);
        if (!node || !node->IsInstance()) return;
        PropBag& bag = node->overrides[nodeInComponent];
        const Value* existing = bag.Find(prop);
        if (existing ? *existing == value : !IsSet(value)) return;
        bag.Set(prop, std::move(value));
        Notify(instance);
    }

    void Document::ClearOverride(Uuid instance, Uuid nodeInComponent, Prop prop) {
        Node* node = Find(instance);
        if (!node || !node->IsInstance()) return;

        auto it = node->overrides.find(nodeInComponent);
        if (it == node->overrides.end()) return;
        it->second.Unset(prop);
        if (it->second.Empty()) node->overrides.erase(it);
        Notify(instance);
    }

    void Document::SetOverride(Uuid instance, Uuid nodeInComponent, std::string key, Value value) {
        Node* node = Find(instance);
        if (!node || !node->IsInstance()) return;
        PropBag& bag = node->overrides[nodeInComponent];
        const Value* existing = bag.Find(key);
        if (existing ? *existing == value : !IsSet(value)) return;
        bag.Set(std::move(key), std::move(value));
        Notify(instance);
    }

    void Document::ClearOverride(Uuid instance, Uuid nodeInComponent, std::string_view key) {
        Node* node = Find(instance);
        if (!node || !node->IsInstance()) return;

        auto it = node->overrides.find(nodeInComponent);
        if (it == node->overrides.end()) return;
        it->second.Unset(key);
        if (it->second.Empty()) node->overrides.erase(it);
        Notify(instance);
    }

    const std::vector<ComponentProperty>& Document::PropertiesOf(Uuid component) const {
        static const std::vector<ComponentProperty> kNone;
        const Node* node = Find(component);
        return node ? node->properties : kNone;
    }

    const ComponentProperty* Document::FindProperty(Uuid component, std::string_view name) const {
        for (const ComponentProperty& property : PropertiesOf(component))
            if (property.name == name) return &property;
        return nullptr;
    }

    void Document::SetComponentProperty(Uuid component, ComponentProperty property) {
        Node* node = Find(component);
        if (!node || property.name.empty()) return;
        for (ComponentProperty& existing : node->properties)
            if (existing.name == property.name) { existing = std::move(property); Touch(component); return; }
        node->properties.push_back(std::move(property));
        Touch(component);
    }

    void Document::RemoveComponentProperty(Uuid component, std::string_view name) {
        Node* node = Find(component);
        if (!node) return;
        std::erase_if(node->properties, [&](const ComponentProperty& p) { return p.name == name; });
        Touch(component);
    }

    Value Document::InstanceProperty(Uuid instance, std::string_view name) const {
        const Node* node = Find(instance);
        if (!node || !node->IsInstance()) return {};
        if (const Value* picked = node->props.Find(InstancePropertyKey(name))) return *picked;
        if (const ComponentProperty* declared = FindProperty(node->componentId, name))
            return declared->defaultValue;
        return {};
    }

    void Document::SetInstanceProperty(Uuid instance, std::string_view name, Value value) {
        Node* node = Find(instance);
        if (!node || !node->IsInstance()) return;
        node->props.Set(InstancePropertyKey(name), std::move(value));
        Touch(instance);
    }

    void Document::ClearInstanceProperty(Uuid instance, std::string_view name) {
        Node* node = Find(instance);
        if (!node || !node->IsInstance()) return;
        node->props.Unset(InstancePropertyKey(name));
        Touch(instance);
    }

    namespace {

        // Text of a value, for the variant overlays whose keys are spelled with it.
        std::string OptionText(const Value& value) {
            if (const auto* text = std::get_if<std::string>(&value)) return *text;
            if (const auto* flag = std::get_if<bool>(&value)) return *flag ? "true" : "false";
            return {};
        }

    }

    // The two things a component property does to a node inside that component: it answers a
    // binding that names it, and — when it is a variant — it switches on the overlays keyed to the
    // option that was picked.
    void Document::ApplyComponentProperties(Uuid component, Uuid instance, PropBag& bag) const {
        const std::vector<ComponentProperty>& properties = PropertiesOf(component);
        if (properties.empty()) return;

        for (const ComponentProperty& property : properties) {
            Value value = property.defaultValue;
            if (instance.Valid()) {
                if (const Node* node = Find(instance))
                    if (const Value* picked = node->props.Find(InstancePropertyKey(property.name)))
                        value = *picked;
            }

            // A binding that names this property is answered with the value. One that names
            // anything else is a binding over the app's own state and is left alone for the
            // runtime, which is the only thing that can evaluate it.
            const auto answer = [&](const Value& current) {
                const auto* binding = std::get_if<Binding>(&current);
                return binding && binding->expression == property.name;
            };
            std::vector<std::pair<Prop, Value>> known;
            for (const auto& [key, current] : bag.Known())
                if (answer(current)) known.emplace_back(key, value);
            for (const auto& [key, replacement] : known) bag.Set(key, replacement);

            std::vector<std::pair<std::string, Value>> custom;
            for (const auto& [key, current] : bag.Custom())
                if (answer(current)) custom.emplace_back(key, value);
            for (const auto& [key, replacement] : custom) bag.Set(key, replacement);

            if (!property.IsVariant()) continue;

            // The overlay for the option that was picked. Applied after the bindings so a variant
            // may set a property a binding also names, and after nothing else — an instance's own
            // override still wins, because that is applied later still.
            const std::string prefix = VariantOverlayPrefix(property.name, OptionText(value));
            std::vector<std::pair<std::string, Value>> apply;
            for (const auto& [key, current] : bag.Custom())
                if (key.starts_with(prefix)) apply.emplace_back(key.substr(prefix.size()), current);
            for (const auto& [name, current] : apply) {
                if (auto prop = PropFromName(name)) bag.Set(*prop, current);
                else bag.Set(name, current);
            }
        }
    }

    PropBag Document::ResolvedProps(Uuid instance, Uuid nodeInComponent) const {
        PropBag result;
        if (const Node* source = Find(nodeInComponent)) result = source->props;

        if (const Node* node = Find(instance); node && node->IsInstance()) {
            ApplyComponentProperties(node->componentId, instance, result);
            auto it = node->overrides.find(nodeInComponent);
            if (it != node->overrides.end()) it->second.MergeInto(result);
        }
        return result;
    }

    namespace {

        // One instance's say about one node, if it has one.
        void MergeOverride(const Node* instance, Uuid key, PropBag& into) {
            if (!instance || !instance->IsInstance()) return;
            auto it = instance->overrides.find(key);
            if (it != instance->overrides.end()) it->second.MergeInto(into);
        }

    }

    PropBag Document::ResolvedProps(const std::vector<Uuid>& chain, Uuid node) const {
        PropBag result;
        if (const Node* source = Find(node)) {
            if (source->IsInstance()) {
                // An instance's own props are its component's, plus whatever it says about that root.
                if (const Node* component = Find(source->componentId)) result = component->props;
                ApplyComponentProperties(source->componentId, source->id, result);
                MergeOverride(source, source->componentId, result);
            } else {
                result = source->props;
            }
        }
        // The innermost instance that owns this node decides what its component's properties mean.
        // Outer instances are containers around it and have no say in that — their own properties
        // belong to their own subtrees.
        if (!chain.empty()) {
            if (const Node* inner = Find(chain.back()); inner && inner->IsInstance())
                ApplyComponentProperties(inner->componentId, inner->id, result);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            MergeOverride(Find(*it), node, result);
        return result;
    }

    Uuid Document::SlotOf(Uuid component) const {
        const Node* root = Find(component);
        if (!root) return Uuid::Invalid();
        // A component can be its own slot — a grid of cells, a group of options — in which case the
        // cells it ships with are the placeholder and an instance's children replace them outright.
        if (root->slot) return component;
        std::vector<Uuid> stack{ root->children.rbegin(), root->children.rend() };
        while (!stack.empty()) {
            const Uuid id = stack.back();
            stack.pop_back();
            const Node* node = Find(id);
            if (!node) continue;
            if (node->slot) return id;
            // Not through a nested instance: that component's slot belongs to that component, and
            // filling it from out here would put a page's content two definitions deep.
            if (node->IsInstance()) continue;
            for (auto it = node->children.rbegin(); it != node->children.rend(); ++it)
                stack.push_back(*it);
        }
        return Uuid::Invalid();
    }

    Uuid CloneSubtree(Document& document, Uuid source, Uuid parent, u32 index) {
        return CopySubtreeInto(document, source, document, parent, true, index);
    }

    Uuid CopySubtreeInto(const Document& from, Uuid root, Document& into, Uuid parent,
                         bool freshIds, u32 index) {
        if (!from.Contains(root)) return Uuid::Invalid();

        // Parents before children, so a child's new parent already exists when it is inserted.
        const std::vector<Uuid> subtree = from.Subtree(root);
        std::unordered_map<Uuid, Uuid> remap;
        remap.reserve(subtree.size());

        // A component subtree keeps its ids even on a paste: an instance's overrides are keyed by
        // the ids of the nodes inside the component, so a component that arrived under new ids
        // would arrive with every override pointing at nothing. Decided once for the whole
        // subtree, because "inside a component" is a property of the root, not of each node.
        const Node* rootNode = from.Find(root);
        const bool keepIds = !freshIds || (rootNode && rootNode->IsComponent());

        for (Uuid id : subtree) {
            const Node* original = from.Find(id);
            if (!original) continue;

            Node copy = *original;
            if (!keepIds) copy.id = Uuid();
            copy.children.clear();
            // The root lands where the caller asked; everything else lands under its own copy.
            copy.parent = id == root ? parent : remap[original->parent];
            remap[id] = copy.id;
            into.InsertNode(std::move(copy), id == root ? index : UINT32_MAX);
        }

        // A property that pointed at a node inside what was copied now points at the copy of it.
        // One that pointed outside is left alone: it still means the node it named.
        for (const auto& [before, after] : remap) {
            Node* node = into.Find(after);
            if (!node) continue;
            for (const auto& [prop, value] : node->props.Known()) {
                if (const Uuid* target = std::get_if<Uuid>(&value)) {
                    const auto it = remap.find(*target);
                    if (it != remap.end()) node->props.Set(prop, it->second);
                }
            }
            for (const auto& [key, value] : node->props.Custom()) {
                if (const Uuid* target = std::get_if<Uuid>(&value)) {
                    const auto it = remap.find(*target);
                    if (it != remap.end()) node->props.Set(key, it->second);
                }
            }
        }
        return remap[root];
    }

    // ---------------------------------------------------------------------------- rows

    u32 RowTable::Count() const {
        return columns.empty() ? 0u : static_cast<u32>(cells.size() / columns.size());
    }

    i32 RowTable::ColumnOf(std::string_view name) const {
        for (std::size_t i = 0; i < columns.size(); ++i)
            if (columns[i] == name) return static_cast<i32>(i);
        return -1;
    }

    std::string_view RowTable::Cell(u32 row, u32 column) const {
        if (column >= columns.size()) return {};
        const std::size_t at = static_cast<std::size_t>(row) * columns.size() + column;
        return at < cells.size() ? std::string_view(cells[at]) : std::string_view{};
    }

    std::string_view RowTable::Cell(u32 row, std::string_view column) const {
        const i32 index = ColumnOf(column);
        return index < 0 ? std::string_view{} : Cell(row, static_cast<u32>(index));
    }

    namespace {

        std::string_view Trimmed(std::string_view s) {
            const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
            while (!s.empty() && space(s.front())) s.remove_prefix(1);
            while (!s.empty() && space(s.back()))  s.remove_suffix(1);
            return s;
        }

        std::vector<std::string> SplitCells(std::string_view line) {
            std::vector<std::string> out;
            std::size_t at = 0;
            while (true) {
                const std::size_t bar = line.find('|', at);
                out.emplace_back(Trimmed(line.substr(at, bar == std::string_view::npos
                                                          ? std::string_view::npos : bar - at)));
                if (bar == std::string_view::npos) break;
                at = bar + 1;
            }
            return out;
        }

    }

    RowTable ParseRowText(std::string_view text) {
        RowTable table;
        std::size_t at = 0;
        while (at <= text.size()) {
            const std::size_t nl = text.find('\n', at);
            const std::string_view line =
                Trimmed(text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
            at = nl == std::string_view::npos ? text.size() + 1 : nl + 1;

            if (line.empty()) continue;
            if (table.columns.empty()) { table.columns = SplitCells(line); continue; }

            // A short row pads and a long one drops: half a table still draws, which is the state
            // it is in for most of the time anyone spends typing it.
            std::vector<std::string> cells = SplitCells(line);
            cells.resize(table.columns.size());
            for (std::string& cell : cells) table.cells.push_back(std::move(cell));
        }
        // Column names with no rows under them describe a table with nothing in it, and a table
        // with nothing in it is not one.
        if (table.cells.empty()) table.columns.clear();
        return table;
    }

    std::string RowText(const RowTable& rows) {
        if (rows.columns.empty()) return {};
        std::string out;
        const auto line = [&out](const std::string* first, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                if (i) out += " | ";
                out += first[i];
            }
            out += '\n';
        };
        line(rows.columns.data(), rows.columns.size());
        for (u32 row = 0; row < rows.Count(); ++row)
            line(&rows.cells[static_cast<std::size_t>(row) * rows.columns.size()], rows.columns.size());
        return out;
    }

    namespace {

        // What a property holds, which is what a cell has to be read as. A cell is text on the
        // wire — the app said "3" or "online" or "" — and the property it lands on decides whether
        // that is a number, a token or a fact about whether the node is there at all.
        enum class Shape { Text, Number, Bool, Colour, Asset };

        Shape ShapeOf(Prop prop) {
            // A picture is the one cell that cannot be its own value: an AssetRef is a Uuid, and
            // nothing a person types into a table is a Uuid. The cell carries the asset's name.
            if (PropValueType(prop) == ValueType::Asset) return Shape::Asset;
            switch (prop) {
                case Prop::Fill: case Prop::Stroke: case Prop::TextColor: case Prop::ShadowColor:
                    return Shape::Colour;
                case Prop::Visible: case Prop::ClipContent: case Prop::Enabled: case Prop::Checked:
                case Prop::Multiline: case Prop::Password: case Prop::ReadOnly: case Prop::Open:
                case Prop::Selectable: case Prop::Modal: case Prop::FontItalic:
                case Prop::Resizable:
                    return Shape::Bool;
                case Prop::FillOpacity: case Prop::StrokeWidth: case Prop::CornerRadius:
                case Prop::Opacity: case Prop::ShadowBlur: case Prop::ShadowSpread:
                case Prop::FontSize: case Prop::FontWeight: case Prop::LineHeight:
                case Prop::LetterSpacing: case Prop::Value: case Prop::MinValue:
                case Prop::MaxValue: case Prop::Step: case Prop::SelectedIndex:
                case Prop::MaxLength: case Prop::ScrollX: case Prop::ScrollY:
                case Prop::ItemHeight: case Prop::ItemCount: case Prop::Duration:
                case Prop::ColumnWidth: case Prop::Repeat:
                    return Shape::Number;
                default:
                    return Shape::Text;
            }
        }

        bool TruthOf(std::string_view cell) {
            return !(cell.empty() || cell == "0" || cell == "false" || cell == "no"
                     || cell == "off");
        }

        // "#5865f2", or the name of a token. A colour written into data is nearly always the
        // second: a row saying `online` means the theme's green, not one particular green.
        Value ColourOf(std::string_view cell) {
            if (cell.empty()) return {};
            if (cell.front() != '#') return TokenRef{ std::string(cell) };

            u32 packed = 0;
            const std::string_view digits = cell.substr(1);
            if (digits.size() != 6 && digits.size() != 8) return TokenRef{ std::string(cell) };
            for (const char c : digits) {
                const u32 value = c >= '0' && c <= '9' ? static_cast<u32>(c - '0')
                                : c >= 'a' && c <= 'f' ? static_cast<u32>(c - 'a' + 10)
                                : c >= 'A' && c <= 'F' ? static_cast<u32>(c - 'A' + 10)
                                                       : 16u;
                if (value == 16u) return TokenRef{ std::string(cell) };
                packed = (packed << 4) | value;
            }
            const bool alpha = digits.size() == 8;
            const u32 rgb = alpha ? (packed >> 8) : packed;
            return Color{ static_cast<f32>((rgb >> 16) & 0xFF) / 255.0f,
                          static_cast<f32>((rgb >> 8) & 0xFF) / 255.0f,
                          static_cast<f32>(rgb & 0xFF) / 255.0f,
                          alpha ? static_cast<f32>(packed & 0xFF) / 255.0f : 1.0f };
        }

        // The binding on a node inside a row template: which column it draws, and which of its own
        // properties draws it. "author" fills the node's natural property — text on a label, the
        // picture on an image; "fill:tint" names one outright.
        void ApplyField(const Document& document, PropBag& props, NodeKind kind,
                        const RowTable& table, u32 row) {
            const Value* binding = props.Find(Prop::Field);
            if (!binding) return;
            const auto* text = std::get_if<std::string>(binding);
            if (!text || text->empty()) return;

            std::string_view column(*text);
            Prop target = kind == NodeKind::Image || kind == NodeKind::Vector ? Prop::Image
                                                                             : Prop::Text;
            if (const std::size_t colon = column.find(':'); colon != std::string_view::npos) {
                const std::optional<Prop> named = PropFromName(column.substr(0, colon));
                if (!named) return;
                target = *named;
                column = column.substr(colon + 1);
            }

            const std::string_view cell = table.Cell(row, column);
            switch (ShapeOf(target)) {
                case Shape::Text:   props.Set(target, std::string(cell)); break;
                case Shape::Bool:   props.Set(target, TruthOf(cell)); break;
                case Shape::Number: {
                    // A cell that is not a number leaves the authored value alone: a number that
                    // silently reads as zero is a row that silently disappears.
                    char* end = nullptr;
                    const std::string owned(cell);
                    const f32 value = std::strtof(owned.c_str(), &end);
                    if (end && end != owned.c_str()) props.Set(target, value);
                    break;
                }
                case Shape::Colour: {
                    Value colour = ColourOf(cell);
                    if (IsSet(colour)) props.Set(target, std::move(colour));
                    break;
                }
                case Shape::Asset: {
                    // An empty cell is a row with no picture, which is a picture of nothing. A
                    // name nothing answers to leaves the authored one alone, so a typo shows the
                    // placeholder the designer put there rather than a hole.
                    if (cell.empty()) { props.Set(target, Value{}); break; }
                    if (const Document::Asset* asset = document.FindAssetNamed(cell))
                        props.Set(target, AssetRef{ asset->id });
                    break;
                }
            }
        }

    }

    void Document::FlattenInto(std::vector<FlatNode>& out, Uuid id, u32 parent,
                               std::vector<Uuid>& chain, Uuid pathContext, u32 depth,
                               const SlotContent* slot, const RowLookup* rows,
                               const RowBinding* row) const {
        const Node* node = Find(id);
        if (!node) return;

        if (node->IsInstance()) {
            if (depth >= kMaxInstanceDepth) {
                VAE_CORE_ERROR("instance nesting exceeded {} levels at {} — recursive component?",
                               kMaxInstanceDepth, id.ToString());
                return;
            }
            const Node* component = Find(node->componentId);
            if (!component) {
                VAE_CORE_WARN("instance {} points at a missing component", id.ToString());
                return;
            }

            // The instance's own layout is authoritative: CreateInstance seeds it from the
            // component root, so the shape is inherited once and then belongs to the instance.
            // That is what lets the Inspector show real numbers and a resize handle mean something
            // — reading the component's layout here instead would silently discard both.
            // Two ids, because an instance nested in a component means two different things at
            // once: one authored node (which is where its overrides live) and one copy per copy of
            // the component around it (which is what a click, a caret or a script belongs to).
            const Uuid path = Uuid::Derive(pathContext, id);

            FlatNode flat;
            flat.sourceId = component->id;
            flat.instanceId = path;
            flat.overrideId = chain.empty() ? id : chain.front();
            flat.overrideKey = chain.empty() ? component->id : id;
            flat.authoredId = id;
            flat.parent = parent;
            flat.kind = component->kind == NodeKind::Component ? NodeKind::Frame : component->kind;
            flat.layout = node->layout;
            flat.name = node->name;
            flat.props = ResolvedProps(chain, id);
            if (row && row->table) ApplyField(*this, flat.props, flat.kind, *row->table, row->row);
            out.push_back(std::move(flat));

            const u32 index = static_cast<u32>(out.size() - 1);
            const Uuid slotNode = SlotOf(node->componentId);
            const bool fills = !node->children.empty() && slotNode.Valid();

            // The component is itself the slot, so the instance's children *are* its children —
            // and they stay in the page's scope, which is why the chain is not pushed here.
            if (fills && slotNode == component->id) {
                for (Uuid child : node->children)
                    FlattenInto(out, child, index, chain, pathContext, depth, nullptr, rows, row);
                return;
            }

            // What this instance is handing to the component's slot, remembered with the scope it
            // was authored in: those nodes are the page's, not the component's, and resolving them
            // against the component would file their overrides under the wrong instance.
            SlotContent content;
            if (fills) {
                content.children = &node->children;
                content.chain = chain;
                content.pathContext = pathContext;
            }

            chain.push_back(id);
            for (Uuid child : component->children)
                FlattenInto(out, child, index, chain, path, depth + 1, fills ? &content : nullptr,
                            rows, row);
            chain.pop_back();
            return;
        }

        FlatNode flat;
        flat.sourceId = id;
        flat.instanceId = pathContext;
        flat.overrideId = chain.empty() ? Uuid::Invalid() : chain.front();
        flat.overrideKey = id;
        flat.authoredId = id;
        flat.parent = parent;
        flat.kind = node->kind;
        flat.layout = node->layout;
        flat.name = node->name;
        // Inside an instance, every descendant's props go through the same override resolution as
        // the root — that is what makes "change the label on this one card" work.
        flat.props = ResolvedProps(chain, id);
        // Inside a copy, whatever the template said this node draws is filled in from the row.
        if (row && row->table) ApplyField(*this, flat.props, flat.kind, *row->table, row->row);
        out.push_back(std::move(flat));

        const u32 index = static_cast<u32>(out.size() - 1);

        // The slot: the instance's own children stand in for the component's placeholder ones, in
        // the scope they were written. Nothing supplied and the placeholder stays, which is what
        // makes a freshly dropped Card look like a card instead of an empty rectangle.
        if (node->slot && slot && slot->children && !slot->children->empty()) {
            std::vector<Uuid> outer = slot->chain;
            for (Uuid child : *slot->children)
                FlattenInto(out, child, index, outer, slot->pathContext, depth, nullptr, rows, row);
            return;
        }

        // A container that repeats: the first child is the template, drawn once per count, and
        // whatever else the container holds follows it. One row styled by hand and a number from a
        // script is the whole of "this list has data in it" — no second component, no template
        // language, and the copies are ordinary nodes that lay out and hit-test like any other.
        //
        // Each copy gets an identity of its own, or a checkbox in row three would be the same
        // widget as the one in row one and toggling either would toggle both.
        // Read from the node that was just pushed, not from `flat` — that one was moved out of.
        // Rows, when the app handed any over, say how many copies there are; the authored number
        // is what the designer sees before it runs. Rows win: a list of three messages is three
        // rows long whatever the placeholder said.
        const RowTable* table = rows && *rows ? (*rows)(id, pathContext) : nullptr;
        const f32 authored = out[index].props.Number(Prop::Repeat, 0.0f);
        const f32 repeats = table ? static_cast<f32>(table->Count()) : authored;
        if ((table || repeats >= 1.0f) && !node->children.empty()) {
            const u32 count = std::min(static_cast<u32>(std::max(repeats, 0.0f)), kMaxRepeat);
            if (static_cast<f32>(count) < repeats)
                VAE_CORE_WARN("'{}' asked to repeat {} times; {} is the limit", node->name,
                              static_cast<u32>(repeats), kMaxRepeat);

            const Uuid templateId = node->children.front();
            const std::string base = Find(templateId) ? Find(templateId)->name : std::string("Item");
            for (u32 i = 0; i < count; ++i) {
                const std::size_t at = out.size();
                const RowBinding binding{ table, i };
                FlattenInto(out, templateId, index, chain,
                            Uuid::Derive(pathContext, Uuid(templateId.Value() + i + 1)), depth,
                            slot, rows, table ? &binding : row);
                // Named for its place, so a script can address the third one by saying so.
                if (out.size() > at) out[at].name = base + " " + std::to_string(i + 1);
                for (std::size_t k = at; k < out.size(); ++k) {
                    out[k].repeated = true;
                    out[k].row = static_cast<i32>(i);
                }
                if (out.size() > at) out[at].rowRoot = true;
            }
            for (std::size_t c = 1; c < node->children.size(); ++c)
                FlattenInto(out, node->children[c], index, chain, pathContext, depth, slot, rows,
                            row);
            return;
        }

        for (Uuid child : node->children)
            FlattenInto(out, child, index, chain, pathContext, depth, slot, rows, row);
    }

    std::vector<Document::FlatNode> Document::Flatten(Uuid root, const RowLookup& rows) const {
        std::vector<FlatNode> out;
        std::vector<Uuid> chain;
        FlattenInto(out, root, UINT32_MAX, chain, Uuid::Invalid(), 0, nullptr, &rows);
        return out;
    }

}
