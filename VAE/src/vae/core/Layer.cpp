#include "vaepch.h"
#include "vae/core/Layer.h"

namespace vae {

    LayerStack::~LayerStack() { Clear(); }

    void LayerStack::Push(Scope<Layer> layer) {
        layer->OnAttach();
        m_Layers.emplace(m_Layers.begin() + static_cast<std::ptrdiff_t>(m_InsertIndex), std::move(layer));
        ++m_InsertIndex;
    }

    void LayerStack::PushOverlay(Scope<Layer> overlay) {
        overlay->OnAttach();
        m_Layers.emplace_back(std::move(overlay));
    }

    void LayerStack::Clear() {
        // Detach in reverse push order: an overlay may hold resources a lower layer owns.
        for (auto it = m_Layers.rbegin(); it != m_Layers.rend(); ++it) (*it)->OnDetach();
        m_Layers.clear();
        m_InsertIndex = 0;
    }

}
