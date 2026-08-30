#include "vaepch.h"
#include "vae/ui/Library.h"

#include "vae/ui/library/Catalog.h"

namespace vae::ui {

    namespace {
        doc::Token Colours(Color light, Color dark) { return doc::Token{ light, dark, {} }; }
    }

    void InstallDefaultTokens(doc::Document& document) {
        document.SetToken("bg",          Colours({ 0.96f, 0.96f, 0.97f, 1.0f }, { 0.086f, 0.094f, 0.118f, 1.0f }));
        document.SetToken("surface",     Colours({ 1.00f, 1.00f, 1.00f, 1.0f }, { 0.129f, 0.141f, 0.176f, 1.0f }));
        document.SetToken("surfaceAlt",  Colours({ 0.93f, 0.93f, 0.95f, 1.0f }, { 0.176f, 0.192f, 0.235f, 1.0f }));
        document.SetToken("border",      Colours({ 0.84f, 0.85f, 0.87f, 1.0f }, { 0.255f, 0.278f, 0.333f, 1.0f }));
        document.SetToken("text",        Colours({ 0.09f, 0.10f, 0.13f, 1.0f }, { 0.918f, 0.929f, 0.949f, 1.0f }));
        document.SetToken("textMuted",   Colours({ 0.42f, 0.44f, 0.49f, 1.0f }, { 0.576f, 0.612f, 0.671f, 1.0f }));
        document.SetToken("accent",      Colours({ 0.29f, 0.44f, 0.85f, 1.0f }, { 0.365f, 0.510f, 0.894f, 1.0f }));
        document.SetToken("accentHover", Colours({ 0.34f, 0.49f, 0.90f, 1.0f }, { 0.435f, 0.573f, 0.933f, 1.0f }));
        document.SetToken("accentActive",Colours({ 0.22f, 0.36f, 0.76f, 1.0f }, { 0.290f, 0.427f, 0.808f, 1.0f }));
        document.SetToken("accentText",  Colours({ 1.00f, 1.00f, 1.00f, 1.0f }, { 1.000f, 1.000f, 1.000f, 1.0f }));
        document.SetToken("danger",      Colours({ 0.80f, 0.24f, 0.24f, 1.0f }, { 0.902f, 0.353f, 0.353f, 1.0f }));
        document.SetToken("success",     Colours({ 0.16f, 0.60f, 0.36f, 1.0f }, { 0.298f, 0.733f, 0.475f, 1.0f }));
        document.SetToken("scrim",       Colours({ 0.00f, 0.00f, 0.00f, 0.35f }, { 0.000f, 0.000f, 0.000f, 0.55f }));
    }

    Uuid Library::Find(std::string_view name) const {
        auto it = components.find(name);
        return it == components.end() ? Uuid::Invalid() : it->second;
    }

    Library BuildStandardLibrary(doc::Document& document) {
        // The catalog itself lives in ui/library/, four files by what the widgets are for.
        // This is the one place that knows the whole list, and the name each entry answers to.
        using namespace vae::ui::catalog;
        InstallDefaultTokens(document);

        Library library;
        // Each widget gets its own id scope, so the catalog comes back on the same ids every time
        // it is rebuilt and adding a widget cannot renumber the ones already in people's files.
        auto Add = [&](const char* name, auto&& build) {
            document.PushIdScope(std::string("vae.std/") + name);
            library.components[name] = build(document);
            document.PopIdScope();
        };
        Add("Button", BuildButton);
        Add("TextInput", BuildTextInput);
        Add("Checkbox", [](doc::Document& d) { return BuildCheckLike(d, "Checkbox", Role::Checkbox, 4.0f, 4.0f, 2.0f); });
        Add("Radio", [](doc::Document& d) { return BuildCheckLike(d, "Radio", Role::Radio, 9.0f, 5.0f, 4.0f); });
        Add("Switch", BuildSwitch);
        Add("Slider", BuildSlider);
        Add("Dropdown", BuildDropdown);
        Add("Tabs", BuildTabs);
        Add("Scroll", BuildScroll);
        Add("List", BuildList);
        Add("Table", BuildTable);
        Add("Modal", BuildModal);
        Add("Popover", BuildPopover);
        Add("Toast", BuildToast);
        Add("Router", BuildRouter);
        Add("Icon", BuildIcon);
        Add("Image", BuildImage);

        // Containers and the states an app spends most of its time in. All composition — frames,
        // text and the layout modes — which is what having a layout engine is for.
        Add("Card", BuildCard);
        Add("Section", BuildSection);
        Add("Separator", BuildSeparator);
        Add("AspectRatio", BuildAspectRatio);
        Add("Grid", BuildGridView);
        Add("Sidebar", BuildSidebar);
        Add("Field", BuildField);
        Add("ButtonGroup", BuildButtonGroup);
        Add("InputGroup", BuildInputGroup);
        Add("Item", BuildItem);
        Add("Badge", BuildBadge);
        Add("Kbd", BuildKbd);
        Add("Empty", BuildEmpty);
        Add("Alert", BuildAlert);
        Add("Skeleton", BuildSkeleton);
        Add("Avatar", BuildAvatar);
        Add("Breadcrumb", BuildBreadcrumb);
        Add("Toggle", BuildToggle);

        // Behaviour, not composition: each of these needed a native half before the component
        // could exist. Together they are what a long page, a running task and a right-click are.
        Add("Collapsible", BuildCollapsible);
        Add("Accordion", BuildAccordion);
        Add("Progress", BuildProgress);
        Add("Spinner", BuildSpinner);
        Add("Chart", BuildChart);
        Add("InputOtp", BuildInputOtp);
        Add("Carousel", BuildCarousel);
        Add("Combobox", BuildCombobox);
        Add("Calendar", BuildCalendar);
        Add("Splitter", BuildSplitter);
        Add("Tooltip", BuildTooltip);
        Add("ContextMenu", BuildContextMenu);

        // Menus, navigation, and the two things every page has that no input covers: text you can
        // copy out of, and a place to say more without clicking.
        Add("Menu", BuildMenu);
        Add("Menubar", BuildMenubar);
        Add("Navbar", BuildNavbar);
        Add("Pagination", BuildPagination);
        Add("Command", BuildCommand);
        Add("HoverCard", BuildHoverCard);
        return library;
    }

}
