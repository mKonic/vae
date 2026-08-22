#pragma once

#include "vae/base/Base.h"

#include <vector>

namespace vae::rx {

    class Node;

    // Process-wide reactive runtime. Single-threaded by design: the UI graph is owned by the main
    // thread, and worker results enter it by posting a Set back onto that thread.
    struct Runtime {
        Node* listener = nullptr;              // the computation currently recording dependencies
        std::vector<Node*> pendingEffects;
        u32 batchDepth = 0;
        bool flushing = false;
    };

    Runtime& Rt();

    // One vertex of the dependency graph. Signals are sources, Computeds are both, Effects are sinks.
    //
    // Propagation is push-then-pull ("Reactively"'s algorithm): a write pushes Dirty to its direct
    // observers and Check ("maybe stale") transitively, then a read pulls — a Check node asks each
    // source whether it actually changed value, and only recomputes if one did. That is what makes
    // a diamond (a -> b, a -> c, b+c -> d) evaluate d exactly once per write instead of twice, and
    // what stops an unread branch of the graph from computing at all.
    class Node {
    public:
        enum class State : u8 { Clean = 0, Check = 1, Dirty = 2 };

        virtual ~Node();

        // Bring this node's value up to date. Signals are always up to date; Computeds re-run.
        virtual void Update() {}
        virtual bool IsEffect() const { return false; }

        u64 Version() const { return m_Version; }

    protected:
        // Record "the node currently being evaluated depends on me".
        void Observe();
        // A source changed value: push staleness outward.
        void PropagateChange();
        void MarkStale(State s);
        // Pull: make sure this node is current before its value is read.
        void Pull();
        void ClearSources();

        struct Source { Node* node; u64 version; };

        std::vector<Source> m_Sources;
        std::vector<Node*>  m_Observers;
        State m_State   = State::Clean;
        u64   m_Version = 1;
        bool  m_Queued  = false;

        friend struct TrackScope;
        friend void Flush();
    };

    // RAII dependency-tracking scope.
    struct TrackScope {
        explicit TrackScope(Node* n) : m_Prev(Rt().listener) { Rt().listener = n; }
        ~TrackScope() { Rt().listener = m_Prev; }
        Node* m_Prev;
    };

    // Suppresses tracking inside its scope — a read that must not create a dependency.
    struct UntrackScope {
        UntrackScope() : m_Prev(Rt().listener) { Rt().listener = nullptr; }
        ~UntrackScope() { Rt().listener = m_Prev; }
        Node* m_Prev;
    };

    void Flush();

    // Coalesces writes: effects run once at the end of the outermost batch, not per Set.
    template<typename Fn>
    void Batch(Fn&& fn) {
        ++Rt().batchDepth;
        fn();
        if (--Rt().batchDepth == 0) Flush();
    }

    template<typename Fn>
    auto Untracked(Fn&& fn) { UntrackScope guard; return fn(); }

}
