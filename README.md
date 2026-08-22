# VAE

Virtual App Engine — a visual builder for applications. Drag and drop components on a canvas, give
them layout and style, attach logic in Lua or C++, and run or export the result.

Vulkan renderer behind a swappable GPU abstraction. C++23, premake5, dependencies as git submodules.

## Layout

| Path | What |
|------|------|
| `VAE/` | Engine. Two static libraries: `VAE-Core` (headless: document, layout, reactive graph, text metrics, codegen) and `VAE` (window, GPU, renderer, widgets, scripting) |
| `VAE-Studio/` | The designer |
| `VAE-Player/` | Standalone runtime for a built project |
| `VAE-Tests/` | Headless unit tests; links `VAE-Core` only |
| `VAE/vendor/` | Vendored dependencies (submodules) |
| `vendor-build/` | premake scripts for the dependencies we compile |
| `scripts/` | Setup, shader compilation, Wayland protocol generation |

## Building (Linux)

```sh
git clone --recursive <url> vae
cd vae
./scripts/Setup.sh                  # submodules, wayland protocols, premake
make config=debug -j$(nproc)        # or config=release / config=dist
```

Requires a C++23 compiler, `premake5`, `wayland-scanner`, the Vulkan loader
(`vulkan-icd-loader`), and `glslc` (from `shaderc`). Vulkan headers are vendored.

## Running

```sh
./bin/Debug-linux-x86_64/VAE-Studio/VAE-Studio
./bin/Debug-linux-x86_64/VAE-Player/VAE-Player <project.vaeproj>
```

`VAE_GPU=vulkan|d3d12|software` selects the graphics backend; unimplemented backends report why and
fall back to Vulkan. `software` routes Vulkan through Mesa's lavapipe ICD, which needs
`vulkan-swrast` installed.

`VAE_ROOT` overrides engine-asset resolution. Without it the engine walks up from the executable
looking for a `.vaeroot` marker.

## Tests

```sh
./bin/Debug-linux-x86_64/VAE-Tests/VAE-Tests            # all
./bin/Debug-linux-x86_64/VAE-Tests/VAE-Tests --filter=layout
```

GUI behaviour is driven inside a nested compositor rather than the live session:

```sh
vc box start vae --size 1600x900
vc box exec vae -- ./bin/Debug-linux-x86_64/VAE-Studio/VAE-Studio &
vc in vae move 400 300 click left
vc shot vae out.png
vc box kill vae
```

`VAE-Studio --selftest` runs the same interaction checks with the window created but never shown.
