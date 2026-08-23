#include "vaepch.h"
#include "vae/a11y/Bridge.h"

#ifdef VAE_A11Y_ATSPI

#include <systemd/sd-bus.h>

#include <array>
#include <cstdio>
#include <cstring>
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

        // Reached from the bus callbacks, which are free functions because sd-bus is C.
        const Snapshot& Data() const { return m_Snapshot; }
        u32 IndexForPath(const char* path) const;
        const char* UniqueName() const { return m_UniqueName.c_str(); }

    private:
        bool Announce();
        void EmitFocus(u32 node, bool gained);

        sd_bus* m_Bus = nullptr;
        std::vector<sd_bus_slot*> m_Slots;
        std::string m_UniqueName;
        std::string m_Status = "not started";
        std::string m_Application = "VAE";
        Snapshot m_Snapshot;
        bool m_Embedded = false;
        u32 m_Focused = Node::kInvalid;
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

        m_Snapshot = std::move(snapshot);

        // Following focus is the one thing a screen reader cannot do by polling: by the time it
        // noticed, the user has typed. Everything else it asks for when it wants it.
        u32 focused = Node::kInvalid;
        for (u32 i = 0; i < m_Snapshot.items.size(); ++i)
            if (m_Snapshot.items[i].state & (1ull << kStateFocused)) { focused = i; break; }
        if (focused != m_Focused) {
            if (m_Focused != Node::kInvalid) EmitFocus(m_Focused, false);
            if (focused != Node::kInvalid)   EmitFocus(focused, true);
            m_Focused = focused;
        }
    }

    // `object:state-changed:focused`, which is what a screen reader listens for. The signature is
    // AT-SPI's: a detail string, two integers, a variant of anything else, and a property bag.
    void AtspiBridge::EmitFocus(u32 node, bool gained) {
        if (!m_Bus || node >= m_Snapshot.items.size()) return;
        const std::string path = PathFor(node);

        sd_bus_message* signal = nullptr;
        if (sd_bus_message_new_signal(m_Bus, &signal, path.c_str(),
                                      "org.a11y.atspi.Event.Object", "StateChanged") < 0) return;
        sd_bus_message_append(signal, "siiv", "focused", gained ? 1 : 0, 0, "i", 0);
        sd_bus_message_open_container(signal, 'a', "{sv}");
        sd_bus_message_close_container(signal);
        sd_bus_send(nullptr, signal, nullptr);
        sd_bus_message_unref(signal);
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
