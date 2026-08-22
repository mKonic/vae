#include "vaepch.h"
#include "vae/doc/Builder.h"

namespace vae::doc {

    Uuid Builder::Screen(std::string name, Vec2 size) {
        const Uuid id = m_Document->CreateNode(NodeKind::Screen, Uuid::Invalid(), std::move(name));
        if (Node* node = m_Document->Find(id)) {
            node->layout.mode = layout::LayoutMode::Absolute;
            node->layout.width = layout::Size::Px(size.x);
            node->layout.height = layout::Size::Px(size.y);
            m_Document->Touch(id);
        }
        return id;
    }

    Uuid Builder::Detached(NodeKind kind, std::string name) {
        return m_Document->CreateNode(kind, Uuid::Invalid(), std::move(name));
    }

    Uuid Builder::Seal(Uuid root, std::string name) {
        return m_Document->MakeComponent(root, std::move(name));
    }

    Uuid Builder::Child(NodeKind kind, Uuid parent, std::string name) {
        return m_Document->CreateNode(kind, parent, std::move(name));
    }

    Uuid Builder::Text(Uuid parent, std::string name, std::string content) {
        const Uuid id = m_Document->CreateNode(NodeKind::Text, parent, std::move(name));
        m_Document->SetProp(id, Prop::Text, std::move(content));
        return id;
    }

    Uuid Builder::Instance(Uuid component, Uuid parent, std::string name) {
        const Uuid id = m_Document->CreateInstance(component, parent);
        if (Node* node = m_Document->Find(id)) {
            node->name = std::move(name);
            m_Document->Touch(id);
        }
        return id;
    }

    void Builder::Set(Uuid node, Prop prop, Value value) {
        m_Document->SetProp(node, prop, std::move(value));
    }

    void Builder::Set(Uuid node, std::string key, Value value) {
        m_Document->SetProp(node, std::move(key), std::move(value));
    }

    void Builder::Token(Uuid node, Prop prop, std::string token) {
        m_Document->SetProp(node, prop, TokenRef{ std::move(token) });
    }

    void Builder::Override(Uuid instance, Uuid nodeInComponent, Prop prop, Value value) {
        m_Document->SetOverride(instance, nodeInComponent, prop, std::move(value));
    }

    void Builder::Hide(Uuid node) {
        if (Node* found = m_Document->Find(node)) {
            found->visible = false;
            m_Document->Touch(node);
        }
    }

    void Builder::Layout(Uuid node, const layout::LayoutStyle& style) {
        if (Node* found = m_Document->Find(node)) {
            found->layout = style;
            m_Document->Touch(node);
        }
    }

    void Builder::DefineToken(const std::string& name, doc::Token token) {
        m_Document->SetToken(name, std::move(token));
    }

    void Builder::UseTheme(doc::Theme theme) { m_Document->SetTheme(theme); }

}
