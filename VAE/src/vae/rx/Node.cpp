#include "vaepch.h"
#include "vae/rx/Node.h"

#include <algorithm>

namespace vae::rx {

    Runtime& Rt() { static Runtime rt; return rt; }

    Node::~Node() {
        // Unlink both directions. A dangling observer pointer here is a use-after-free the next
        // time anything writes, and it would surface far from the cause.
        for (auto& s : m_Sources) {
            auto& obs = s.node->m_Observers;
            obs.erase(std::remove(obs.begin(), obs.end(), this), obs.end());
        }
        for (auto* o : m_Observers) {
            auto& srcs = o->m_Sources;
            srcs.erase(std::remove_if(srcs.begin(), srcs.end(),
                                      [this](const Source& s) { return s.node == this; }),
                       srcs.end());
        }
        auto& pending = Rt().pendingEffects;
        pending.erase(std::remove(pending.begin(), pending.end(), this), pending.end());
        if (Rt().listener == this) Rt().listener = nullptr;
    }

    void Node::Observe() {
        Node* listener = Rt().listener;
        if (!listener || listener == this) return;

        for (auto& s : listener->m_Sources)
            if (s.node == this) { s.version = m_Version; return; }

        listener->m_Sources.push_back({ this, m_Version });
        m_Observers.push_back(listener);
    }

    void Node::MarkStale(State s) {
        if (m_State >= s) {
            // Already at least this stale. Still need to reach observers the first time we became
            // stale at all, which the earlier call did — so there is nothing left to do.
            if (!(m_State == State::Check && s == State::Dirty)) return;
        }
        m_State = s;

        if (IsEffect()) {
            if (!m_Queued) { m_Queued = true; Rt().pendingEffects.push_back(this); }
            return;
        }
        for (auto* o : m_Observers) o->MarkStale(State::Check);
    }

    void Node::PropagateChange() {
        for (auto* o : m_Observers) o->MarkStale(State::Dirty);
    }

    void Node::Pull() {
        if (m_State == State::Clean) return;

        if (m_State == State::Check) {
            // "Maybe stale": recurse into sources and see whether any actually changed value.
            for (auto& s : m_Sources) {
                s.node->Pull();
                if (s.node->m_Version != s.version) { m_State = State::Dirty; break; }
            }
        }

        if (m_State == State::Dirty) Update();
        m_State = State::Clean;
    }

    void Node::ClearSources() {
        for (auto& s : m_Sources) {
            auto& obs = s.node->m_Observers;
            obs.erase(std::remove(obs.begin(), obs.end(), this), obs.end());
        }
        m_Sources.clear();
    }

    void Flush() {
        Runtime& rt = Rt();
        if (rt.flushing || rt.batchDepth > 0) return;

        rt.flushing = true;
        // Effects queued *by* an effect are appended and run in the same flush, so a cascade
        // settles before control returns. Index-based because the vector can grow underneath us.
        for (std::size_t i = 0; i < rt.pendingEffects.size(); ++i) {
            Node* e = rt.pendingEffects[i];
            e->m_Queued = false;
            e->Pull();
        }
        rt.pendingEffects.clear();
        rt.flushing = false;
    }

}
