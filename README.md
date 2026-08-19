# Celtic Kings / Imperivm ASI

Win32 plugin extracted from the old `winmm_proxy`: Vulkan present, soft 1920×1080,
cutscenes, CVM — now as **`CK.asi`**, injected by a thin **`winmm.dll`** loader
(or any Ultimate ASI Loader).

## Build

```bash
make            # CK.asi + loader/winmm.dll
make install    # → ../Imperivm/scripts/CK.asi + ../Imperivm/winmm.dll
```

Needs `i686-w64-mingw32-gcc` and Vulkan headers (`/usr/include/vulkan`).

## Layout

| Path | Role |
|------|------|
| `CK.asi` | Game hooks + Vulkan (this project) |
| `loader/winmm.dll` | Forwards real winmm, `LoadLibrary(scripts/CK.asi)` |
| `hooks.c` / `vk_present.c` / `movie_player.c` | Core logic |

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
