# VAE

**Virtual App Engine** — a visual builder for desktop applications. Draw the interface on a canvas,
attach logic in Lua or C++, then run it or export it as C++.

![VAE Studio](docs/studio.png)

## Features

- The canvas draws with the app's own renderer — the design is the running app
- 53 components: buttons, inputs, tables, charts, modals, menus, calendars
- Any component can be opened and edited; an edited one forks into the project
- Figma layout: auto-layout stacks, hug/fill/fixed sizing, absolute children with constraints
- Components declare properties and variants; instances answer them
- Design tokens and themes, dark and light
- Data-bound rows — draw one row, hand over a table at runtime
- Logic in Lua or C++, chosen once per project
- Services for files, storage, HTTP, WebSockets, timers and audio
- SVG import, vector shapes, pictures and fonts as project assets
- Text shaped by HarfBuzz: right-to-left scripts, reordering, ligatures, colour emoji
- Translations from one flat JSON per locale
- Screen readers read an app over AT-SPI
- Undo and redo everywhere, autosave with a recovery copy
- Run a project on the canvas, or in its own window
- Documents are XML, one file per screen, diffable
- Export readable C++ that builds into a standalone folder

## Install

```sh
git clone --recursive https://github.com/mKonic/vae
cd vae
./install.sh
```

Needs a C++23 compiler, premake5 (5.0.0-beta8 or newer), `glslc`, `wayland-scanner` and the Vulkan
loader. Linux only for now.

`--copy` installs copies instead of symlinks into the clone, `--system` installs under
`/usr/local`, `--uninstall` removes it. `./install.sh --help` lists the rest.

## Usage

```sh
vae-studio                                          # the designer
vae-studio <project>                                # open one
vae-player <project> [--screen NAME] [--locale pt-BR]
vae-player <project> --headless                     # lay out and paint, no window
```

Projects live in `~/Documents/VAE`, one folder each — set `VAE_PROJECTS` to move them.

`F5` runs a project on the canvas. `Ctrl+F5` runs it in its own window, `Ctrl+F6` starting on the
screen you are looking at, and `Shift+F5` stops.

## Scripting

A script registers a class per component name. Every instance runs one, with its own state.

```lua
vae.component("Counter", {
    on_mount = function(self) self:show() end,

    on_event = function(self, event)
        if event.kind == "clicked" and event.source == "Increment" then
            self:set_state("count", self:state("count") + 1)
            self:show()
        end
    end,

    show = function(self)
        self:set_text("Count", "text", tostring(self:state("count")))
    end,
})
```

The C++ form is the same shape against `vae/script/VaeScriptAPI.h`, compiled to a `.so` beside the
project. The player loads whichever sits next to it, native winning when both do.

## Export

```sh
vae-studio --export <project> [directory]      # or File > Export C++
cd <Name>-export && premake5 gmake && make
```

The folder that comes out is the app: the binary, and beside it the fonts, pictures, translations
and script. Copy it to a machine with no VAE on it and it runs.

## Build from source

```sh
./scripts/Setup.sh                   # submodules, then premake5 gmake
./scripts/CompileShaders.sh          # GLSL -> SPIR-V
make config=debug -j8                # or config=release / config=dist
```

Binaries land in `bin/<config>-linux-x86_64/<project>/`. Re-run `premake5 gmake` after adding a
source file.

```sh
./bin/Debug-linux-x86_64/VAE-Tests/VAE-Tests
./bin/Debug-linux-x86_64/VAE-Studio/VAE-Studio --selftest
```

`VAE_GPU` picks the backend: `vulkan` (default), `software` (needs `vulkan-swrast`), `d3d12`.

## Built with it

![Vaecord](docs/vaecord.png)

A chat client: one screen, one Lua file, rows fed to repeating containers, WebSockets for the
transport.

## Licence

LGPL-3.0-or-later. See [COPYING.LESSER](COPYING.LESSER).

Vendored dependencies keep their own, all permissive: GLFW, Dear ImGui, Lua, sol2, spdlog, pugixml,
nlohmann/json, GLM, miniaudio, cpp-httplib, vk-bootstrap, VulkanMemoryAllocator, HarfBuzz,
ImGuiColorTextEdit, stb.
