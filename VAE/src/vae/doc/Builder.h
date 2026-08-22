#pragma once

#include "vae/doc/Document.h"

#include <string>

namespace vae::doc {

    // Building a document in code, in the shape the exporter emits.
    //
    // Flat rather than fluent, on purpose: a generated file is read top to bottom by someone looking
    // for one node, and a chain of forty calls is not something you can scan. Every method is one
    // thing the designer did, in the order they did it.
    //
    // This is a public API, not a codegen private. That is the point of exporting at all — the code
    // that comes out has to be code you could have written by hand, or the export is a black box
    // with extra steps.
    class Builder {
    public:
        explicit Builder(Document& document) : m_Document(&document) {}

        Document& Doc() { return *m_Document; }

        // --- structure --------------------------------------------------------------------------
        Uuid Screen(std::string name, Vec2 size);
        // A component's contents are built detached and then sealed. Detached because a component is
        // a definition: it is not on any screen, and anything built on one would render there.
        Uuid Detached(NodeKind kind, std::string name);
        Uuid Seal(Uuid root, std::string name);
        Uuid Child(NodeKind kind, Uuid parent, std::string name);
        Uuid Frame(Uuid parent, std::string name) { return Child(NodeKind::Frame, parent, name); }
        Uuid Text(Uuid parent, std::string name, std::string content);
        Uuid Instance(Uuid component, Uuid parent, std::string name);

        // --- properties -------------------------------------------------------------------------
        void Set(Uuid node, Prop prop, Value value);
        // A property the enum does not name — a state overlay, or something a project invented.
        void Set(Uuid node, std::string key, Value value);
        void Token(Uuid node, Prop prop, std::string token);
        void Override(Uuid instance, Uuid nodeInComponent, Prop prop, Value value);
        void Hide(Uuid node);
        void Layout(Uuid node, const layout::LayoutStyle& style);

        // --- theme ------------------------------------------------------------------------------
        void DefineToken(const std::string& name, doc::Token token);
        void UseTheme(doc::Theme theme);

    private:
        Document* m_Document;
    };

}
