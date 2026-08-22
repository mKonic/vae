#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace vae::ui::widgets {

    namespace {

        u32 Named(const ViewTree& tree, u32 view, std::string_view name) {
            std::vector<u32> stack{ view };
            while (!stack.empty()) {
                const u32 current = stack.back();
                stack.pop_back();
                if (tree.At(current).name == name) return current;
                for (u32 child : tree.At(current).children) stack.push_back(child);
            }
            return ViewTree::kInvalid;
        }

        u32 TextUnder(const ViewTree& tree, u32 view) {
            std::vector<u32> stack{ view };
            while (!stack.empty()) {
                const u32 current = stack.back();
                stack.pop_back();
                if (tree.At(current).kind == doc::NodeKind::Text) return current;
                for (u32 child : tree.At(current).children) stack.push_back(child);
            }
            return ViewTree::kInvalid;
        }

        std::string Lowered(std::string_view text) {
            std::string out(text);
            for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        // ------------------------------------------------------------------------- combobox

        // A select you can type into. The field is an ordinary TextInput child, so the caret, the
        // selection and Ctrl+C are the ones that already work; this behavior only decides when the
        // list is showing and which of its rows still match.
        class ComboboxBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Combobox; }
            bool Focusable() const override { return false; }

            void Sync(WidgetContext& context) override {
                const u32 menu = context.tree.FindRole(context.view, Role::Content);
                if (menu != ViewTree::kInvalid) context.tree.SetRuntimeVisible(menu, false);
                context.SetState(StateBit::Open, context.host.HasOverlay(context.Id()));
                WearChoice(context);
            }

            // Opening follows focus rather than a click: a typeahead whose list appears only after
            // a second click on a chevron is a select with extra steps.
            void Arrange(WidgetContext& context) override {
                const u32 field = context.tree.FindRole(context.view, Role::TextInput);
                if (field == ViewTree::kInvalid) return;
                const bool focused = context.host.Focused() == field;
                const bool open = context.host.HasOverlay(context.Id());

                if (!focused) {
                    m_Suppress = false;
                    if (open && m_Opened) {
                        context.host.CloseOverlay(context.Id());
                        m_Opened = false;
                    }
                    return;
                }
                // Chosen or dismissed while the field still has focus: do not spring back open.
                if (m_Opened && !open) { m_Suppress = true; m_Opened = false; }
                if (!open && !m_Suppress) {
                    const u32 menu = context.tree.FindRole(context.view, Role::Content);
                    if (menu == ViewTree::kInvalid) return;
                    context.host.OpenOverlay(context.Id(), context.tree.At(menu).sourceId, false,
                                             context.Bounds());
                    m_Opened = true;
                    return;
                }
                if (open) Filter(context, field);
            }

        private:
            // The rows that still match what has been typed. The menu lives in its own tree, so the
            // filtering reaches across the overlay boundary the same way a menu item reaches back.
            void Filter(WidgetContext& context, u32 field) const {
                ViewTree* menu = context.host.OverlayTreeOf(context.Id());
                if (!menu) return;
                const std::string needle = Lowered(context.tree.Str(field, doc::Prop::Text));

                u32 shown = 0;
                for (u32 item : menu->FindAllRoles(menu->Root(), Role::DropdownItem)) {
                    const u32 label = TextUnder(*menu, item);
                    const bool matches = needle.empty() || label == ViewTree::kInvalid
                        || Lowered(menu->Str(label, doc::Prop::Text)).find(needle) != std::string::npos;
                    menu->SetRuntimeVisible(item, matches);
                    shown += matches ? 1u : 0u;
                }
                // "No matches" is a row the designer drew, shown only when nothing else is.
                if (const u32 empty = Named(*menu, menu->Root(), "Empty"); empty != ViewTree::kInvalid)
                    menu->SetRuntimeVisible(empty, shown == 0);
            }

            // A combobox does wear its choice — that is the half of it that is a select.
            void WearChoice(WidgetContext& context) {
                const auto index = static_cast<i32>(context.Number(doc::Prop::SelectedIndex, -1.0f));
                if (index < 0 || index == m_Worn) return;
                m_Worn = index;

                const u32 menu = context.tree.FindRole(context.view, Role::Content);
                const u32 field = context.tree.FindRole(context.view, Role::TextInput);
                if (menu == ViewTree::kInvalid || field == ViewTree::kInvalid) return;
                const auto items = context.tree.FindAllRoles(menu, Role::DropdownItem);
                if (static_cast<std::size_t>(index) >= items.size()) return;

                const u32 label = TextUnder(context.tree, items[static_cast<std::size_t>(index)]);
                if (label == ViewTree::kInvalid) return;
                context.tree.SetViewProp(field, doc::Prop::Text,
                                         doc::Value{ context.tree.Str(label, doc::Prop::Text) });
            }

            bool m_Opened = false;
            bool m_Suppress = false;
            i32  m_Worn = -1;
        };

        // ------------------------------------------------------------------------- calendar

        struct Ymd { int year = 1970; unsigned month = 1; unsigned day = 1; };

        Ymd Today() {
            const auto now = std::chrono::floor<std::chrono::days>(
                std::chrono::system_clock::now());
            const std::chrono::year_month_day ymd{ now };
            return { static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                     static_cast<unsigned>(ymd.day()) };
        }

        bool ParseDate(std::string_view text, Ymd& out) {
            int year = 0;
            unsigned month = 0, day = 0;
            if (std::sscanf(std::string(text).c_str(), "%d-%u-%u", &year, &month, &day) != 3)
                return false;
            if (month < 1 || month > 12 || day < 1 || day > 31) return false;
            out = { year, month, day };
            return true;
        }

        std::string FormatDate(const Ymd& date) {
            char buffer[16];
            std::snprintf(buffer, sizeof buffer, "%04d-%02u-%02u", date.year, date.month, date.day);
            return buffer;
        }

        // A month at a time, in a seven-wide grid. The date is text on the node in ISO order,
        // because that is the one spelling that sorts, parses and reads the same everywhere — and
        // a script setting a date should not have to guess a locale.
        class CalendarBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Calendar; }
            bool Focusable() const override { return true; }

            CursorShape CursorOver(const WidgetContext& context) const override {
                if (!context.Enabled()) return CursorShape::NotAllowed;
                const Vec2 at = context.host.MousePosition();
                return (Step(context, at) != 0 || CellAt(context, at) != ViewTree::kInvalid)
                     ? CursorShape::Hand : CursorShape::Arrow;
            }

            void Sync(WidgetContext& context) override { Show(context); }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!context.Enabled()) return false;
                if (IsLeftPress(event))
                    return Step(context, PointOf(event)) != 0
                        || CellAt(context, PointOf(event)) != ViewTree::kInvalid;
                if (!IsLeftRelease(event)) return false;

                if (const i32 step = Step(context, PointOf(event)); step != 0) {
                    Browse(context, step);
                    return true;
                }
                const u32 cell = CellAt(context, PointOf(event));
                if (cell == ViewTree::kInvalid) return false;
                const auto day = static_cast<unsigned>(
                    std::lround(context.tree.Number(cell, doc::Prop::Value, 0.0f)));
                if (day == 0) return true;   // a blank leading or trailing cell

                const Ymd picked{ m_Year, m_Month, day };
                context.Set(doc::Prop::Text, FormatDate(picked));
                Show(context);
                Fire(context, ActionKind::ValueChanged, doc::Value{ FormatDate(picked) });
                return true;
            }

        private:
            static i32 Step(const WidgetContext& context, Vec2 at) {
                const ViewTree& tree = context.tree;
                const u32 prev = Named(tree, context.view, "Prev");
                const u32 next = Named(tree, context.view, "Next");
                if (prev != ViewTree::kInvalid && tree.Bounds(prev).Contains(at)) return -1;
                if (next != ViewTree::kInvalid && tree.Bounds(next).Contains(at)) return 1;
                return 0;
            }

            static u32 CellAt(const WidgetContext& context, Vec2 at) {
                for (u32 cell : context.tree.FindAllRoles(context.view, Role::Tab))
                    if (context.tree.Bounds(cell).Contains(at)) return cell;
                return ViewTree::kInvalid;
            }

            void Browse(WidgetContext& context, i32 step) {
                auto month = static_cast<i32>(m_Month) + step;
                while (month < 1)  { month += 12; --m_Year; }
                while (month > 12) { month -= 12; ++m_Year; }
                m_Month = static_cast<unsigned>(month);
                Show(context);
                Fire(context, ActionKind::Navigated,
                     doc::Value{ FormatDate({ m_Year, m_Month, 1 }) });
            }

            void Show(WidgetContext& context) {
                ViewTree& tree = context.tree;
                Ymd selected{};
                const bool hasSelection = ParseDate(context.Str(doc::Prop::Text), selected);
                if (!m_Browsing) {
                    const Ymd start = hasSelection ? selected : Today();
                    m_Year = start.year;
                    m_Month = start.month;
                    m_Browsing = true;
                }

                using namespace std::chrono;
                const year_month_day first{ year{ m_Year } / month{ m_Month } / 1 };
                const auto lead = static_cast<unsigned>(
                    weekday{ sys_days{ first } }.c_encoding());   // 0 = Sunday
                const auto length = static_cast<unsigned>(
                    (year{ m_Year } / month{ m_Month } / last).day());

                static const char* kMonths[] = { "January", "February", "March", "April", "May",
                                                 "June", "July", "August", "September", "October",
                                                 "November", "December" };
                if (const u32 title = Named(tree, context.view, "Title"); title != ViewTree::kInvalid) {
                    const u32 label = TextUnder(tree, title);
                    if (label != ViewTree::kInvalid)
                        tree.At(label).props.Set(doc::Prop::Text,
                                                 doc::Value{ std::string(kMonths[m_Month - 1]) + " "
                                                             + std::to_string(m_Year) });
                }

                const Ymd today = Today();
                const auto cells = tree.FindAllRoles(context.view, Role::Tab);
                for (u32 i = 0; i < cells.size(); ++i) {
                    const i32 day = static_cast<i32>(i) - static_cast<i32>(lead) + 1;
                    const bool real = day >= 1 && day <= static_cast<i32>(length);
                    tree.SetViewPropLocal(cells[i], doc::Prop::Value,
                                          doc::Value{ real ? static_cast<f32>(day) : 0.0f });
                    // Blank cells stay in the grid rather than collapsing it: a month that starts
                    // on a Wednesday has to start on a Wednesday.
                    const u32 label = TextUnder(tree, cells[i]);
                    if (label != ViewTree::kInvalid)
                        tree.At(label).props.Set(doc::Prop::Text,
                                                 doc::Value{ real ? std::to_string(day)
                                                                  : std::string{} });
                    tree.SetState(cells[i], StateBit::Selected,
                                  real && hasSelection && selected.year == m_Year
                                  && selected.month == m_Month
                                  && selected.day == static_cast<unsigned>(day));
                    tree.SetState(cells[i], StateBit::Checked,
                                  real && today.year == m_Year && today.month == m_Month
                                  && today.day == static_cast<unsigned>(day));
                    tree.SetState(cells[i], StateBit::Disabled, !real);
                }
            }

            int m_Year = 1970;
            unsigned m_Month = 1;
            bool m_Browsing = false;
        };

    }

    Scope<Behavior> MakeCombobox() { return CreateScope<ComboboxBehavior>(); }
    Scope<Behavior> MakeCalendar() { return CreateScope<CalendarBehavior>(); }

}
