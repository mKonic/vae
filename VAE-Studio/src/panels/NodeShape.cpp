#include "NodeShape.h"

#include <imgui_internal.h>

#include <algorithm>

namespace vae::blueprint {

    void NodeBuilder::Begin(ed::NodeId id) {
        m_Id = id;
        m_HasHeader = false;
        m_InColumns = false;
        m_OutputsWidth = 0.0f;
        // The body draws its own band, so the node's own top padding is taken off and put back as
        // the band's height. Without this there is a strip of node colour above the header.
        ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(8, 4, 8, 8));
        ed::BeginNode(id);
        ImGui::PushID(static_cast<int>(id.Get()));
    }

    void NodeBuilder::Header(const ImVec4& colour, const char* title) {
        m_HasHeader = true;
        m_HeaderColour = ImGui::ColorConvertFloat4ToU32(colour);

        ImGui::BeginGroup();
        // A little breathing room on either side of the name, inside the band.
        ImGui::Dummy(ImVec2(2.0f, 0.0f));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted(title);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Dummy(ImVec2(6.0f, ImGui::GetTextLineHeight() + 6.0f));
        ImGui::EndGroup();

        m_HeaderMin = ImGui::GetItemRectMin();
        m_HeaderMax = ImGui::GetItemRectMax();
        // The gap between the band and the first pin row.
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }

    void NodeBuilder::BeginInputs() {
        m_InColumns = true;
        ImGui::BeginGroup();
    }

    void NodeBuilder::NextColumn(float outputsWidth) {
        ImGui::EndGroup();
        m_OutputsWidth = std::max(outputsWidth, 1.0f);
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::BeginGroup();
    }

    void NodeBuilder::RightAlign(float rowWidth) {
        const float slack = m_OutputsWidth - rowWidth;
        if (slack <= 1.0f) return;
        ImGui::Dummy(ImVec2(slack, 0.0f));
        ImGui::SameLine(0.0f, 0.0f);
    }

    void NodeBuilder::EndColumns() {
        if (!m_InColumns) return;
        ImGui::EndGroup();
        m_InColumns = false;
    }

    void NodeBuilder::End() {
        EndColumns();
        ImGui::PopID();
        ed::EndNode();
        ed::PopStyleVar();

        if (!m_HasHeader) return;

        // The node's own rectangle, which is only known once it has been submitted — the band has
        // to span the whole width, and the width is decided by whichever row turned out widest.
        const ImVec2 nodeMin = ImGui::GetItemRectMin();
        const ImVec2 nodeMax = ImGui::GetItemRectMax();
        if (nodeMax.x <= nodeMin.x) return;

        const float rounding = ed::GetStyle().NodeRounding;
        const float bottom = m_HeaderMax.y + 2.0f;
        ImDrawList* draw = ed::GetNodeBackgroundDrawList(m_Id);
        if (!draw) return;

        // Only the top corners are rounded: the band's bottom edge is a straight line across the
        // node, which is what separates the name from what the node does.
        draw->AddRectFilled(ImVec2(nodeMin.x + 1.0f, nodeMin.y + 1.0f),
                            ImVec2(nodeMax.x - 1.0f, bottom), m_HeaderColour, rounding,
                            ImDrawFlags_RoundCornersTop);
        draw->AddLine(ImVec2(nodeMin.x + 1.0f, bottom), ImVec2(nodeMax.x - 1.0f, bottom),
                      IM_COL32(0, 0, 0, 90), 1.0f);
    }

}
