# Celtic Kings / Imperivm ASI

Win32 plugin extracted from the old `winmm_proxy`: Vulkan present, soft 1920×1080,
cutscenes, CVM — now as **`CK.asi`**, injected by a thin **`winmm.dll`** loader
(or any Ultimate ASI Loader).

## Build

### Meson (recommended)

```bash
meson setup builddir --cross-file cross/i686-w64-mingw32.txt
meson compile -C builddir
```

Without GPU terrain/decor/obj rendering:

```bash
meson setup builddir --cross-file cross/i686-w64-mingw32.txt -Dgpu_scene=false
meson compile -C builddir
```

### Make (legacy)

```bash
make                # CK.asi + winmm.dll
make GPU_SCENE=0    # without GPU scene modules
make install        # → ../Imperivm/scripts/CK.asi + ../Imperivm/winmm.dll
```

Needs `i686-w64-mingw32-gcc` and the dependencies listed below.

## Third-party dependencies

| Library | Version | License | Usage | Source |
|---------|---------|---------|-------|--------|
| [Vulkan Headers](https://github.com/KhronosGroup/Vulkan-Headers) | 1.0+ | Apache-2.0 / MIT | Swapchain present, GPU terrain/decor/obj rendering | System (`/usr/include/vulkan`) |
| [FreeType](https://freetype.org/) | 2.x | FTL / GPL-2.0 | Tooltip text rendering (`tip_font.c`) | Vendored in `deps/freetype/` |
| [stb_vorbis](https://github.com/nothings/stb) | 1.22 | Public domain | Ogg Vorbis audio decoding (DirectMusic replacement) | Bundled: `src/stb_vorbis.c` |
| [stb_truetype](https://github.com/nothings/stb) | 1.26 | Public domain | TrueType font rasterization (zoom map text) | Bundled: `src/stb_truetype.h` |
| [MinGW-w64](https://www.mingw-w64.org/) | — | Various (ZPL / public domain) | Cross-compiler toolchain + Win32/COM/DirectSound/DirectShow headers | System package |

### Win32 SDK libraries (provided by MinGW)

`kernel32`, `user32`, `gdi32`, `ole32`, `dsound`, `strmiids`, `oleaut32`,
`winmm`, `ws2_32` — linked at build time from the MinGW sysroot.

### Optional / external (not in repo)

| Dependency | Usage |
|------------|-------|
| `menu/native_src` → Imperivm 2 `native/src` | Game UI source (symlink) |
| `menu/native_tp` → Imperivm 2 `native/third_party` | stb, other TP for menu (symlink) |

## Layout

| Path | Role |
|------|------|
| `src/` | All source code |
| `src/loader/` | winmm.dll proxy loader |
| `src/menu/` | Native menu system (C++) |
| `src/shaders/` | GLSL shaders + SPIR-V headers |
| `cross/` | Meson cross-compilation files |
| `deps/freetype/` | Vendored FreeType (include + lib) |

## Run (Wine)

Same as before — game dir `CelticKings` with:

```bash
export WINEDLLOVERRIDES=winmm=n,b
```

Loader looks for `scripts/CK.asi`, then `plugins/CK.asi`, then `CK.asi`.

## Ultimate ASI Loader (optional)

Instead of our thin winmm, place [UAL](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
`dinput8.dll` / `version.dll` and put `CK.asi` in `scripts/`. Do **not** use both
loaders as `winmm.dll` at once.

## Env

| Var | Meaning |
|-----|---------|
| `CK_GDI_FALLBACK=1` | Disable Vulkan present |
| `CK_DM_NATIVE=0` | Disable game-dir DirectMusic CoCreate redirect |
| `CK_DM_REPLACE=0` | Use native/Wine DirectMusic (default: **replace** with DirectSound stub) |
| `CK_PROXY_PROFILE=full` | Extra Wait/Sleep/terrain debug hooks (can freeze) |
| `CK_PROXY_PROFILE=min` | Default: slim hook set |

DirectMusic: `dm*.dll` / `dsound.dll` / `dswave.dll` next to the EXE are loaded via
`CoCreateInstance` redirect (Wine otherwise uses builtin stubs from system32).

Logs: `ck_asi.log` (next to the ASI), `ck_loader.log` (next to winmm).
