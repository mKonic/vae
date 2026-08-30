#pragma once

#include <imgui.h>
#include <imgui_node_editor.h>

// The Unreal-looking node: a coloured band across the top with the node's name in it, a rounded
// body, inputs down the left and outputs down the right.
//
// The upstream library ships exactly this as `BlueprintNodeBuilder` under examples/, and it is not
// used here for one reason: it is written against thedmd's fork of Dear ImGui, which carries a
// stack-layout extension (`BeginHorizontal`, `Spring`) that upstream does not have and VAE does not
// vendor. Everything else in the library compiles against stock ImGui, so this is the one piece
// that had to be written rather than taken — the same drawing, laid out with groups and measured
// rows instead of springs.

namespace vae::blueprint {

    namespace ed = ax::NodeEditor;

    class NodeBuilder {
    public:
        void Begin(ed::NodeId id);
        // The band. Called at most once, straight after Begin.
        void Header(const ImVec4& colour, const char* title);
        // Inputs down the left, then outputs down the right. `outputsWidth` is how wide the right
        // column is: the caller measures it, because only the caller knows what is going in it,
        // and a column that is not measured is a column whose pins do not line up.
        void BeginInputs();
        void NextColumn(float outputsWidth);
        // Pushes the next output row far enough right that its right edge meets the column's.
        void RightAlign(float rowWidth);
        void EndColumns();
        void End();

    private:
        ed::NodeId m_Id = 0;
        bool   m_HasHeader = false;
        ImVec2 m_HeaderMin{};
        ImVec2 m_HeaderMax{};
        ImU32  m_HeaderColour = 0;
        float  m_OutputsWidth = 0.0f;
        bool   m_InColumns = false;
    };

}
