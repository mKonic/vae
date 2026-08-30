#include "Convert.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"
#include "vae/core/Application.h"
#include "vae/doc/Serializer.h"
#include "vae/ui/Library.h"

#include <chrono>

namespace vae {

    namespace {

        // Node for node, ignoring nothing except the order a map happens to iterate in. This is the
        // check that matters: a format is only correct if the document that comes back out is the
        // one that went in.
        bool SameDocument(const doc::Document& a, const doc::Document& b, std::string& why) {
            if (a.NodeCount() != b.NodeCount()) {
                why = "node count " + std::to_string(a.NodeCount()) + " -> "
                    + std::to_string(b.NodeCount());
                return false;
            }
            if (a.Tokens() != b.Tokens())   { why = "tokens differ"; return false; }
            if (a.Assets() != b.Assets())   { why = "assets differ"; return false; }
            if (a.ActiveTheme() != b.ActiveTheme()) { why = "theme differs"; return false; }
            if (a.Roots().size() != b.Roots().size()) { why = "root count differs"; return false; }

            // Walked in parallel rather than compared by id, because ids are exactly what the file
            // is allowed to stop writing.
            std::function<bool(Uuid, Uuid)> same = [&](Uuid ia, Uuid ib) {
                const doc::Node* na = a.Find(ia);
                const doc::Node* nb = b.Find(ib);
                if (!na || !nb) { why = "missing node"; return false; }
                if (na->kind != nb->kind)   { why = "kind differs on '" + na->name + "'"; return false; }
                if (na->name != nb->name)   { why = "name differs: '" + na->name + "'"; return false; }
                if (!(na->layout == nb->layout)) { why = "layout differs on '" + na->name + "'"; return false; }
                if (!(na->props == nb->props))   { why = "props differ on '" + na->name + "'"; return false; }
                if (na->visible != nb->visible || na->locked != nb->locked || na->slot != nb->slot) {
                    why = "flags differ on '" + na->name + "'";
                    return false;
                }
                if (na->overrides.size() != nb->overrides.size()) {
                    why = "override count differs on '" + na->name + "'";
                    return false;
                }
                if (na->children.size() != nb->children.size()) {
                    why = "child count differs on '" + na->name + "'";
                    return false;
                }
                for (std::size_t i = 0; i < na->children.size(); ++i)
                    if (!same(na->children[i], nb->children[i])) return false;
                return true;
            };
            for (std::size_t i = 0; i < a.Roots().size(); ++i)
                if (!same(a.Roots()[i], b.Roots()[i])) return false;
            return true;
        }

    }

    void ConvertLayer::OnAttach() {
        Application& app = Application::Get();
        const auto fail = [&](const std::string& message) {
            VAE_ERROR("convert: {}", message);
            app.SetExitCode(1);
            app.Close();
        };

        // Read before writing: converting in place is the normal case, and measuring the input
        // afterwards measures the output.
        const auto before = FileSystem::ReadText(m_In);

        doc::Document in;
        std::string error;
        if (!doc::Serializer::Load(m_In, in, &error, &ui::StandardLibrary()))
            return fail(error);

        if (m_Bench > 0) {
            // The whole load, library rebuild included, because that is what opening a file costs.
            const auto text = FileSystem::ReadText(m_In);
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < m_Bench; ++i) {
                doc::Document scratch;
                std::string ignored;
                doc::Serializer::FromXml(*text, scratch, &ignored, &ui::StandardLibrary());
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            const f64 ms = std::chrono::duration<f64, std::milli>(elapsed).count() / m_Bench;
            VAE_INFO("convert: {} loads of {} bytes, {:.3f} ms each",
                     m_Bench, text ? text->size() : 0u, ms);
            app.Close();
            return;
        }

        const std::string xml = doc::Serializer::ToXml(in, true, &ui::StandardLibrary());

        if (m_Check) {
            doc::Document back;
            if (!doc::Serializer::FromXml(xml, back, &error, &ui::StandardLibrary()))
                return fail("written, but does not read back: " + error);
            std::string why;
            if (!SameDocument(in, back, why))
                return fail("round trip changed the document: " + why);
            VAE_INFO("convert: round trip clean ({} nodes)", in.NodeCount());
        }

        if (!m_Out.empty() && !FileSystem::WriteText(m_Out, xml))
            return fail("cannot write " + m_Out.string());

        const auto lines = [](std::string_view s) {
            return std::count(s.begin(), s.end(), '\n') + (s.empty() || s.back() == '\n' ? 0 : 1);
        };
        VAE_INFO("convert: {} bytes / {} lines -> {} bytes / {} lines",
                 before ? before->size() : 0u, before ? lines(*before) : 0,
                 xml.size(), lines(xml));
        if (m_Out.empty()) std::fwrite(xml.data(), 1, xml.size(), stdout);
        app.Close();
    }

}
