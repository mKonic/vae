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

    Uuid Document::CreateNode(NodeKind kind, Uuid parent, std::string name) {
        Node node;
        node.id = Uuid{};
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

    PropBag Document::ResolvedProps(Uuid instance, Uuid nodeInComponent) const {
        PropBag result;
        if (const Node* source = Find(nodeInComponent)) result = source->props;

        if (const Node* node = Find(instance); node && node->IsInstance()) {
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
                MergeOverride(source, source->componentId, result);
            } else {
                result = source->props;
            }
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

    void Document::FlattenInto(std::vector<FlatNode>& out, Uuid id, u32 parent,
                               std::vector<Uuid>& chain, Uuid pathContext, u32 depth,
                               const SlotContent* slot) const {
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
            out.push_back(std::move(flat));

            const u32 index = static_cast<u32>(out.size() - 1);
            const Uuid slotNode = SlotOf(node->componentId);
            const bool fills = !node->children.empty() && slotNode.Valid();

            // The component is itself the slot, so the instance's children *are* its children —
            // and they stay in the page's scope, which is why the chain is not pushed here.
            if (fills && slotNode == component->id) {
                for (Uuid child : node->children)
                    FlattenInto(out, child, index, chain, pathContext, depth);
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
                FlattenInto(out, child, index, chain, path, depth + 1, fills ? &content : nullptr);
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
        out.push_back(std::move(flat));

        const u32 index = static_cast<u32>(out.size() - 1);

        // The slot: the instance's own children stand in for the component's placeholder ones, in
        // the scope they were written. Nothing supplied and the placeholder stays, which is what
        // makes a freshly dropped Card look like a card instead of an empty rectangle.
        if (node->slot && slot && slot->children && !slot->children->empty()) {
            std::vector<Uuid> outer = slot->chain;
            for (Uuid child : *slot->children)
                FlattenInto(out, child, index, outer, slot->pathContext, depth);
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
        const f32 repeats = out[index].props.Number(Prop::Repeat, 0.0f);
        if (repeats >= 1.0f && !node->children.empty()) {
            const u32 count = std::min(static_cast<u32>(repeats), kMaxRepeat);
            if (static_cast<f32>(count) < repeats)
                VAE_CORE_WARN("'{}' asked to repeat {} times; {} is the limit", node->name,
                              static_cast<u32>(repeats), kMaxRepeat);

            const Uuid templateId = node->children.front();
            const std::string base = Find(templateId) ? Find(templateId)->name : std::string("Item");
            for (u32 i = 0; i < count; ++i) {
                const std::size_t at = out.size();
                FlattenInto(out, templateId, index, chain,
                            Uuid::Derive(pathContext, Uuid(templateId.Value() + i + 1)), depth,
                            slot);
                // Named for its place, so a script can address the third one by saying so.
                if (out.size() > at) out[at].name = base + " " + std::to_string(i + 1);
                for (std::size_t k = at; k < out.size(); ++k) out[k].repeated = true;
            }
            for (std::size_t c = 1; c < node->children.size(); ++c)
                FlattenInto(out, node->children[c], index, chain, pathContext, depth, slot);
            return;
        }

        for (Uuid child : node->children)
            FlattenInto(out, child, index, chain, pathContext, depth, slot);
    }

    std::vector<Document::FlatNode> Document::Flatten(Uuid root) const {
        std::vector<FlatNode> out;
        std::vector<Uuid> chain;
        FlattenInto(out, root, UINT32_MAX, chain, Uuid::Invalid(), 0);
        return out;
    }

}
