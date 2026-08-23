# VAE

**Virtual App Engine** — a visual builder for desktop applications. Draw the interface on a canvas,
give it layout, style and state, attach logic in Lua or C++, then run it or export it as C++.

![VAE Studio](docs/studio.png)

C++23, premake5, Vulkan behind a swappable GPU abstraction, dependencies as git submodules. Linux is
the supported target.

## What it does

- **Designer.** Layers, screens, components, inspector, assets, script editor, console. The canvas is
  the runtime renderer drawing into an offscreen target, so the design and the running app are the
  same picture.
- **53 components.** Button, TextInput, Checkbox, Radio, Switch, Slider, Dropdown, Combobox, Tabs,
  Scroll, List, Table, Grid, Card, Modal, Popover, Toast, Tooltip, Menu, Menubar, Navbar, Command,
  Calendar, Carousel, Chart, Accordion, Progress and the rest. They are built from frames, text and
  layout rather than native code, so any of them can be opened and edited; an edited one is forked
  into the project and stops tracking the library.
- **Figma layout semantics.** Auto-layout stacks with hug/fill/fixed sizing, absolute children with
  edge constraints, wrap, gap, padding, alignment. Solved headlessly and unit-tested.
- **Instanced SDF rendering.** Rounded rectangles with per-corner radii, borders, solid/linear/radial
  fills, images, analytic shadows and glyph quads, all as instances in one storage buffer. A screen
  is two or three draw calls.
- **Logic in Lua or C++,** chosen once per project. Lua 5.4 through sol2, or a native module compiled
  to a `.so` and reached through a C ABI table. Components bind by name and each instance gets its
  own state.
- **Services:** files, key-value storage, HTTP, WebSockets, timers and audio.
- **Data-bound rows.** A container marked repeating plus a table of rows handed over at runtime; a
  node inside the template says which column it draws.
- **Export C++.** The document is the source of truth; export emits readable C++ against the same
  public API, plus a premake project that builds it.

## Repository layout

| Path | What |
|------|------|
| `VAE/` | Engine. Two static libraries: `VAE-Core` (headless — document, layout, reactive graph, text, codegen) and `VAE` (window, GPU, renderer, widgets, scripting, services) |
| `VAE-Studio/` | The designer |
| `VAE-Player/` | Standalone runtime for a project |
| `VAE-Tests/` | Unit tests; links `VAE-Core` only, so they need no GPU and no window |
| `VAE/vendor/` | Dependencies, as submodules |
| `vendor-build/` | premake scripts for the dependencies compiled from source |
| `scripts/` | Setup, shader compilation, Wayland protocol generation |

## Building

Needs a C++23 compiler, `premake5` (5.0.0-beta8 or newer), `glslc` (from shaderc), `wayland-scanner`,
and the Vulkan loader. Vulkan headers are vendored.

```sh
git clone --recursive <url> vae
cd vae
./scripts/Setup.sh                   # submodules + premake5 gmake
./scripts/CompileShaders.sh          # GLSL -> SPIR-V, needed once and after any shader change
make config=debug -j$(nproc)         # or config=release / config=dist
```

Binaries land in `bin/<config>-linux-x86_64/<project>/`. Re-run `premake5 gmake` after adding a
source file — the project files are generated from globs.

## Running

```sh
./bin/Debug-linux-x86_64/VAE-Studio/VAE-Studio
./bin/Debug-linux-x86_64/VAE-Player/VAE-Player <project.vaescreen> [--screen NAME]
```

The Player loads the script beside the project — `<project>.so` or `<project>.lua`, native winning
when both exist. `--headless [--frames N]` lays out and paints without a window and exits non-zero if
the project or its script failed.

Projects live under `~/Documents/VAE` (or `$VAE_PROJECTS`), one directory each.

| Variable | Effect |
|---|---|
| `VAE_GPU` | `vulkan` (default), `d3d12`, `software`. Unimplemented backends report why and fall back. `software` routes through Mesa's lavapipe ICD and needs `vulkan-swrast`. |
| `VAE_PROJECTS` | Projects root. Otherwise `$XDG_DOCUMENTS_DIR/VAE`, then `~/Documents/VAE`. |
| `VAE_ROOT` | Engine asset root. Otherwise the engine walks up from the executable for a `.vaeroot` marker. |
| `VAE_FRAMES` | Render N frames and exit. |

## Documents

`.vaescreen` and `.vaecomp` are XML (format 3). The element name is the node kind, the tree is the
indentation, and only what cannot be worked out on load is written — defaults, derivable types,
unreferenced ids and the stock component catalog are all left out.

```xml
<vae version="3" library="vae.std@1" theme="dark">
  <tokens>
    <token name="accent" dark="0.365 0.51 0.894 1" light="0.29 0.44 0.85 1"/>
  </tokens>
  <screen name="Home" start="true" mode="stack" width="1280" height="800" padding="48" gap="32"
          align="center" justify="center" fill="@surface">
    <instance name="Left" of="c80f0394e2916d19" width="260"/>
  </screen>
</vae>
```

Attribute sigils: `@token`, `=binding`, `#hex`, `&node`, `*asset`, `$literal`. Sizes are `72`, `hug`,
`fill`, `fill 2` or `50%`; padding takes 1, 2 or 4 values.

Files written in the earlier JSON formats still open — `VAE-Studio --convert [--check] [--bench N]`
rewrites one in format 3, verifies the round trip node for node, or times the load.

## Scripting

A script registers classes by component name. Every instance of that component runs one, with its own
`self` and its own state.

```lua
vae.component("Counter", {
    on_mount = function(self) self:show() end,

    on_event = function(self, event)
        if event.kind ~= "clicked" then return end
        if event.source == "Increment" then
            self:set_state("count", self:state("count") + 1)
        end
        self:show()
    end,

    show = function(self)
        self:set_text("Count", "text", tostring(math.floor(self:state("count"))))
    end,
})
```

The C++ form is the same shape against `vae/script/VaeScriptAPI.h`, compiled to a `.so` beside the
project.

## Tests

```sh
./bin/Debug-linux-x86_64/VAE-Tests/VAE-Tests
./bin/Debug-linux-x86_64/VAE-Tests/VAE-Tests --filter=layout

VAE-Studio --selftest    # editor logic, no window and no device; exit code is the result
```

`--selftest` also rewrites the sample projects into the projects root.

## Built with it

![Vaecord](docs/vaecord.png)

A chat client: one `.vaescreen`, one Lua file, rows fed to repeating containers, WebSockets for the
transport.
