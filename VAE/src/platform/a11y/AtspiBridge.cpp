#include "vaepch.h"
#include "vae/a11y/Bridge.h"
#include "vae/a11y/TextBoundary.h"

#ifdef VAE_A11Y_ATSPI

#include "vae/base/Utf8.h"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

// AT-SPI, over D-Bus.
//
// The protocol is: find the accessibility bus (the session bus knows where it is), connect to it,
// hand the registry a reference to our root object, and then answer questions about a tree of
// objects — one D-Bus object per accessible node. A screen reader is a client walking that tree.
//
// The role and state numbers below are AT-SPI's own constants (atspi-constants.h). They are
// written out rather than pulled in from at-spi2-core's headers because the header is the only
// thing we would want from that package, and requiring the whole of it to build an engine that
// might never talk to a screen reader is not a trade worth making.
namespace vae::a11y {

    namespace {

        // AtspiRole. Only the ones this maps to.
        enum : u32 {
            kRoleInvalid = 0, kRoleAlert = 2, kRoleCanvas = 6, kRoleCheckBox = 7,
            kRoleColumnHeader = 10, kRoleComboBox = 11, kRoleDateEditor = 12, kRoleDialog = 16,
            kRoleFiller = 20, kRoleImage = 27, kRoleLabel = 29, kRoleList = 31, kRoleListItem = 32,
            kRoleMenu = 33, kRoleMenuItem = 35, kRolePageTab = 37, kRolePageTabList = 38,
            kRolePanel = 39, kRolePasswordText = 40, kRoleProgressBar = 42, kRolePushButton = 43,
            kRoleRadioButton = 44, kRoleScrollPane = 49, kRoleSeparator = 50, kRoleSlider = 51,
            kRoleTable = 55, kRoleToggleButton = 62, kRoleToolTip = 64, kRoleWindow = 69,
            kRoleApplication = 75, kRoleEntry = 79, kRoleLink = 88, kRoleNotification = 96,
        };

        // AtspiStateType, as bit positions in the 64-bit state set.
        enum : u32 {
            kStateChecked = 4, kStateEditable = 7, kStateEnabled = 8, kStateExpandable = 9,
            kStateExpanded = 10, kStateFocusable = 11, kStateFocused = 12, kStateHorizontal = 14,
            kStatePressed = 20, kStateSelectable = 22, kStateSelected = 23, kStateSensitive = 24,
            kStateShowing = 25, kStateVertical = 29, kStateVisible = 30,
        };

        u32 AtspiRole(Role role) {
            switch (role) {
                case Role::Application:  return kRoleApplication;
                case Role::Window:       return kRoleWindow;
                case Role::Dialog:       return kRoleDialog;
                case Role::Alert:        return kRoleAlert;
                case Role::Panel:        return kRolePanel;
                case Role::Filler:       return kRoleFiller;
                case Role::PushButton:   return kRolePushButton;
                case Role::ToggleButton: return kRoleToggleButton;
                case Role::CheckBox:     return kRoleCheckBox;
                case Role::RadioButton:  return kRoleRadioButton;
                case Role::Slider:       return kRoleSlider;
                case Role::ComboBox:     return kRoleComboBox;
                case Role::Entry:        return kRoleEntry;
                case Role::PasswordText: return kRolePasswordText;
                case Role::PageTabList:  return kRolePageTabList;
                case Role::PageTab:      return kRolePageTab;
                case Role::ScrollPane:   return kRoleScrollPane;
                case Role::List:         return kRoleList;
                case Role::ListItem:     return kRoleListItem;
                case Role::Table:        return kRoleTable;
                case Role::ColumnHeader: return kRoleColumnHeader;
                case Role::Menu:         return kRoleMenu;
                case Role::MenuItem:     return kRoleMenuItem;
                case Role::ToolTip:      return kRoleToolTip;
                case Role::Notification: return kRoleNotification;
                case Role::ProgressBar:  return kRoleProgressBar;
                case Role::Separator:    return kRoleSeparator;
                case Role::Label:        return kRoleLabel;
                case Role::Image:        return kRoleImage;
                case Role::Link:         return kRoleLink;
                case Role::Canvas:       return kRoleCanvas;
                case Role::DateEditor:   return kRoleDateEditor;
                default:                 return kRoleInvalid;
            }
        }

        u64 AtspiStates(StateSet set) {
            u64 out = 0;
            const auto bit = [&](State from, u32 to) { if (Has(set, from)) out |= 1ull << to; };
            bit(State::Enabled,    kStateEnabled);
            bit(State::Sensitive,  kStateSensitive);
            bit(State::Visible,    kStateVisible);
            bit(State::Showing,    kStateShowing);
            bit(State::Focusable,  kStateFocusable);
            bit(State::Focused,    kStateFocused);
            bit(State::Checked,    kStateChecked);
            bit(State::Selected,   kStateSelected);
            bit(State::Selectable, kStateSelectable);
            bit(State::Editable,   kStateEditable);
            bit(State::Expanded,   kStateExpanded);
            bit(State::Expandable, kStateExpandable);
            bit(State::Pressed,    kStatePressed);
            bit(State::Horizontal, kStateHorizontal);
            bit(State::Vertical,   kStateVertical);
            return out;
        }

        constexpr const char* kRootPath = "/org/a11y/atspi/accessible/root";
        constexpr const char* kPathPrefix = "/org/a11y/atspi/accessible";
        constexpr const char* kNullPath = "/org/a11y/atspi/null";

        // What the bus answers questions from. A snapshot, not the live tree: bus callbacks run
        // from Pump() and the tree is rebuilt by the frame, and answering out of the live one
        // would mean a screen reader could see it half-rebuilt.
        struct Snapshot {
            // One character's box on screen, in the same space as the node's own.
            struct Box { i32 x = 0, y = 0, w = 0, h = 0; };

            struct Item {
                u32 parent = Node::kInvalid;
                std::vector<u32> children;
                u32 indexInParent = 0;
                u32 role = kRolePanel;
                std::string roleName;
                std::string name, description;
                u64 state = 0;
                i32 x = 0, y = 0, w = 0, h = 0;
                bool hasValue = false;
                double value = 0.0, minimum = 0.0, maximum = 0.0, step = 0.0;
                // What may be done to it, and what it holds. `text` is already masked for a
                // password field by the time it gets here — see Accessibility.cpp.
                ActionSet actions = 0;
                bool hasText = false;
                // Whether a screen reader may type into it, which is not the same as whether it
                // has text: a label and a read-only field both have text and neither takes any.
                bool editable = false;
                // Whether choosing among its children is a thing that can be done to it — a list,
                // a set of tabs, a menu. Held on the container, because that is where AT-SPI's
                // Selection interface lives.
                bool selects = false;
                std::string text;
                std::vector<Box> characters;
                u32 caret = 0, selectionStart = 0, selectionEnd = 0;
                // Document identity, for telling a node that changed from one that moved.
                u64 key = 0;
            };
            std::vector<Item> items;
            std::string application;
        };

        std::string PathFor(u32 index) {
            if (index == 0) return kRootPath;
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "%s/%u", kPathPrefix, index);
            return buffer;
        }

    }

    class AtspiBridge final : public Bridge {
    public:
        ~AtspiBridge() override { Shutdown(); }

        bool Connect(std::string_view applicationName) override;
        bool Connected() const override { return m_Bus != nullptr && m_Embedded; }
        void Shutdown() override;
        void Publish(const Tree& tree) override;
        void Pump() override;
        const std::string& Status() const override { return m_Status; }

        void SetActor(Actor* actor) override { m_Actor = actor; }

        // Reached from the bus callbacks, which are free functions because sd-bus is C.
        const Snapshot& Data() const { return m_Snapshot; }
        u32 IndexForPath(const char* path) const;
        const char* UniqueName() const { return m_UniqueName.c_str(); }

        // The three things a screen reader can ask to have *done*. All of them run here, on the
        // app's thread, because Pump() is called from the frame — so reaching into the live widget
        // is safe in a way that answering a question out of the live tree would not be.
        bool Perform(u32 node, Action action);
        bool SetCaret(u32 node, u32 start, u32 end);
        bool GrabFocus(u32 node);
        bool Edit(u32 node, u32 start, u32 end, std::string_view insert);
        bool Clip(u32 node, u32 start, u32 end, bool cut);
        bool Paste(u32 node, u32 at);
        bool Choose(u32 node, u32 child, bool selected);

    private:
        bool Announce();
        sd_bus_message* BeginEvent(u32 node, const char* signal, const char* detail,
                                   i32 first, i32 second);
        void SendEvent(sd_bus_message* message);
        void EmitState(u32 node, const char* detail, bool on);
        void EmitNameChanged(u32 node, const std::string& name);
        void EmitValueChanged(u32 node, double value);
        void EmitCaretMoved(u32 node, u32 offset);
        void EmitTextChanged(u32 node, bool inserted, u32 offset, u32 count,
                             const std::string& text);
        void EmitChildrenChanged(u32 parent, bool added, u32 index, u32 child);
        void Diff(const Snapshot& before, const Snapshot& after);

        sd_bus* m_Bus = nullptr;
        std::vector<sd_bus_slot*> m_Slots;
        std::string m_UniqueName;
        std::string m_Status = "not started";
        std::string m_Application = "VAE";
        Snapshot m_Snapshot;
        bool m_Embedded = false;
        u32 m_Focused = Node::kInvalid;
        Actor* m_Actor = nullptr;
    };

    namespace {

        AtspiBridge* Self(void* userdata) { return static_cast<AtspiBridge*>(userdata); }

        const Snapshot::Item* ItemFor(void* userdata, const char* path) {
            AtspiBridge* bridge = Self(userdata);
            const u32 index = bridge->IndexForPath(path);
            const Snapshot& snapshot = bridge->Data();
            return index < snapshot.items.size() ? &snapshot.items[index] : nullptr;
        }

        // sd-bus asks this whether an object exists at a path before it dispatches to the vtable.
        int FindAccessible(sd_bus*, const char* path, const char*, void* userdata, void** found,
                           sd_bus_error*) {
            if (!ItemFor(userdata, path)) return 0;
            *found = userdata;
            return 1;
        }

        int FindApplication(sd_bus*, const char* path, const char*, void* userdata, void** found,
                            sd_bus_error*) {
            // Only the root is the application. Every other node has the Accessible interface and
            // not this one, which is how a client tells them apart.
            if (std::strcmp(path, kRootPath) != 0) return 0;
            *found = userdata;
            return 1;
        }

        int FindValue(sd_bus*, const char* path, const char*, void* userdata, void** found,
                      sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, path);
            if (!item || !item->hasValue) return 0;
            *found = userdata;
            return 1;
        }

        // An interface a node does not have must not be *found* on it either: a client asks what
        // is there by introspecting, and an Action interface answering "0 actions" on every label
        // is a tree full of things that look operable and are not.
        int FindAction(sd_bus*, const char* path, const char*, void* userdata, void** found,
                       sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, path);
            if (!item || item->actions == 0) return 0;
            *found = userdata;
            return 1;
        }

        int FindText(sd_bus*, const char* path, const char*, void* userdata, void** found,
                     sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, path);
            if (!item || !item->hasText) return 0;
            *found = userdata;
            return 1;
        }

        // A read-only field has text and takes none, and a client that finds EditableText on one
        // has been told it can type there. Being absent is the only way to say it cannot.
        int FindEditableText(sd_bus*, const char* path, const char*, void* userdata, void** found,
                             sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, path);
            if (!item || !item->hasText || !item->editable) return 0;
            *found = userdata;
            return 1;
        }

        int FindSelection(sd_bus*, const char* path, const char*, void* userdata, void** found,
                          sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, path);
            if (!item || !item->selects) return 0;
            *found = userdata;
            return 1;
        }

        // A reference to another accessible is (bus name, object path) everywhere in this protocol.
        int AppendRef(sd_bus_message* reply, const char* name, const std::string& path) {
            return sd_bus_message_append(reply, "(so)", name, path.c_str());
        }

        int PropertyName(sd_bus*, const char* path, const char*, const char* property,
                         sd_bus_message* reply, void* userdata, sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, path);
            const char* text = !item ? ""
                             : (std::strcmp(property, "Name") == 0 ? item->name.c_str()
                                                                   : item->description.c_str());
            return sd_bus_message_append(reply, "s", text);
        }

        int PropertyParent(sd_bus*, const char* path, const char*, const char*,
                           sd_bus_message* reply, void* userdata, sd_bus_error*) {
            AtspiBridge* bridge = Self(userdata);
            const Snapshot::Item* item = ItemFor(userdata, path);
            if (!item) return AppendRef(reply, "", kNullPath);
            // The root's parent is the desktop, which lives on the registry rather than here.
            if (item->parent == Node::kInvalid)
                return sd_bus_message_append(reply, "(so)", "org.a11y.atspi.Registry", kRootPath);
            return AppendRef(reply, bridge->UniqueName(), PathFor(item->parent));
        }

        int PropertyChildCount(sd_bus*, const char* path, const char*, const char*,
                               sd_bus_message* reply, void* userdata, sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, path);
            const auto count = static_cast<i32>(item ? item->children.size() : 0);
            return sd_bus_message_append(reply, "i", count);
        }

        int PropertyLocale(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
                           void*, sd_bus_error*) {
            return sd_bus_message_append(reply, "s", "C");
        }

        int PropertyAccessibleId(sd_bus*, const char* path, const char*, const char*,
                                 sd_bus_message* reply, void*, sd_bus_error*) {
            return sd_bus_message_append(reply, "s", path);
        }

        int MethodGetChildAtIndex(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 wanted = 0;
            if (int r = sd_bus_message_read(message, "i", &wanted); r < 0) return r;
            AtspiBridge* bridge = Self(userdata);
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));

            sd_bus_message* reply = nullptr;
            if (int r = sd_bus_message_new_method_return(message, &reply); r < 0) return r;
            if (!item || wanted < 0 || static_cast<std::size_t>(wanted) >= item->children.size())
                AppendRef(reply, "", kNullPath);
            else
                AppendRef(reply, bridge->UniqueName(), PathFor(item->children[wanted]));
            const int r = sd_bus_send(nullptr, reply, nullptr);
            sd_bus_message_unref(reply);
            return r;
        }

        int MethodGetChildren(sd_bus_message* message, void* userdata, sd_bus_error*) {
            AtspiBridge* bridge = Self(userdata);
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));

            sd_bus_message* reply = nullptr;
            if (int r = sd_bus_message_new_method_return(message, &reply); r < 0) return r;
            sd_bus_message_open_container(reply, 'a', "(so)");
            if (item)
                for (u32 child : item->children)
                    AppendRef(reply, bridge->UniqueName(), PathFor(child));
            sd_bus_message_close_container(reply);
            const int r = sd_bus_send(nullptr, reply, nullptr);
            sd_bus_message_unref(reply);
            return r;
        }

        int MethodGetIndexInParent(sd_bus_message* message, void* userdata, sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            return sd_bus_reply_method_return(message, "i",
                                              item ? static_cast<i32>(item->indexInParent) : -1);
        }

        int MethodGetRole(sd_bus_message* message, void* userdata, sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            return sd_bus_reply_method_return(message, "u", item ? item->role : kRoleInvalid);
        }

        int MethodGetRoleName(sd_bus_message* message, void* userdata, sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            return sd_bus_reply_method_return(message, "s", item ? item->roleName.c_str() : "invalid");
        }

        int MethodGetState(sd_bus_message* message, void* userdata, sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            const u64 state = item ? item->state : 0;
            // The state set is 64 bits and travels as two 32-bit words, low first.
            const u32 words[2] = { static_cast<u32>(state & 0xFFFFFFFFu),
                                   static_cast<u32>(state >> 32) };
            sd_bus_message* reply = nullptr;
            if (int r = sd_bus_message_new_method_return(message, &reply); r < 0) return r;
            sd_bus_message_append_array(reply, 'u', words, sizeof words);
            const int r = sd_bus_send(nullptr, reply, nullptr);
            sd_bus_message_unref(reply);
            return r;
        }

        int MethodGetApplication(sd_bus_message* message, void* userdata, sd_bus_error*) {
            AtspiBridge* bridge = Self(userdata);
            sd_bus_message* reply = nullptr;
            if (int r = sd_bus_message_new_method_return(message, &reply); r < 0) return r;
            AppendRef(reply, bridge->UniqueName(), kRootPath);
            const int r = sd_bus_send(nullptr, reply, nullptr);
            sd_bus_message_unref(reply);
            return r;
        }

        int MethodGetAttributes(sd_bus_message* message, void*, sd_bus_error*) {
            sd_bus_message* reply = nullptr;
            if (int r = sd_bus_message_new_method_return(message, &reply); r < 0) return r;
            sd_bus_message_open_container(reply, 'a', "{ss}");
            sd_bus_message_close_container(reply);
            const int r = sd_bus_send(nullptr, reply, nullptr);
            sd_bus_message_unref(reply);
            return r;
        }

        int MethodGetRelationSet(sd_bus_message* message, void*, sd_bus_error*) {
            sd_bus_message* reply = nullptr;
            if (int r = sd_bus_message_new_method_return(message, &reply); r < 0) return r;
            sd_bus_message_open_container(reply, 'a', "(ua(so))");
            sd_bus_message_close_container(reply);
            const int r = sd_bus_send(nullptr, reply, nullptr);
            sd_bus_message_unref(reply);
            return r;
        }

        int MethodGetInterfaces(sd_bus_message* message, void* userdata, sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            sd_bus_message* reply = nullptr;
            if (int r = sd_bus_message_new_method_return(message, &reply); r < 0) return r;
            sd_bus_message_open_container(reply, 'a', "s");
            sd_bus_message_append(reply, "s", "org.a11y.atspi.Accessible");
            sd_bus_message_append(reply, "s", "org.a11y.atspi.Component");
            if (item && item->hasValue) sd_bus_message_append(reply, "s", "org.a11y.atspi.Value");
            if (item && item->actions)  sd_bus_message_append(reply, "s", "org.a11y.atspi.Action");
            if (item && item->hasText)  sd_bus_message_append(reply, "s", "org.a11y.atspi.Text");
            if (item && item->hasText && item->editable)
                sd_bus_message_append(reply, "s", "org.a11y.atspi.EditableText");
            if (item && item->selects)  sd_bus_message_append(reply, "s", "org.a11y.atspi.Selection");
            if (std::strcmp(sd_bus_message_get_path(message), kRootPath) == 0)
                sd_bus_message_append(reply, "s", "org.a11y.atspi.Application");
            sd_bus_message_close_container(reply);
            const int r = sd_bus_send(nullptr, reply, nullptr);
            sd_bus_message_unref(reply);
            return r;
        }

        int MethodGetExtents(sd_bus_message* message, void* userdata, sd_bus_error*) {
            u32 coordinateType = 0;
            if (int r = sd_bus_message_read(message, "u", &coordinateType); r < 0) return r;
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            // Window-relative and screen-relative are the same here: an app that does not know
            // where its window is on screen would have to ask the compositor, and Wayland does
            // not answer that question at all.
            return sd_bus_reply_method_return(message, "(iiii)",
                                              item ? item->x : 0, item ? item->y : 0,
                                              item ? item->w : 0, item ? item->h : 0);
        }

        int MethodContains(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 x = 0, y = 0;
            u32 type = 0;
            if (int r = sd_bus_message_read(message, "iiu", &x, &y, &type); r < 0) return r;
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            const bool inside = item && x >= item->x && y >= item->y
                             && x < item->x + item->w && y < item->y + item->h;
            return sd_bus_reply_method_return(message, "b", inside ? 1 : 0);
        }

        int PropertyDouble(sd_bus*, const char* path, const char*, const char* property,
                           sd_bus_message* reply, void* userdata, sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, path);
            double out = 0.0;
            if (item) {
                if (std::strcmp(property, "CurrentValue") == 0)      out = item->value;
                else if (std::strcmp(property, "MinimumValue") == 0) out = item->minimum;
                else if (std::strcmp(property, "MaximumValue") == 0) out = item->maximum;
                else                                                 out = item->step;
            }
            return sd_bus_message_append(reply, "d", out);
        }

        int PropertyToolkit(sd_bus*, const char*, const char*, const char* property,
                            sd_bus_message* reply, void* userdata, sd_bus_error*) {
            AtspiBridge* bridge = Self(userdata);
            if (std::strcmp(property, "ToolkitName") == 0)
                return sd_bus_message_append(reply, "s", "VAE");
            if (std::strcmp(property, "Version") == 0)
                return sd_bus_message_append(reply, "s", "1.0");
            if (std::strcmp(property, "AtspiVersion") == 0)
                return sd_bus_message_append(reply, "s", "2.1");
            return sd_bus_message_append(reply, "s", bridge->Data().application.c_str());
        }

        int PropertyApplicationId(sd_bus*, const char*, const char*, const char*,
                                  sd_bus_message* reply, void*, sd_bus_error*) {
            return sd_bus_message_append(reply, "i", 0);
        }

        int SetApplicationId(sd_bus*, const char*, const char*, const char*, sd_bus_message*,
                             void*, sd_bus_error*) {
            return 0;    // the registry assigns one and does not care what we think of it
        }

        // --- text, counted in characters ---------------------------------------------------
        //
        // AT-SPI counts characters and C++ counts bytes, and in a field holding anything but ASCII
        // those are different numbers. `a11y::Characters` converts once per call, and the word,
        // line and sentence boundaries a reader walks by live beside it in the core library where
        // the headless suite can check them — the answers are what a screen reader reads out, and
        // they are far too easy to get subtly wrong to be verifiable only by listening.
        int ReplyTextRange(sd_bus_message* message, const Snapshot::Item* item, Range range) {
            const std::string text = item ? std::string(Characters(item->text)
                                                            .Slice(range.start, range.end))
                                          : std::string{};
            return sd_bus_reply_method_return(message, "sii", text.c_str(), range.start, range.end);
        }

        int MethodGetText(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 start = 0, end = 0;
            if (int r = sd_bus_message_read(message, "ii", &start, &end); r < 0) return r;
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            if (!item) return sd_bus_reply_method_return(message, "s", "");
            const Characters chars(item->text);
            // -1 means "to the end", which is how every client asks for the whole field.
            if (end < 0) end = chars.Count();
            const std::string text(chars.Slice(start, end));
            return sd_bus_reply_method_return(message, "s", text.c_str());
        }

        int MethodGetStringAtOffset(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 offset = 0;
            u32 granularity = 0;
            if (int r = sd_bus_message_read(message, "iu", &offset, &granularity); r < 0) return r;
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            if (!item) return sd_bus_reply_method_return(message, "sii", "", 0, 0);
            const Characters chars(item->text);
            return ReplyTextRange(message, item,
                                  RangeFor(chars, offset, static_cast<a11y::Granularity>(granularity)));
        }

        // The three that share a shape: the range at an offset, the one before it, the one after.
        template <int kDirection>
        int MethodGetTextRelativeToOffset(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 offset = 0;
            u32 boundary = 0;
            if (int r = sd_bus_message_read(message, "iu", &offset, &boundary); r < 0) return r;
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            if (!item) return sd_bus_reply_method_return(message, "sii", "", 0, 0);

            const Characters chars(item->text);
            const auto kind = static_cast<a11y::Boundary>(boundary);
            Range range = RangeFor(chars, offset, kind);
            if constexpr (kDirection < 0) {
                if (range.start <= 0) return sd_bus_reply_method_return(message, "sii", "", 0, 0);
                range = RangeFor(chars, range.start - 1, kind);
            } else if constexpr (kDirection > 0) {
                if (range.end >= chars.Count()) {
                    const i32 at = chars.Count();
                    return sd_bus_reply_method_return(message, "sii", "", at, at);
                }
                range = RangeFor(chars, range.end, kind);
            }
            return ReplyTextRange(message, item, range);
        }

        int MethodGetCharacterAtOffset(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 offset = 0;
            if (int r = sd_bus_message_read(message, "i", &offset); r < 0) return r;
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            i32 code = 0;
            if (item) code = static_cast<i32>(Characters(item->text).CodeAt(offset));
            return sd_bus_reply_method_return(message, "i", code);
        }

        int MethodGetCaretOffset(sd_bus_message* message, void* userdata, sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            return sd_bus_reply_method_return(message, "i", item ? static_cast<i32>(item->caret) : 0);
        }

        int MethodSetCaretOffset(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 offset = 0;
            if (int r = sd_bus_message_read(message, "i", &offset); r < 0) return r;
            AtspiBridge* bridge = Self(userdata);
            const u32 node = bridge->IndexForPath(sd_bus_message_get_path(message));
            const auto at = static_cast<u32>(std::max(offset, 0));
            return sd_bus_reply_method_return(message, "b", bridge->SetCaret(node, at, at) ? 1 : 0);
        }

        int MethodGetNSelections(sd_bus_message* message, void* userdata, sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            const bool selected = item && item->selectionEnd > item->selectionStart;
            return sd_bus_reply_method_return(message, "i", selected ? 1 : 0);
        }

        int MethodGetSelection(sd_bus_message* message, void* userdata, sd_bus_error* error) {
            i32 which = 0;
            if (int r = sd_bus_message_read(message, "i", &which); r < 0) return r;
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            if (!item || which != 0 || item->selectionEnd <= item->selectionStart) {
                sd_bus_error_set(error, "org.freedesktop.DBus.Error.InvalidArgs",
                                 "no such selection");
                return -EINVAL;
            }
            return sd_bus_reply_method_return(message, "ii", static_cast<i32>(item->selectionStart),
                                              static_cast<i32>(item->selectionEnd));
        }

        // One selection, because a text field has one. Adding, setting and removing are all the
        // same operation on it, which is why they share an implementation rather than pretending
        // to keep a list nothing here can hold.
        int MethodSetSelectionRange(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 first = 0, second = 0;
            const char* signature = sd_bus_message_get_signature(message, 1);
            if (signature && std::strcmp(signature, "iii") == 0) {
                i32 which = 0;
                if (int r = sd_bus_message_read(message, "iii", &which, &first, &second); r < 0)
                    return r;
            } else if (int r = sd_bus_message_read(message, "ii", &first, &second); r < 0) {
                return r;
            }
            AtspiBridge* bridge = Self(userdata);
            const u32 node = bridge->IndexForPath(sd_bus_message_get_path(message));
            const bool done = bridge->SetCaret(node, static_cast<u32>(std::max(first, 0)),
                                                     static_cast<u32>(std::max(second, 0)));
            return sd_bus_reply_method_return(message, "b", done ? 1 : 0);
        }

        int MethodRemoveSelection(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 which = 0;
            if (int r = sd_bus_message_read(message, "i", &which); r < 0) return r;
            AtspiBridge* bridge = Self(userdata);
            const u32 node = bridge->IndexForPath(sd_bus_message_get_path(message));
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            // Removing the selection leaves the caret where the selection ended, which is what
            // every text field does when you press an arrow key with something selected.
            const u32 at = item ? item->selectionEnd : 0;
            return sd_bus_reply_method_return(message, "b",
                                              bridge->SetCaret(node, at, at) ? 1 : 0);
        }

        int MethodGetTextAttributes(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 offset = 0;
            if (int r = sd_bus_message_read(message, "i", &offset); r < 0) return r;
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            const i32 count = item ? Characters(item->text).Count() : 0;

            // No attribute runs: a VAE text field is one style all the way through, so the honest
            // answer is an empty set spanning the whole of it rather than an invented one.
            sd_bus_message* reply = nullptr;
            if (int r = sd_bus_message_new_method_return(message, &reply); r < 0) return r;
            sd_bus_message_open_container(reply, 'a', "{ss}");
            sd_bus_message_close_container(reply);
            sd_bus_message_append(reply, "ii", 0, count);
            const int r = sd_bus_send(nullptr, reply, nullptr);
            sd_bus_message_unref(reply);
            return r;
        }

        int MethodGetDefaultAttributes(sd_bus_message* message, void*, sd_bus_error*) {
            sd_bus_message* reply = nullptr;
            if (int r = sd_bus_message_new_method_return(message, &reply); r < 0) return r;
            sd_bus_message_open_container(reply, 'a', "{ss}");
            sd_bus_message_close_container(reply);
            const int r = sd_bus_send(nullptr, reply, nullptr);
            sd_bus_message_unref(reply);
            return r;
        }

        int MethodGetAttributeValue(sd_bus_message* message, void*, sd_bus_error*) {
            return sd_bus_reply_method_return(message, "s", "");
        }

        // Where a character is on screen — from the run that was actually shaped, when the tree
        // published one. A screen reader draws a box around what it is reading and a magnifier
        // follows it, and proportional type puts the evenly-divided guess a whole character out
        // within a few words. The even split is still here as the fallback, for a node whose text
        // was never laid out: a slightly wrong box beats no box at all.
        struct Extent { i32 x = 0, y = 0, w = 0, h = 0; };

        Extent CharacterExtent(const Snapshot::Item& item, i32 offset) {
            if (!item.characters.empty()) {
                // One past the end is where a caret sits after the last character, and clients ask
                // for it: answer with a zero-width box at the right edge of the last one.
                const auto count = static_cast<i32>(item.characters.size());
                if (offset >= count) {
                    const Snapshot::Box& last = item.characters[static_cast<std::size_t>(count - 1)];
                    return { last.x + last.w, last.y, 0, last.h };
                }
                const Snapshot::Box& box = item.characters[
                    static_cast<std::size_t>(std::max(offset, 0))];
                return { box.x, box.y, box.w, box.h };
            }
            const i32 count = std::max(Characters(item.text).Count(), 1);
            const i32 width = std::max(item.w / count, 1);
            return { item.x + std::clamp(offset, 0, count) * width, item.y, width, item.h };
        }

        int MethodGetCharacterExtents(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 offset = 0;
            u32 type = 0;
            if (int r = sd_bus_message_read(message, "iu", &offset, &type); r < 0) return r;
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            if (!item) return sd_bus_reply_method_return(message, "iiii", 0, 0, 0, 0);
            const Extent extent = CharacterExtent(*item, offset);
            return sd_bus_reply_method_return(message, "iiii", extent.x, extent.y,
                                              extent.w, extent.h);
        }

        int MethodGetRangeExtents(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 start = 0, end = 0;
            u32 type = 0;
            if (int r = sd_bus_message_read(message, "iiu", &start, &end, &type); r < 0) return r;
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            if (!item) return sd_bus_reply_method_return(message, "iiii", 0, 0, 0, 0);
            const i32 count = Characters(item->text).Count();
            start = std::clamp(start, 0, count);
            end   = std::clamp(end, start, count);
            if (end == start) {
                const Extent at = CharacterExtent(*item, start);
                return sd_bus_reply_method_return(message, "iiii", at.x, at.y, 0, at.h);
            }
            // The union of the characters in it, not the first box stretched to the last: a range
            // that spans a wrapped line covers two rows, and its extent is the block around both.
            Extent box = CharacterExtent(*item, start);
            i32 right = box.x + box.w, bottom = box.y + box.h;
            for (i32 i = start + 1; i < end; ++i) {
                const Extent extent = CharacterExtent(*item, i);
                box.x  = std::min(box.x, extent.x);
                box.y  = std::min(box.y, extent.y);
                right  = std::max(right, extent.x + extent.w);
                bottom = std::max(bottom, extent.y + extent.h);
            }
            return sd_bus_reply_method_return(message, "iiii", box.x, box.y,
                                              right - box.x, bottom - box.y);
        }

        int MethodGetOffsetAtPoint(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 x = 0, y = 0;
            u32 type = 0;
            if (int r = sd_bus_message_read(message, "iiu", &x, &y, &type); r < 0) return r;
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            if (!item || item->w <= 0) return sd_bus_reply_method_return(message, "i", -1);
            if (x < item->x || y < item->y || x >= item->x + item->w || y >= item->y + item->h)
                return sd_bus_reply_method_return(message, "i", -1);
            if (!item->characters.empty()) {
                // The character the point is inside, and failing that the nearest one on the
                // closest line — a point in the padding to the right of a short line means the end
                // of that line, not "nowhere".
                i32 best = -1;
                i64 nearest = INT64_MAX;
                for (std::size_t i = 0; i < item->characters.size(); ++i) {
                    const Snapshot::Box& box = item->characters[i];
                    if (x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h)
                        return sd_bus_reply_method_return(message, "i", static_cast<i32>(i));
                    const i64 dx = x < box.x ? box.x - x : (x >= box.x + box.w ? x - box.x - box.w : 0);
                    const i64 dy = y < box.y ? box.y - y : (y >= box.y + box.h ? y - box.y - box.h : 0);
                    const i64 distance = dy * 1000 + dx;    // the line first, then along it
                    if (distance < nearest) { nearest = distance; best = static_cast<i32>(i); }
                }
                return sd_bus_reply_method_return(message, "i", best);
            }
            const i32 count = std::max(Characters(item->text).Count(), 1);
            const i32 width = std::max(item->w / count, 1);
            return sd_bus_reply_method_return(message, "i", std::min((x - item->x) / width, count));
        }

        int MethodGetBoundedRanges(sd_bus_message* message, void*, sd_bus_error*) {
            sd_bus_message* reply = nullptr;
            if (int r = sd_bus_message_new_method_return(message, &reply); r < 0) return r;
            sd_bus_message_open_container(reply, 'a', "(iisv)");
            sd_bus_message_close_container(reply);
            const int r = sd_bus_send(nullptr, reply, nullptr);
            sd_bus_message_unref(reply);
            return r;
        }

        int MethodScrollSubstringTo(sd_bus_message* message, void*, sd_bus_error*) {
            // Nothing here scrolls to a substring, and saying so is better than a silent true.
            return sd_bus_reply_method_return(message, "b", 0);
        }

        int PropertyTextInt(sd_bus*, const char* path, const char*, const char* property,
                            sd_bus_message* reply, void* userdata, sd_bus_error*) {
            const Snapshot::Item* item = ItemFor(userdata, path);
            i32 out = 0;
            if (item) {
                if (std::strcmp(property, "CaretOffset") == 0) out = static_cast<i32>(item->caret);
                else                                           out = Characters(item->text).Count();
            }
            return sd_bus_message_append(reply, "i", out);
        }

        // --- Action ---------------------------------------------------------------------------

        // The actions of one node, in a fixed order, so an index means the same thing twice.
        std::vector<Action> ActionsOf(const Snapshot::Item* item) {
            std::vector<Action> out;
            if (!item) return out;
            for (u32 i = 0; i < static_cast<u32>(Action::Count); ++i)
                if (Has(item->actions, static_cast<Action>(i))) out.push_back(static_cast<Action>(i));
            return out;
        }

        int PropertyActionCount(sd_bus*, const char* path, const char*, const char*,
                                sd_bus_message* reply, void* userdata, sd_bus_error*) {
            const auto actions = ActionsOf(ItemFor(userdata, path));
            return sd_bus_message_append(reply, "i", static_cast<i32>(actions.size()));
        }

        int MethodActionName(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 index = 0;
            if (int r = sd_bus_message_read(message, "i", &index); r < 0) return r;
            const auto actions = ActionsOf(ItemFor(userdata, sd_bus_message_get_path(message)));
            const bool valid = index >= 0 && static_cast<std::size_t>(index) < actions.size();
            return sd_bus_reply_method_return(message, "s",
                valid ? ActionName(actions[static_cast<std::size_t>(index)]) : "");
        }

        int MethodActionDescription(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 index = 0;
            if (int r = sd_bus_message_read(message, "i", &index); r < 0) return r;
            const auto actions = ActionsOf(ItemFor(userdata, sd_bus_message_get_path(message)));
            const bool valid = index >= 0 && static_cast<std::size_t>(index) < actions.size();
            return sd_bus_reply_method_return(message, "s",
                valid ? ActionDescription(actions[static_cast<std::size_t>(index)]) : "");
        }

        int MethodActionKeyBinding(sd_bus_message* message, void*, sd_bus_error*) {
            // VAE has no per-control accelerators to report. An empty string is the protocol's
            // "none", and inventing "Return" for every button would be a lie a reader reads out.
            i32 index = 0;
            if (int r = sd_bus_message_read(message, "i", &index); r < 0) return r;
            return sd_bus_reply_method_return(message, "s", "");
        }

        int MethodGetActions(sd_bus_message* message, void* userdata, sd_bus_error*) {
            const auto actions = ActionsOf(ItemFor(userdata, sd_bus_message_get_path(message)));
            sd_bus_message* reply = nullptr;
            if (int r = sd_bus_message_new_method_return(message, &reply); r < 0) return r;
            sd_bus_message_open_container(reply, 'a', "(sss)");
            for (Action action : actions)
                sd_bus_message_append(reply, "(sss)", ActionName(action),
                                      ActionDescription(action), "");
            sd_bus_message_close_container(reply);
            const int r = sd_bus_send(nullptr, reply, nullptr);
            sd_bus_message_unref(reply);
            return r;
        }

        int MethodDoAction(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 index = 0;
            if (int r = sd_bus_message_read(message, "i", &index); r < 0) return r;
            AtspiBridge* bridge = Self(userdata);
            const char* path = sd_bus_message_get_path(message);
            const auto actions = ActionsOf(ItemFor(userdata, path));
            if (index < 0 || static_cast<std::size_t>(index) >= actions.size())
                return sd_bus_reply_method_return(message, "b", 0);
            const bool done = bridge->Perform(bridge->IndexForPath(path),
                                              actions[static_cast<std::size_t>(index)]);
            return sd_bus_reply_method_return(message, "b", done ? 1 : 0);
        }

        // --- Component, the half that acts --------------------------------------------------

        int MethodGrabFocus(sd_bus_message* message, void* userdata, sd_bus_error*) {
            AtspiBridge* bridge = Self(userdata);
            const u32 node = bridge->IndexForPath(sd_bus_message_get_path(message));
            return sd_bus_reply_method_return(message, "b", bridge->GrabFocus(node) ? 1 : 0);
        }

        // --- EditableText -------------------------------------------------------------------
        //
        // Reading a field out and being unable to change it is half an app. Everything here comes
        // down to one operation on the widget — replace these characters with those — so that a
        // screen reader's typing goes through the same behavior a keyboard's does rather than down
        // a second path with its own bugs.

        int MethodSetTextContents(sd_bus_message* message, void* userdata, sd_bus_error*) {
            const char* text = nullptr;
            if (int r = sd_bus_message_read(message, "s", &text); r < 0) return r;
            AtspiBridge* bridge = Self(userdata);
            const char* path = sd_bus_message_get_path(message);
            const Snapshot::Item* item = ItemFor(userdata, path);
            const i32 count = item ? Characters(item->text).Count() : 0;
            const bool done = bridge->Edit(bridge->IndexForPath(path), 0, count,
                                           text ? text : "");
            return sd_bus_reply_method_return(message, "b", done ? 1 : 0);
        }

        int MethodInsertText(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 position = 0, length = 0;
            const char* text = nullptr;
            if (int r = sd_bus_message_read(message, "isi", &position, &text, &length); r < 0)
                return r;
            // `length` is in characters and is allowed to be shorter than the string, which is how
            // a client inserts a prefix of what it holds. Negative means all of it.
            std::string_view insert = text ? std::string_view(text) : std::string_view{};
            if (length >= 0) insert = Characters(insert).Slice(0, length);
            AtspiBridge* bridge = Self(userdata);
            const char* path = sd_bus_message_get_path(message);
            const auto at = static_cast<u32>(std::max(position, 0));
            return sd_bus_reply_method_return(message, "b",
                bridge->Edit(bridge->IndexForPath(path), at, at, insert) ? 1 : 0);
        }

        int MethodDeleteText(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 start = 0, end = 0;
            if (int r = sd_bus_message_read(message, "ii", &start, &end); r < 0) return r;
            AtspiBridge* bridge = Self(userdata);
            const char* path = sd_bus_message_get_path(message);
            const bool done = bridge->Edit(bridge->IndexForPath(path),
                                           static_cast<u32>(std::max(start, 0)),
                                           static_cast<u32>(std::max(end, 0)), {});
            return sd_bus_reply_method_return(message, "b", done ? 1 : 0);
        }

        template <bool kCut>
        int MethodCopyText(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 start = 0, end = 0;
            if (int r = sd_bus_message_read(message, "ii", &start, &end); r < 0) return r;
            AtspiBridge* bridge = Self(userdata);
            const char* path = sd_bus_message_get_path(message);
            const bool done = bridge->Clip(bridge->IndexForPath(path),
                                           static_cast<u32>(std::max(start, 0)),
                                           static_cast<u32>(std::max(end, 0)), kCut);
            // CopyText returns nothing at all in this protocol; CutText answers whether it did.
            if constexpr (kCut) return sd_bus_reply_method_return(message, "b", done ? 1 : 0);
            else                return sd_bus_reply_method_return(message, "");
        }

        int MethodPasteText(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 position = 0;
            if (int r = sd_bus_message_read(message, "i", &position); r < 0) return r;
            AtspiBridge* bridge = Self(userdata);
            const char* path = sd_bus_message_get_path(message);
            const bool done = bridge->Paste(bridge->IndexForPath(path),
                                            static_cast<u32>(std::max(position, 0)));
            return sd_bus_reply_method_return(message, "b", done ? 1 : 0);
        }

        // --- Selection ------------------------------------------------------------------------
        //
        // Which of a container's children is chosen. Read out of the snapshot like everything else;
        // choosing one reaches the live widget, and reaches it as a click, because that is how a
        // row gets selected when a pointer does it.

        std::vector<u32> SelectedChildren(const Snapshot& snapshot, const Snapshot::Item* item) {
            std::vector<u32> out;
            if (!item) return out;
            for (u32 child : item->children)
                if (child < snapshot.items.size()
                    && (snapshot.items[child].state & (1ull << kStateSelected)) != 0)
                    out.push_back(child);
            return out;
        }

        int PropertySelectionCount(sd_bus*, const char* path, const char*, const char*,
                                   sd_bus_message* reply, void* userdata, sd_bus_error*) {
            const Snapshot& snapshot = Self(userdata)->Data();
            const auto selected = SelectedChildren(snapshot, ItemFor(userdata, path));
            return sd_bus_message_append(reply, "i", static_cast<i32>(selected.size()));
        }

        int MethodGetSelectedChild(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 which = 0;
            if (int r = sd_bus_message_read(message, "i", &which); r < 0) return r;
            AtspiBridge* bridge = Self(userdata);
            const auto selected = SelectedChildren(bridge->Data(),
                                                   ItemFor(userdata, sd_bus_message_get_path(message)));
            sd_bus_message* reply = nullptr;
            if (int r = sd_bus_message_new_method_return(message, &reply); r < 0) return r;
            if (which < 0 || static_cast<std::size_t>(which) >= selected.size())
                AppendRef(reply, "", kNullPath);
            else
                AppendRef(reply, bridge->UniqueName(),
                          PathFor(selected[static_cast<std::size_t>(which)]));
            const int r = sd_bus_send(nullptr, reply, nullptr);
            sd_bus_message_unref(reply);
            return r;
        }

        int MethodIsChildSelected(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 index = 0;
            if (int r = sd_bus_message_read(message, "i", &index); r < 0) return r;
            const Snapshot& snapshot = Self(userdata)->Data();
            const Snapshot::Item* item = ItemFor(userdata, sd_bus_message_get_path(message));
            bool selected = false;
            if (item && index >= 0 && static_cast<std::size_t>(index) < item->children.size()) {
                const u32 child = item->children[static_cast<std::size_t>(index)];
                selected = child < snapshot.items.size()
                        && (snapshot.items[child].state & (1ull << kStateSelected)) != 0;
            }
            return sd_bus_reply_method_return(message, "b", selected ? 1 : 0);
        }

        template <bool kSelect>
        int MethodSelectChild(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 index = 0;
            if (int r = sd_bus_message_read(message, "i", &index); r < 0) return r;
            AtspiBridge* bridge = Self(userdata);
            const char* path = sd_bus_message_get_path(message);
            const bool done = bridge->Choose(bridge->IndexForPath(path),
                                             static_cast<u32>(std::max(index, 0)), kSelect);
            return sd_bus_reply_method_return(message, "b", done ? 1 : 0);
        }

        // Deselecting *the* selected child rather than a child by index — the same operation, one
        // level of indirection away.
        int MethodDeselectSelectedChild(sd_bus_message* message, void* userdata, sd_bus_error*) {
            i32 which = 0;
            if (int r = sd_bus_message_read(message, "i", &which); r < 0) return r;
            AtspiBridge* bridge = Self(userdata);
            const char* path = sd_bus_message_get_path(message);
            const Snapshot::Item* item = ItemFor(userdata, path);
            const auto selected = SelectedChildren(bridge->Data(), item);
            if (!item || which < 0 || static_cast<std::size_t>(which) >= selected.size())
                return sd_bus_reply_method_return(message, "b", 0);
            const u32 child = selected[static_cast<std::size_t>(which)];
            const auto at = std::find(item->children.begin(), item->children.end(), child);
            const auto index = static_cast<u32>(std::distance(item->children.begin(), at));
            return sd_bus_reply_method_return(message, "b",
                bridge->Choose(bridge->IndexForPath(path), index, false) ? 1 : 0);
        }

        // A VAE container selects one child, so there is no "all" to select and no way to end up
        // with none. Both answer false rather than pretending, which is what tells a client to
        // offer the user something else.
        int MethodSelectionRefusal(sd_bus_message* message, void*, sd_bus_error*) {
            return sd_bus_reply_method_return(message, "b", 0);
        }

        const sd_bus_vtable kAccessibleVtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_PROPERTY("Name", "s", PropertyName, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
            SD_BUS_PROPERTY("Description", "s", PropertyName, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
            SD_BUS_PROPERTY("Parent", "(so)", PropertyParent, 0, 0),
            SD_BUS_PROPERTY("ChildCount", "i", PropertyChildCount, 0, 0),
            SD_BUS_PROPERTY("Locale", "s", PropertyLocale, 0, 0),
            SD_BUS_PROPERTY("AccessibleId", "s", PropertyAccessibleId, 0, 0),
            SD_BUS_METHOD("GetChildAtIndex", "i", "(so)", MethodGetChildAtIndex,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetChildren", "", "a(so)", MethodGetChildren, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetIndexInParent", "", "i", MethodGetIndexInParent,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetRelationSet", "", "a(ua(so))", MethodGetRelationSet,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetRole", "", "u", MethodGetRole, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetRoleName", "", "s", MethodGetRoleName, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetLocalizedRoleName", "", "s", MethodGetRoleName,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetState", "", "au", MethodGetState, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetApplication", "", "(so)", MethodGetApplication,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetAttributes", "", "a{ss}", MethodGetAttributes,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetInterfaces", "", "as", MethodGetInterfaces, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };

        const sd_bus_vtable kComponentVtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("GetExtents", "u", "(iiii)", MethodGetExtents, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("Contains", "iiu", "b", MethodContains, SD_BUS_VTABLE_UNPRIVILEGED),
            // Moving the keyboard focus is how a screen reader walks an app, and it lives on
            // Component rather than Accessible because focus is about the thing on screen.
            SD_BUS_METHOD("GrabFocus", "", "b", MethodGrabFocus, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };

        const sd_bus_vtable kActionVtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_PROPERTY("NActions", "i", PropertyActionCount, 0, 0),
            SD_BUS_METHOD("GetName", "i", "s", MethodActionName, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetLocalizedName", "i", "s", MethodActionName,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetDescription", "i", "s", MethodActionDescription,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetKeyBinding", "i", "s", MethodActionKeyBinding,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetActions", "", "a(sss)", MethodGetActions, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("DoAction", "i", "b", MethodDoAction, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };

        const sd_bus_vtable kTextVtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_PROPERTY("CharacterCount", "i", PropertyTextInt, 0, 0),
            SD_BUS_PROPERTY("CaretOffset", "i", PropertyTextInt, 0,
                            SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
            SD_BUS_METHOD("GetText", "ii", "s", MethodGetText, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetStringAtOffset", "iu", "sii", MethodGetStringAtOffset,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetTextAtOffset", "iu", "sii",
                          MethodGetTextRelativeToOffset<0>, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetTextBeforeOffset", "iu", "sii",
                          MethodGetTextRelativeToOffset<-1>, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetTextAfterOffset", "iu", "sii",
                          MethodGetTextRelativeToOffset<1>, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetCharacterAtOffset", "i", "i", MethodGetCharacterAtOffset,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetCaretOffset", "", "i", MethodGetCaretOffset,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("SetCaretOffset", "i", "b", MethodSetCaretOffset,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetNSelections", "", "i", MethodGetNSelections,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetSelection", "i", "ii", MethodGetSelection,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("AddSelection", "ii", "b", MethodSetSelectionRange,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("SetSelection", "iii", "b", MethodSetSelectionRange,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("RemoveSelection", "i", "b", MethodRemoveSelection,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetAttributes", "i", "a{ss}ii", MethodGetTextAttributes,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetAttributeRun", "ib", "a{ss}ii", MethodGetTextAttributes,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetDefaultAttributes", "", "a{ss}", MethodGetDefaultAttributes,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetDefaultAttributeSet", "", "a{ss}", MethodGetDefaultAttributes,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetAttributeValue", "is", "s", MethodGetAttributeValue,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetCharacterExtents", "iu", "iiii", MethodGetCharacterExtents,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetRangeExtents", "iiu", "iiii", MethodGetRangeExtents,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetOffsetAtPoint", "iiu", "i", MethodGetOffsetAtPoint,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetBoundedRanges", "iiiiuuu", "a(iisv)", MethodGetBoundedRanges,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("ScrollSubstringTo", "iiu", "b", MethodScrollSubstringTo,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };

        const sd_bus_vtable kEditableTextVtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("SetTextContents", "s", "b", MethodSetTextContents,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("InsertText", "isi", "b", MethodInsertText, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("DeleteText", "ii", "b", MethodDeleteText, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("CopyText", "ii", "", MethodCopyText<false>, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("CutText", "ii", "b", MethodCopyText<true>, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("PasteText", "i", "b", MethodPasteText, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };

        const sd_bus_vtable kSelectionVtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_PROPERTY("NSelectedChildren", "i", PropertySelectionCount, 0,
                            SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
            SD_BUS_METHOD("GetSelectedChild", "i", "(so)", MethodGetSelectedChild,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("SelectChild", "i", "b", MethodSelectChild<true>,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("DeselectChild", "i", "b", MethodSelectChild<false>,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("DeselectSelectedChild", "i", "b", MethodDeselectSelectedChild,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("IsChildSelected", "i", "b", MethodIsChildSelected,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("SelectAll", "", "b", MethodSelectionRefusal, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("ClearSelection", "", "b", MethodSelectionRefusal,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };

        const sd_bus_vtable kValueVtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_PROPERTY("MinimumValue", "d", PropertyDouble, 0, 0),
            SD_BUS_PROPERTY("MaximumValue", "d", PropertyDouble, 0, 0),
            SD_BUS_PROPERTY("CurrentValue", "d", PropertyDouble, 0,
                            SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
            SD_BUS_PROPERTY("MinimumIncrement", "d", PropertyDouble, 0, 0),
            SD_BUS_VTABLE_END
        };

        const sd_bus_vtable kApplicationVtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_PROPERTY("ToolkitName", "s", PropertyToolkit, 0, 0),
            SD_BUS_PROPERTY("Version", "s", PropertyToolkit, 0, 0),
            SD_BUS_PROPERTY("AtspiVersion", "s", PropertyToolkit, 0, 0),
            SD_BUS_WRITABLE_PROPERTY("Id", "i", PropertyApplicationId, SetApplicationId, 0, 0),
            SD_BUS_VTABLE_END
        };

    }

    u32 AtspiBridge::IndexForPath(const char* path) const {
        if (!path) return Node::kInvalid;
        if (std::strcmp(path, kRootPath) == 0) return 0;
        const std::size_t prefix = std::strlen(kPathPrefix);
        if (std::strncmp(path, kPathPrefix, prefix) != 0 || path[prefix] != '/') return Node::kInvalid;
        char* end = nullptr;
        const unsigned long index = std::strtoul(path + prefix + 1, &end, 10);
        if (!end || *end != '\0') return Node::kInvalid;
        return static_cast<u32>(index);
    }

    bool AtspiBridge::Connect(std::string_view applicationName) {
        m_Application = std::string(applicationName);
        m_Snapshot.application = m_Application;

        // Where the accessibility bus is, which only the session bus knows.
        sd_bus* session = nullptr;
        if (sd_bus_open_user(&session) < 0) {
            m_Status = "no session bus";
            return false;
        }
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        const char* address = nullptr;
        int r = sd_bus_call_method(session, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus",
                                   "GetAddress", &error, &reply, "");
        if (r >= 0) r = sd_bus_message_read(reply, "s", &address);
        std::string busAddress = r >= 0 && address ? address : std::string{};
        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        sd_bus_unref(session);

        if (busAddress.empty()) {
            m_Status = "no accessibility bus on this desktop";
            return false;
        }

        if (sd_bus_new(&m_Bus) < 0
            || sd_bus_set_address(m_Bus, busAddress.c_str()) < 0
            || sd_bus_set_bus_client(m_Bus, 1) < 0
            || sd_bus_start(m_Bus) < 0) {
            m_Status = "cannot reach the accessibility bus at " + busAddress;
            Shutdown();
            return false;
        }

        const char* unique = nullptr;
        if (sd_bus_get_unique_name(m_Bus, &unique) < 0 || !unique) {
            m_Status = "the accessibility bus gave us no name";
            Shutdown();
            return false;
        }
        m_UniqueName = unique;

        // One vtable serving every object, rather than an object registered per node: the tree is
        // rebuilt whenever the screen changes, and re-registering hundreds of bus objects each
        // time would be the most expensive thing in the frame.
        struct Registration {
            const char* interface;
            const sd_bus_vtable* vtable;
            sd_bus_node_enumerator_t unused;
            int (*find)(sd_bus*, const char*, const char*, void*, void**, sd_bus_error*);
        };
        const Registration registrations[] = {
            { "org.a11y.atspi.Accessible",  kAccessibleVtable,  nullptr, FindAccessible },
            { "org.a11y.atspi.Component",   kComponentVtable,   nullptr, FindAccessible },
            { "org.a11y.atspi.Value",       kValueVtable,       nullptr, FindValue },
            { "org.a11y.atspi.Action",      kActionVtable,      nullptr, FindAction },
            { "org.a11y.atspi.Text",        kTextVtable,        nullptr, FindText },
            { "org.a11y.atspi.EditableText", kEditableTextVtable, nullptr, FindEditableText },
            { "org.a11y.atspi.Selection",   kSelectionVtable,   nullptr, FindSelection },
            { "org.a11y.atspi.Application", kApplicationVtable, nullptr, FindApplication },
        };
        for (const Registration& registration : registrations) {
            sd_bus_slot* slot = nullptr;
            if (sd_bus_add_fallback_vtable(m_Bus, &slot, kPathPrefix, registration.interface,
                                           registration.vtable, registration.find, this) < 0) {
                m_Status = std::string("cannot export ") + registration.interface;
                Shutdown();
                return false;
            }
            m_Slots.push_back(slot);
        }

        // A tree of one, so the root answers before anything has been published.
        if (m_Snapshot.items.empty()) {
            Snapshot::Item root;
            root.role = kRoleApplication;
            root.roleName = "application";
            root.name = m_Application;
            m_Snapshot.items.push_back(std::move(root));
        }

        if (!Announce()) { Shutdown(); return false; }
        m_Status = "connected to " + busAddress;
        return true;
    }

    // Hand the registry a reference to our root. Until this lands, nothing knows we are here.
    bool AtspiBridge::Announce() {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        const int r = sd_bus_call_method(m_Bus, "org.a11y.atspi.Registry", kRootPath,
                                         "org.a11y.atspi.Socket", "Embed", &error, &reply,
                                         "(so)", m_UniqueName.c_str(), kRootPath);
        if (r < 0) {
            m_Status = std::string("the accessibility registry refused us: ")
                     + (error.message ? error.message : "no reason given");
            sd_bus_error_free(&error);
            return false;
        }
        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        m_Embedded = true;
        return true;
    }

    void AtspiBridge::Publish(const Tree& tree) {
        Snapshot snapshot;
        snapshot.application = m_Application;
        snapshot.items.reserve(tree.Count());

        for (const Node& node : tree.Nodes()) {
            Snapshot::Item item;
            item.parent = node.parent;
            item.children = node.children;
            item.role = AtspiRole(node.role);
            item.roleName = RoleName(node.role);
            item.name = node.name;
            item.description = node.description;
            item.state = AtspiStates(node.state);
            item.x = static_cast<i32>(node.bounds.pos.x);
            item.y = static_cast<i32>(node.bounds.pos.y);
            item.w = static_cast<i32>(node.bounds.size.x);
            item.h = static_cast<i32>(node.bounds.size.y);
            item.hasValue = node.hasValue;
            item.value   = node.value;
            item.minimum = node.minimum;
            item.maximum = node.maximum;
            item.step    = node.step;
            item.actions = node.actions;
            item.text    = node.text;
            // A label carries text too, and a screen reader that can ask for it by range can read
            // a long one a line at a time instead of all at once.
            item.hasText = node.role == Role::Entry || node.role == Role::PasswordText
                        || node.role == Role::Label;
            item.editable = Has(node.state, State::Editable);
            item.characters.reserve(node.characters.size());
            for (const Rect& box : node.characters)
                item.characters.push_back({ static_cast<i32>(box.pos.x), static_cast<i32>(box.pos.y),
                                            static_cast<i32>(box.size.x),
                                            static_cast<i32>(box.size.y) });
            item.caret          = node.caret;
            item.selectionStart = node.selectionStart;
            item.selectionEnd   = node.selectionEnd;
            item.key            = node.key;
            snapshot.items.push_back(std::move(item));
        }
        // The root of the exported tree is the application, whatever the tree calls its own root.
        if (!snapshot.items.empty()) {
            snapshot.items[0].role = kRoleApplication;
            snapshot.items[0].roleName = "application";
            if (snapshot.items[0].name.empty()) snapshot.items[0].name = m_Application;
        }
        for (u32 i = 0; i < snapshot.items.size(); ++i)
            for (u32 slot = 0; slot < snapshot.items[i].children.size(); ++slot)
                snapshot.items[snapshot.items[i].children[slot]].indexInParent = slot;
        // A container is a selection if something inside it can be selected. Asked of the children
        // rather than of the role: a list of plain frames is not one, and a row that came out of a
        // repeated container is, without anybody having declared a list.
        for (Snapshot::Item& item : snapshot.items)
            for (u32 child : item.children)
                if (child < snapshot.items.size()
                    && (snapshot.items[child].state & (1ull << kStateSelectable)) != 0) {
                    item.selects = true;
                    break;
                }

        const Snapshot before = std::move(m_Snapshot);
        m_Snapshot = std::move(snapshot);

        // Following focus is the one thing a screen reader cannot do by polling: by the time it
        // noticed, the user has typed. Everything else it asks for when it wants it — but only if
        // it knows to ask, which is what the rest of the diff is for.
        u32 focused = Node::kInvalid;
        for (u32 i = 0; i < m_Snapshot.items.size(); ++i)
            if (m_Snapshot.items[i].state & (1ull << kStateFocused)) { focused = i; break; }
        if (focused != m_Focused) {
            if (m_Focused != Node::kInvalid) EmitState(m_Focused, "focused", false);
            if (focused != Node::kInvalid)   EmitState(focused, "focused", true);
            m_Focused = focused;
        }

        Diff(before, m_Snapshot);
    }

    // What changed, said out loud. Nodes are matched by their document key rather than by index,
    // because the view tree is rebuilt from scratch every frame: a dialog opening renumbers
    // everything, and an index-matched diff would report every control on the screen as changed.
    void AtspiBridge::Diff(const Snapshot& before, const Snapshot& after) {
        if (before.items.empty()) return;         // the first publish is not a change

        std::unordered_map<u64, u32> was;
        was.reserve(before.items.size());
        for (u32 i = 0; i < before.items.size(); ++i)
            if (before.items[i].key) was.emplace(before.items[i].key, i);

        // A cap, because a screen that replaced itself entirely has nothing useful to say
        // node by node — a client re-walks the tree after a children-changed anyway, and a
        // thousand signals in one frame is a stall on the bus rather than information.
        constexpr u32 kMaxEvents = 64;
        u32 sent = 0;

        for (u32 index = 0; index < after.items.size() && sent < kMaxEvents; ++index) {
            const Snapshot::Item& now = after.items[index];
            if (!now.key) continue;
            const auto found = was.find(now.key);
            if (found == was.end()) continue;
            const Snapshot::Item& then = before.items[found->second];

            // Focus is emitted above, from the one node that has it, so it is not repeated here.
            const std::pair<u32, const char*> watched[] = {
                { kStateChecked,   "checked" },   { kStatePressed,   "pressed" },
                { kStateSelected,  "selected" },  { kStateExpanded,  "expanded" },
                { kStateSensitive, "sensitive" }, { kStateShowing,   "showing" },
            };
            for (const auto& [bit, detail] : watched) {
                const bool had = (then.state & (1ull << bit)) != 0;
                const bool has = (now.state & (1ull << bit)) != 0;
                if (had != has) { EmitState(index, detail, has); ++sent; }
            }

            if (then.name != now.name)                    { EmitNameChanged(index, now.name); ++sent; }
            if (now.hasValue && then.value != now.value)  { EmitValueChanged(index, now.value); ++sent; }

            if (now.hasText && then.text != now.text) {
                // Where they stop agreeing, and where they start agreeing again. Everything
                // between is what was replaced, which is one delete and one insert however the
                // edit was actually made.
                const Characters had(then.text), has(now.text);
                i32 head = 0;
                while (head < had.Count() && head < has.Count()
                       && had.CodeAt(head) == has.CodeAt(head)) ++head;
                i32 tail = 0;
                while (tail < had.Count() - head && tail < has.Count() - head
                       && had.CodeAt(had.Count() - 1 - tail)
                          == has.CodeAt(has.Count() - 1 - tail)) ++tail;

                const i32 removed = had.Count() - head - tail;
                const i32 added   = has.Count() - head - tail;
                if (removed > 0) {
                    EmitTextChanged(index, false, static_cast<u32>(head), static_cast<u32>(removed),
                                    std::string(had.Slice(head, head + removed)));
                    ++sent;
                }
                if (added > 0) {
                    EmitTextChanged(index, true, static_cast<u32>(head), static_cast<u32>(added),
                                    std::string(has.Slice(head, head + added)));
                    ++sent;
                }
            }
            if (now.hasText && then.caret != now.caret) { EmitCaretMoved(index, now.caret); ++sent; }

            // Children, by key again. A list that grew a row says so rather than making a reader
            // notice on its own — which it does by reading the whole screen out again.
            std::vector<u64> hadKeys, hasKeys;
            for (u32 child : then.children)
                if (child < before.items.size()) hadKeys.push_back(before.items[child].key);
            for (u32 child : now.children)
                if (child < after.items.size()) hasKeys.push_back(after.items[child].key);
            if (hadKeys != hasKeys) {
                for (u32 slot = 0; slot < hasKeys.size() && sent < kMaxEvents; ++slot)
                    if (std::ranges::find(hadKeys, hasKeys[slot]) == hadKeys.end()) {
                        EmitChildrenChanged(index, true, slot, now.children[slot]);
                        ++sent;
                    }
                for (u32 slot = 0; slot < hadKeys.size() && sent < kMaxEvents; ++slot)
                    if (std::ranges::find(hasKeys, hadKeys[slot]) == hasKeys.end()) {
                        EmitChildrenChanged(index, false, slot, then.children[slot]);
                        ++sent;
                    }
            }
        }
    }

    // Every AT-SPI event has the same shape, whatever it is about: a detail string, two integers,
    // a variant of whatever else the event carries, and a property bag nobody fills in. Opening
    // one is what these five share; what goes in the variant is the only thing that differs.
    sd_bus_message* AtspiBridge::BeginEvent(u32 node, const char* signal, const char* detail,
                                            i32 first, i32 second) {
        if (!m_Bus || node >= m_Snapshot.items.size()) return nullptr;
        const std::string path = PathFor(node);
        sd_bus_message* message = nullptr;
        if (sd_bus_message_new_signal(m_Bus, &message, path.c_str(),
                                      "org.a11y.atspi.Event.Object", signal) < 0)
            return nullptr;
        if (sd_bus_message_append(message, "sii", detail, first, second) < 0) {
            sd_bus_message_unref(message);
            return nullptr;
        }
        return message;
    }

    void AtspiBridge::SendEvent(sd_bus_message* message) {
        if (!message) return;
        // The trailing a{sv}. Empty everywhere here: it carries a sender's own extra properties,
        // and inventing some would be data a screen reader has to decide to ignore.
        sd_bus_message_open_container(message, 'a', "{sv}");
        sd_bus_message_close_container(message);
        sd_bus_send(nullptr, message, nullptr);
        sd_bus_message_unref(message);
    }

    // `object:state-changed:<detail>`, which is what a screen reader listens for.
    void AtspiBridge::EmitState(u32 node, const char* detail, bool on) {
        sd_bus_message* message = BeginEvent(node, "StateChanged", detail, on ? 1 : 0, 0);
        if (!message) return;
        sd_bus_message_append(message, "v", "i", 0);
        SendEvent(message);
    }

    void AtspiBridge::EmitNameChanged(u32 node, const std::string& name) {
        sd_bus_message* message = BeginEvent(node, "PropertyChange", "accessible-name", 0, 0);
        if (!message) return;
        sd_bus_message_append(message, "v", "s", name.c_str());
        SendEvent(message);
    }

    void AtspiBridge::EmitValueChanged(u32 node, double value) {
        sd_bus_message* message = BeginEvent(node, "PropertyChange", "accessible-value", 0, 0);
        if (!message) return;
        sd_bus_message_append(message, "v", "d", value);
        SendEvent(message);
    }

    void AtspiBridge::EmitCaretMoved(u32 node, u32 offset) {
        sd_bus_message* message =
            BeginEvent(node, "TextCaretMoved", "", static_cast<i32>(offset), 0);
        if (!message) return;
        sd_bus_message_append(message, "v", "i", 0);
        SendEvent(message);
    }

    // The variant carries the text that came or went, which is what lets a screen reader read out
    // the character you just typed without asking for the whole field back.
    void AtspiBridge::EmitTextChanged(u32 node, bool inserted, u32 offset, u32 count,
                                      const std::string& text) {
        sd_bus_message* message = BeginEvent(node, "TextChanged", inserted ? "insert" : "delete",
                                             static_cast<i32>(offset), static_cast<i32>(count));
        if (!message) return;
        sd_bus_message_append(message, "v", "s", text.c_str());
        SendEvent(message);
    }

    void AtspiBridge::EmitChildrenChanged(u32 parent, bool added, u32 index, u32 child) {
        sd_bus_message* message = BeginEvent(parent, "ChildrenChanged", added ? "add" : "remove",
                                             static_cast<i32>(index), 0);
        if (!message) return;
        // A reference to the child, which is how the receiver knows which one without re-walking.
        const std::string path = PathFor(child);
        sd_bus_message_open_container(message, 'v', "(so)");
        sd_bus_message_append(message, "(so)", m_UniqueName.c_str(), path.c_str());
        sd_bus_message_close_container(message);
        SendEvent(message);
    }

    // --- and the three things it can ask to have done ------------------------------------------
    //
    // All of these run on the app's own thread: Pump() is called from the frame, so a bus callback
    // reaching into the live widget is reaching into it between frames rather than during one.
    // Every one of them answers false rather than nothing when there is no app to ask, because a
    // screen reader that says "done" about nothing is worse than one that says it cannot.

    bool AtspiBridge::Perform(u32 node, Action action) {
        return m_Actor && node < m_Snapshot.items.size() && m_Actor->Do(node, action);
    }

    bool AtspiBridge::SetCaret(u32 node, u32 start, u32 end) {
        if (!m_Actor || node >= m_Snapshot.items.size() || !m_Snapshot.items[node].hasText)
            return false;
        return m_Actor->SetCaret(node, start, end);
    }

    bool AtspiBridge::GrabFocus(u32 node) {
        return m_Actor && node < m_Snapshot.items.size() && m_Actor->Focus(node);
    }

    // Editing is refused here rather than in the widget when the field is not one that can be
    // typed into. The check is the same one that decides whether the interface exists at all, and
    // a client that got hold of a path before the tree changed under it gets an honest no.
    bool AtspiBridge::Edit(u32 node, u32 start, u32 end, std::string_view insert) {
        if (!m_Actor || node >= m_Snapshot.items.size()) return false;
        const Snapshot::Item& item = m_Snapshot.items[node];
        if (!item.hasText || !item.editable) return false;
        return m_Actor->EditText(node, std::min(start, end), std::max(start, end), insert);
    }

    bool AtspiBridge::Clip(u32 node, u32 start, u32 end, bool cut) {
        if (!m_Actor || node >= m_Snapshot.items.size()) return false;
        const Snapshot::Item& item = m_Snapshot.items[node];
        // Copying does not change the field, so a read-only one may still be copied from — but a
        // password field may not be copied from at all, by either. What it holds is exactly what
        // it exists to refuse to hand over.
        if (!item.hasText || item.role == kRolePasswordText) return false;
        if (cut && !item.editable) return false;
        return m_Actor->CopyText(node, std::min(start, end), std::max(start, end), cut);
    }

    bool AtspiBridge::Paste(u32 node, u32 at) {
        if (!m_Actor || node >= m_Snapshot.items.size()) return false;
        const Snapshot::Item& item = m_Snapshot.items[node];
        if (!item.hasText || !item.editable) return false;
        return m_Actor->PasteText(node, at);
    }

    bool AtspiBridge::Choose(u32 node, u32 child, bool selected) {
        if (!m_Actor || node >= m_Snapshot.items.size()) return false;
        const Snapshot::Item& item = m_Snapshot.items[node];
        if (!item.selects || child >= item.children.size()) return false;
        return m_Actor->SelectChild(node, child, selected);
    }

    void AtspiBridge::Pump() {
        if (!m_Bus) return;
        // Non-blocking, from the app's own loop: everything the callbacks read is a snapshot, so
        // there is nothing here to race with the frame that rebuilds the tree.
        while (sd_bus_process(m_Bus, nullptr) > 0) {}
    }

    void AtspiBridge::Shutdown() {
        for (sd_bus_slot* slot : m_Slots) sd_bus_slot_unref(slot);
        m_Slots.clear();
        if (m_Bus) { sd_bus_flush(m_Bus); sd_bus_unref(m_Bus); m_Bus = nullptr; }
        m_Embedded = false;
    }

    Scope<Bridge> Bridge::Create() { return CreateScope<AtspiBridge>(); }

}

#else

namespace vae::a11y {
    // No sd-bus at build time. An app still builds and runs; it simply has no bridge, which is
    // also what happens on a desktop with no accessibility bus running.
    Scope<Bridge> Bridge::Create() { return nullptr; }
}

#endif
