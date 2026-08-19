#include "dm_native.h"
#include "dm_replace.h"
#include "dm_trace.h"
#include "log.h"

#include <objbase.h>
#include <stdio.h>
#include <string.h>

/*
 * Wine resolves DirectMusic/DirectSound via CoCreate (system32 path) or
 * LoadLibrary("dsound.dll") from inside dmime — EXE IAT hooks miss that.
 * Fix: preload game-dir DLLs, SetDllDirectory, inline-hook CoCreateInstance +
 * LoadLibrary* so system32/app loads of our DLLs redirect to the game folder.
 */

typedef HRESULT(WINAPI *PFN_CoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef HRESULT(WINAPI *PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID *);
typedef HMODULE(WINAPI *PFN_LoadLibraryA)(LPCSTR);
typedef HMODULE(WINAPI *PFN_LoadLibraryW)(LPCWSTR);
typedef HMODULE(WINAPI *PFN_LoadLibraryExA)(LPCSTR, HANDLE, DWORD);
typedef HMODULE(WINAPI *PFN_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD);

static PFN_CoCreateInstance real_CoCreateInstance;
static PFN_LoadLibraryA real_LoadLibraryA;
static PFN_LoadLibraryW real_LoadLibraryW;
static PFN_LoadLibraryExA real_LoadLibraryExA;
static PFN_LoadLibraryExW real_LoadLibraryExW;

static BYTE *g_cci_tramp;
static BYTE g_cci_saved[16];
static BYTE *g_lla_tramp;
static BYTE g_lla_saved[16];
static BYTE *g_llw_tramp;
static BYTE g_llw_saved[16];
static BYTE *g_llea_tramp;
static BYTE g_llea_saved[16];
static BYTE *g_llew_tramp;
static BYTE g_llew_saved[16];

static char g_game_dir[MAX_PATH];
static int g_enabled = 1;
static volatile LONG g_in_ll; /* avoid recursion */

struct dm_map {
    GUID clsid;
    const char *dll;
};

static const char *const g_dll_names[] = {
    "dsound.dll",  "dswave.dll",  "dsdmo.dll",   "msdmo.dll",  "dmime.dll",
    "dmloader.dll", "dmusic.dll", "dmsynth.dll", "dmband.dll", "dmstyle.dll",
    "dmcompos.dll", NULL,
};

static const struct dm_map g_map[] = {
    {{0xd2ac2881, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmime.dll"},
    {{0xd2ac2882, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmime.dll"},
    {{0xd2ac2883, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmime.dll"},
    {{0xd2ac2884, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmime.dll"},
    {{0xd2ac2885, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmime.dll"},
    {{0xd2ac2886, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmime.dll"},
    {{0xd2ac2887, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmime.dll"},
    {{0xd2ac2888, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmime.dll"},
    {{0xeed36461, 0x9ea5, 0x11d3, {0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74}}, "dmime.dll"},
    {{0xd2ac2892, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmloader.dll"},
    {{0x636b9f10, 0x0c7d, 0x11d1, {0x95, 0xb2, 0x00, 0x20, 0xaf, 0xdc, 0x74, 0x21}}, "dmusic.dll"},
    {{0x480ff4b0, 0x28b2, 0x11d1, {0xbe, 0xf7, 0x00, 0xc0, 0x4f, 0xbf, 0x8f, 0xef}}, "dmusic.dll"},
    {{0x58c2b4d0, 0x46e7, 0x11d1, {0x89, 0xac, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}}, "dmsynth.dll"},
    {{0xaec17ce3, 0xa514, 0x11d1, {0xaf, 0xa6, 0x00, 0xaa, 0x00, 0x24, 0xd8, 0xb6}}, "dmsynth.dll"},
    {{0x79ba9e00, 0xb6ee, 0x11d1, {0x86, 0xbe, 0x00, 0xc0, 0x4f, 0xbf, 0x8f, 0xef}}, "dmband.dll"},
    {{0xd2ac2894, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmband.dll"},
    {{0xd2ac288a, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmstyle.dll"},
    {{0xd2ac288d, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmstyle.dll"},
    {{0xd2ac288b, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmstyle.dll"},
    {{0xd2ac288c, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmstyle.dll"},
    {{0xd2ac288e, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmstyle.dll"},
    {{0xd2ac288f, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmcompos.dll"},
    {{0xd2ac2890, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmcompos.dll"},
    {{0xd2ac2896, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}, "dmcompos.dll"},
    {{0x8a667154, 0xf9de, 0x4d22, {0xb8, 0x89, 0x24, 0x35, 0xb9, 0xa0, 0xbb, 0xcf}}, "dswave.dll"},
    {{0x47d4d946, 0x62e8, 0x11cf, {0x93, 0xbc, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}}, "dsound.dll"},
    /* DirectSound8 */
    {{0x3901cc3f, 0x84b5, 0x4fa4, {0xba, 0x35, 0xaa, 0x81, 0x72, 0xb8, 0xa0, 0x9b}}, "dsound.dll"},
};

static const char *dll_for_clsid(REFCLSID clsid)
{
    size_t i;
    if (!clsid)
        return NULL;
    for (i = 0; i < sizeof(g_map) / sizeof(g_map[0]); ++i) {
        if (IsEqualGUID(clsid, &g_map[i].clsid))
            return g_map[i].dll;
    }
    return NULL;
}

static int is_tracked_dll(const char *base)
{
    int i;
    if (!base || !base[0])
        return 0;
    for (i = 0; g_dll_names[i]; ++i) {
        if (lstrcmpiA(base, g_dll_names[i]) == 0)
            return 1;
    }
    return 0;
}

static const char *basename_a(const char *path)
{
    const char *b = path;
    const char *p;
    if (!path)
        return "";
    for (p = path; *p; ++p) {
        if (*p == '\\' || *p == '/')
            b = p + 1;
    }
    return b;
}

static void fill_game_dir(void)
{
    char *slash;
    DWORD n = GetModuleFileNameA(NULL, g_game_dir, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        g_game_dir[0] = '\0';
        return;
    }
    slash = strrchr(g_game_dir, '\\');
    if (!slash)
        slash = strrchr(g_game_dir, '/');
    if (slash)
        slash[1] = '\0';
    else
        g_game_dir[0] = '\0';
}

/* If name refers to a tracked DLL and game copy exists → write full game path. */
static int rewrite_to_game_a(const char *name, char *out, size_t out_sz)
{
    const char *base;
    size_t len;

    if (!name || !g_game_dir[0] || !out || out_sz < 8)
        return 0;
    base = basename_a(name);
    if (!is_tracked_dll(base))
        return 0;
    /* already loading from game dir */
    if (lstrlenA(name) >= (int)lstrlenA(g_game_dir)) {
        size_t i;
        int same = 1;
        for (i = 0; g_game_dir[i]; ++i) {
            char a = name[i], b = g_game_dir[i];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (char)(b - 'A' + 'a');
            if (a != b) {
                same = 0;
                break;
            }
        }
        if (same)
            return 0;
    }

    len = strlen(g_game_dir);
    if (len + strlen(base) + 1 >= out_sz)
        return 0;
    memcpy(out, g_game_dir, len + 1);
    lstrcatA(out, base);
    if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES)
        return 0;
    return 1;
}

static int rewrite_to_game_w(const wchar_t *name, wchar_t *out, size_t out_chars)
{
    char narrow[MAX_PATH];
    char path[MAX_PATH];
    int n;

    if (!name)
        return 0;
    n = WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, (int)sizeof(narrow), NULL, NULL);
    if (n <= 0)
        return 0;
    if (!rewrite_to_game_a(narrow, path, sizeof(path)))
        return 0;
    n = MultiByteToWideChar(CP_ACP, 0, path, -1, out, (int)out_chars);
    return n > 0;
}

static BOOL install_inline(void *target, void *hook, SIZE_T steal, BYTE **tramp_out, BYTE *saved)
{
    BYTE *tramp;
    DWORD oldprot;
    INT32 rel;
    SIZE_T i;

    if (!target || !hook || steal < 5 || steal > 16)
        return FALSE;
    tramp = (BYTE *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return FALSE;
    memcpy(tramp, target, steal);
    if (saved)
        memcpy(saved, target, steal);
    tramp[steal] = 0xE9;
    rel = (INT32)((BYTE *)target + (INT32)steal - (tramp + steal + 5));
    memcpy(tramp + steal + 1, &rel, 4);

    if (!VirtualProtect(target, steal, PAGE_EXECUTE_READWRITE, &oldprot)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return FALSE;
    }
    *(BYTE *)target = 0xE9;
    rel = (INT32)((BYTE *)hook - ((BYTE *)target + 5));
    memcpy((BYTE *)target + 1, &rel, 4);
    for (i = 5; i < steal; ++i)
        ((BYTE *)target)[i] = 0x90;
    VirtualProtect(target, steal, oldprot, &oldprot);
    FlushInstructionCache(GetCurrentProcess(), target, steal);
    *tramp_out = tramp;
    return TRUE;
}

static HRESULT create_from_game_dll(const char *dll, REFCLSID clsid, LPUNKNOWN outer, REFIID iid,
                                    LPVOID *ppv)
{
    char path[MAX_PATH];
    HMODULE mod;
    PFN_DllGetClassObject dgco;
    IClassFactory *cf = NULL;
    HRESULT hr;
    size_t len;

    if (!g_game_dir[0] || !dll || !ppv)
        return E_FAIL;
    *ppv = NULL;

    len = strlen(g_game_dir);
    if (len + strlen(dll) + 1 >= sizeof(path))
        return E_FAIL;
    memcpy(path, g_game_dir, len + 1);
    lstrcatA(path, dll);

    mod = GetModuleHandleA(dll);
    if (!mod) {
        InterlockedIncrement(&g_in_ll);
        mod = real_LoadLibraryA ? real_LoadLibraryA(path) : LoadLibraryA(path);
        InterlockedDecrement(&g_in_ll);
    }
    if (!mod) {
        log_msg("dm-native: LoadLibrary(%s) failed err=%lu", path, (unsigned long)GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }

    dgco = (PFN_DllGetClassObject)GetProcAddress(mod, "DllGetClassObject");
    if (!dgco) {
        log_msg("dm-native: no DllGetClassObject in %s", dll);
        return E_FAIL;
    }

    hr = dgco(clsid, &IID_IClassFactory, (void **)&cf);
    if (FAILED(hr) || !cf) {
        log_msg("dm-native: DllGetClassObject(%s) hr=0x%08lx", dll, (unsigned long)hr);
        return hr;
    }
    hr = cf->lpVtbl->CreateInstance(cf, outer, iid, ppv);
    cf->lpVtbl->Release(cf);
    if (SUCCEEDED(hr))
        log_msg("dm-native: CoCreate %s OK (%p)", dll, mod);
    else
        log_msg("dm-native: CreateInstance(%s) hr=0x%08lx", dll, (unsigned long)hr);
    return hr;
}

static HRESULT WINAPI hook_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid,
                                            LPVOID *ppv)
{
    const char *dll;
    HRESULT hr;

    (void)outer;
    (void)ctx;

    /* Prefer our DirectMusic stub over native/Wine when enabled. */
    if (dm_replace_enabled() && clsid && ppv) {
        hr = dm_replace_cocreate(clsid, iid, ppv);
        if (SUCCEEDED(hr))
            return hr;
    }

    if (g_enabled && clsid && ppv) {
        dll = dll_for_clsid(clsid);
        if (dll) {
            hr = create_from_game_dll(dll, clsid, outer, iid, ppv);
            if (SUCCEEDED(hr)) {
                if (ppv && *ppv)
                    dm_trace_on_cocreate(clsid, *ppv);
                return hr;
            }
            log_msg("dm-native: fallback CCI for %s", dll);
        }
    }
    hr = real_CoCreateInstance(clsid, outer, ctx, iid, ppv);
    if (SUCCEEDED(hr) && clsid && ppv && *ppv)
        dm_trace_on_cocreate(clsid, *ppv);
    return hr;
}

static HMODULE WINAPI hook_LoadLibraryA(LPCSTR name)
{
    char path[MAX_PATH];
    HMODULE m;

    if (g_enabled && !g_in_ll && name && rewrite_to_game_a(name, path, sizeof(path))) {
        InterlockedIncrement(&g_in_ll);
        m = real_LoadLibraryA(path);
        InterlockedDecrement(&g_in_ll);
        if (m) {
            log_msg("dm-native: LoadLibraryA %s -> %s", name, path);
            return m;
        }
    }
    return real_LoadLibraryA(name);
}

static HMODULE WINAPI hook_LoadLibraryW(LPCWSTR name)
{
    wchar_t path[MAX_PATH];
    HMODULE m;

    if (g_enabled && !g_in_ll && name && rewrite_to_game_w(name, path, MAX_PATH)) {
        InterlockedIncrement(&g_in_ll);
        m = real_LoadLibraryW(path);
        InterlockedDecrement(&g_in_ll);
        if (m) {
            log_msg("dm-native: LoadLibraryW redirected");
            return m;
        }
    }
    return real_LoadLibraryW(name);
}

static HMODULE WINAPI hook_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    char path[MAX_PATH];
    HMODULE m;

    if (g_enabled && !g_in_ll && name && rewrite_to_game_a(name, path, sizeof(path))) {
        InterlockedIncrement(&g_in_ll);
        m = real_LoadLibraryExA(path, file, flags);
        InterlockedDecrement(&g_in_ll);
        if (m) {
            log_msg("dm-native: LoadLibraryExA %s -> %s", name, path);
            return m;
        }
    }
    return real_LoadLibraryExA(name, file, flags);
}

static HMODULE WINAPI hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    wchar_t path[MAX_PATH];
    HMODULE m;

    if (g_enabled && !g_in_ll && name && rewrite_to_game_w(name, path, MAX_PATH)) {
        InterlockedIncrement(&g_in_ll);
        m = real_LoadLibraryExW(path, file, flags);
        InterlockedDecrement(&g_in_ll);
        if (m) {
            log_msg("dm-native: LoadLibraryExW redirected");
            return m;
        }
    }
    return real_LoadLibraryExW(name, file, flags);
}

static void preload_game_dlls(void)
{
    int i;
    char path[MAX_PATH];
    size_t len = strlen(g_game_dir);

    for (i = 0; g_dll_names[i]; ++i) {
        HMODULE m;
        if (len + strlen(g_dll_names[i]) + 1 >= sizeof(path))
            continue;
        memcpy(path, g_game_dir, len + 1);
        lstrcatA(path, g_dll_names[i]);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            continue;
        InterlockedIncrement(&g_in_ll);
        m = real_LoadLibraryA ? real_LoadLibraryA(path) : LoadLibraryA(path);
        InterlockedDecrement(&g_in_ll);
        log_msg("dm-native: preload %s => %p err=%lu", path, (void *)m,
                m ? 0ul : (unsigned long)GetLastError());
    }
}

void dm_native_install(void)
{
    HMODULE ole;
    HMODULE k32;
    char env[32];
    DWORD n;

    n = GetEnvironmentVariableA("CK_DM_NATIVE", env, sizeof(env));
    if (n > 0 && n < sizeof(env) &&
        (env[0] == '0' || env[0] == 'f' || env[0] == 'F' || env[0] == 'n' || env[0] == 'N'))
        g_enabled = 0;

    fill_game_dir();
    log_msg("dm-native: game_dir=%s enabled=%d", g_game_dir, g_enabled);
    if (!g_enabled || !g_game_dir[0])
        return;

    SetDllDirectoryA(g_game_dir);
    log_msg("dm-native: SetDllDirectory(%s)", g_game_dir);

    k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        real_LoadLibraryA = (PFN_LoadLibraryA)GetProcAddress(k32, "LoadLibraryA");
        real_LoadLibraryW = (PFN_LoadLibraryW)GetProcAddress(k32, "LoadLibraryW");
        real_LoadLibraryExA = (PFN_LoadLibraryExA)GetProcAddress(k32, "LoadLibraryExA");
        real_LoadLibraryExW = (PFN_LoadLibraryExW)GetProcAddress(k32, "LoadLibraryExW");
    }

    /* Preload before hooks so dsound is already mapped when dmime InitAudio runs. */
    preload_game_dlls();

    /* Inline hooks catch calls from dmime/dmusic, not only the game EXE IAT. */
    ole = GetModuleHandleA("ole32.dll");
    if (ole)
        real_CoCreateInstance = (PFN_CoCreateInstance)GetProcAddress(ole, "CoCreateInstance");
    if (real_CoCreateInstance) {
        /* Wine/PE CoCreateInstance usually starts with mov edi,edi hotpatch (2) + more; steal 5. */
        if (install_inline((void *)real_CoCreateInstance, (void *)hook_CoCreateInstance, 5, &g_cci_tramp,
                           g_cci_saved)) {
            real_CoCreateInstance = (PFN_CoCreateInstance)g_cci_tramp;
            log_msg("dm-native: inline hooked ole32!CoCreateInstance");
        } else
            log_msg("dm-native: WARN inline CoCreateInstance failed");
    }

    if (real_LoadLibraryExW &&
        install_inline((void *)GetProcAddress(k32, "LoadLibraryExW"), (void *)hook_LoadLibraryExW, 5,
                       &g_llew_tramp, g_llew_saved)) {
        real_LoadLibraryExW = (PFN_LoadLibraryExW)g_llew_tramp;
        log_msg("dm-native: inline hooked LoadLibraryExW");
    }
    if (real_LoadLibraryExA &&
        install_inline((void *)GetProcAddress(k32, "LoadLibraryExA"), (void *)hook_LoadLibraryExA, 5,
                       &g_llea_tramp, g_llea_saved)) {
        real_LoadLibraryExA = (PFN_LoadLibraryExA)g_llea_tramp;
        log_msg("dm-native: inline hooked LoadLibraryExA");
    }
    if (real_LoadLibraryA &&
        install_inline((void *)GetProcAddress(k32, "LoadLibraryA"), (void *)hook_LoadLibraryA, 5,
                       &g_lla_tramp, g_lla_saved)) {
        real_LoadLibraryA = (PFN_LoadLibraryA)g_lla_tramp;
        log_msg("dm-native: inline hooked LoadLibraryA");
    }
    if (real_LoadLibraryW &&
        install_inline((void *)GetProcAddress(k32, "LoadLibraryW"), (void *)hook_LoadLibraryW, 5,
                       &g_llw_tramp, g_llw_saved)) {
        real_LoadLibraryW = (PFN_LoadLibraryW)g_llw_tramp;
        log_msg("dm-native: inline hooked LoadLibraryW");
    }
}

void dm_native_remove(void)
{
    /* Process exit — leave hooks. */
}
