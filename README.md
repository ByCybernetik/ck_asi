# Celtic Kings / Imperivm ASI

Win32 plugin extracted from the old `winmm_proxy`: Vulkan present, soft 1920×1080,
cutscenes, CVM — now as **`CK.asi`**, injected by a thin **`winmm.dll`** loader
(or any Ultimate ASI Loader).

## Build

### Meson (recommended)

```bash
git submodule update --init
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
git submodule update --init
make                # CK.asi + winmm.dll
make GPU_SCENE=0    # without GPU scene modules
make install        # → ../Imperivm/scripts/CK.asi + ../Imperivm/winmm.dll
```

Needs `i686-w64-mingw32-gcc` and the dependencies listed below.

## Third-party dependencies

Git submodules in `third_party/` — fetch with `git submodule update --init`.

| Library | Version | License | Usage | Path |
|---------|---------|---------|-------|------|
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | 1.3.302 | Apache-2.0 / MIT | Swapchain present, GPU terrain/decor/obj rendering | `third_party/Vulkan-Headers/` |
| [FreeType](https://github.com/freetype/freetype) | 2.14.3 | FTL / GPL-2.0 | Tooltip text rendering (`tip_font.c`) | `third_party/freetype/` |
| [stb](https://github.com/nothings/stb) | latest | Public domain | stb_vorbis 1.22 (Ogg Vorbis), stb_truetype 1.26 (TTF rasterization) | `third_party/stb/` |

### Win32 SDK libraries (provided by MinGW)

`kernel32`, `user32`, `gdi32`, `ole32`, `dsound`, `strmiids`, `oleaut32`,
`winmm`, `ws2_32` — linked at build time from the MinGW sysroot.

### Optional / external (not in repo)

| Dependency | Usage |
|------------|-------|
| `src/menu/native_src` → Imperivm 2 `native/src` | Game UI source (symlink) |
| `src/menu/native_tp` → Imperivm 2 `native/third_party` | stb, other TP for menu (symlink) |
| `deps/freetype/lib/` | Prebuilt FreeType i686 static lib (not in repo) |

## Layout

| Path | Role |
|------|------|
| `src/` | All source code |
| `src/loader/` | winmm.dll proxy loader |
| `src/menu/` | Native menu system (C++) |
| `src/shaders/` | GLSL shaders + SPIR-V headers |
| `third_party/` | Git submodules (Vulkan-Headers, FreeType, stb) |
| `cross/` | Meson cross-compilation files |
| `deps/freetype/lib/` | Prebuilt FreeType static lib (external) |

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
| `CK_DM_REPLACE=0` | Disable DirectMusic stub; use native/Wine DirectMusic (default: **replace** with DirectSound stub) |
| `CK_PROXY_PROFILE=full` | Extra Wait/Sleep/terrain debug hooks (can freeze) |
| `CK_PROXY_PROFILE=min` | Default: slim hook set |

DirectMusic: `dm*.dll` / `dsound.dll` / `dswave.dll` next to the EXE are loaded via
`CoCreateInstance` redirect (Wine otherwise uses builtin stubs from system32).

Logs: `ck_asi.log` (next to the ASI), `ck_loader.log` (next to winmm).
