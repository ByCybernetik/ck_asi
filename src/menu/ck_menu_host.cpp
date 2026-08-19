/* RE MainMenu inside CK.asi — letterbox 4:3 + LoadGameMenu from native sources.
 * Own HWND for input; sized to tpw client. Do not hide tpw. */
#include "ck_menu.h"

#include "ui/dialog.hpp"
#include "ui/font_stb.hpp"
#include "ui/load_game_menu.hpp"
#include "ui/loc_xml.hpp"
#include "ui/main_menu.hpp"
#include "vfs/hmmsys.hpp"

#include "../log.h"
#include "../vk_present.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

struct FindHwndCtx {
    DWORD pid;
    HWND best;
    int best_area;
};

struct InputState {
    int cx = 0;
    int cy = 0;
    int click_pending = 0;
    int mouse_down = 0;
    int wheel_y = 0;
    int esc = 0;
    int enter = 0;
};

static InputState g_input;
static HWND g_menu_hwnd = NULL;
static HWND g_tpw_hwnd = NULL;
static WNDPROC g_tpw_prev = NULL;
static volatile LONG g_handoff_tutorial = 0;
static DWORD g_handoff_t0 = 0;

enum { CK_WM_TUTORIAL = WM_APP + 0x40, CK_TIMER_TUTORIAL = 0xC001 };

/* Retail open-game wrapper @ 0x58ee50 — cdecl(path, void **out, int a2, int a3); add esp,10.
 * Used by adventure/load paths; opens .BFHP via 0x58cae0. Skip UI start_mode @ 0x659bd0. */
typedef void*(__cdecl* PFN_RetailOpenGame)(const char* path, void** out, int a2, int a3);
enum { CK_RETAIL_OPEN_GAME = 0x58ee50 };
static const char kTutorialBfhp[] = "Adventures/Tutorial.BFHP";

/* #region agent log */
static void agent_log(const char* hid, const char* loc, const char* msg, const char* data_json) {
    FILE* df = fopen("Z:\\home\\cybernetik\\Games\\Imperivm\\ck_asi\\.cursor\\debug-764ba7.log", "a");
    if (!df)
        return;
    fprintf(df,
            "{\"sessionId\":\"764ba7\",\"runId\":\"direct-load\",\"hypothesisId\":\"%s\","
            "\"location\":\"%s\",\"message\":\"%s\",\"data\":%s,\"timestamp\":%lu}\n",
            hid, loc, msg, data_json ? data_json : "{}", (unsigned long)GetTickCount());
    fclose(df);
}
/* #endregion */

BOOL CALLBACK enum_find_tpw(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindHwndCtx*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != ctx->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    char cls[64] = {};
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (lstrcmpiA(cls, "CKNativeMenu") == 0 || lstrcmpiA(cls, "CKMenuMsg") == 0)
        return TRUE;
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    const int area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area < 640 * 400)
        return TRUE;
    if (area > ctx->best_area) {
        ctx->best_area = area;
        ctx->best = hwnd;
    }
    return TRUE;
}

HWND wait_tpw_hwnd(int timeout_ms) {
    const DWORD t0 = GetTickCount();
    HWND h = NULL;
    while (GetTickCount() - t0 < (DWORD)timeout_ms) {
        FindHwndCtx ctx = {};
        ctx.pid = GetCurrentProcessId();
        EnumWindows(enum_find_tpw, reinterpret_cast<LPARAM>(&ctx));
        h = ctx.best;
        if (h)
            return h;
        Sleep(50);
    }
    return NULL;
}

void resume_other_threads() {
    DWORD pid = GetCurrentProcessId();
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) {
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (th) {
                    while (ResumeThread(th) > 0) {
                    }
                    CloseHandle(th);
                }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

void retail_start_tutorial() {
    PFN_RetailOpenGame open_fn = (PFN_RetailOpenGame)CK_RETAIL_OPEN_GAME;
    void* out = NULL;
    void* factory = *(void**)0x9baf88;
    void* gate = *(void**)0x7a7d3c;
    /* #region agent log */
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"path\":\"%s\",\"fn\":%lu,\"factory\":%lu,\"gate\":%lu,\"a2\":0,\"a3\":0}",
                 kTutorialBfhp, (unsigned long)CK_RETAIL_OPEN_GAME,
                 (unsigned long)(ULONG_PTR)factory, (unsigned long)(ULONG_PTR)gate);
        agent_log("H1", "ck_menu_host.cpp:retail_start_tutorial", "open-before", buf);
    }
    /* #endregion */
    log_msg("ck_menu: direct open %s factory=%p gate=%p", kTutorialBfhp, factory, gate);
    void* game = open_fn(kTutorialBfhp, &out, 0, 0);
    /* #region agent log */
    {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "{\"game\":%lu,\"out\":%lu,\"factory_after\":%lu,\"gate_after\":%lu}",
                 (unsigned long)(ULONG_PTR)game, (unsigned long)(ULONG_PTR)out,
                 (unsigned long)(ULONG_PTR)(*(void**)0x9baf88),
                 (unsigned long)(ULONG_PTR)(*(void**)0x7a7d3c));
        agent_log("H1", "ck_menu_host.cpp:retail_start_tutorial", "open-after", buf);
    }
    /* #endregion */
    log_msg("ck_menu: direct open after game=%p out=%p", game, out);
}

LRESULT CALLBACK tpw_handoff_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == CK_WM_TUTORIAL || (msg == WM_TIMER && wp == CK_TIMER_TUTORIAL)) {
        if (!InterlockedCompareExchange(&g_handoff_tutorial, 0, 0))
            return 0;
        void* factory = *(void**)0x9baf88;
        const int ready = factory != NULL;
        const int timed_out = (GetTickCount() - g_handoff_t0) > 20000u;
        /* #region agent log */
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"ready\":%d,\"timeout\":%d,\"factory\":%lu,\"msg\":%u}",
                     ready, timed_out, (unsigned long)(ULONG_PTR)factory, (unsigned)msg);
            agent_log("H3", "ck_menu_host.cpp:tpw_proc", "poll", buf);
        }
        /* #endregion */
        if (!ready && !timed_out) {
            SetTimer(hwnd, CK_TIMER_TUTORIAL, 100, NULL);
            return 0;
        }
        KillTimer(hwnd, CK_TIMER_TUTORIAL);
        if (InterlockedExchange(&g_handoff_tutorial, 0))
            retail_start_tutorial();
        return 0;
    }
    if (g_tpw_prev)
        return CallWindowProcA(g_tpw_prev, hwnd, msg, wp, lp);
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int handoff_tutorial_to_retail(HWND tpw) {
    log_msg("ck_menu: Tutorial → retail handoff");
    /* #region agent log */
    agent_log("H1", "ck_menu_host.cpp:handoff", "begin", "{}");
    /* #endregion */

    if (g_menu_hwnd) {
        DestroyWindow(g_menu_hwnd);
        g_menu_hwnd = NULL;
    }
    vk_present_shutdown();
    vk_present_restore_game_soft();

    if (tpw && IsWindow(tpw)) {
        g_tpw_hwnd = tpw;
        if (!g_tpw_prev)
            g_tpw_prev = (WNDPROC)SetWindowLongA(tpw, GWL_WNDPROC, (LONG)(ULONG_PTR)tpw_handoff_proc);
        InterlockedExchange(&g_handoff_tutorial, 1);
        g_handoff_t0 = GetTickCount();
    }

    resume_other_threads();
    log_msg("ck_menu: resumed retail threads");

    if (tpw && IsWindow(tpw)) {
        PostMessageA(tpw, CK_WM_TUTORIAL, 0, 0);
        SetForegroundWindow(tpw);
    } else {
        /* Fallback: call on this thread if hwnd gone. */
        retail_start_tutorial();
    }
    /* #region agent log */
    agent_log("H1", "ck_menu_host.cpp:handoff", "posted", "{}");
    /* #endregion */
    return 2;
}

void suspend_other_threads() {
    DWORD pid = GetCurrentProcessId();
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) {
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (th) {
                    SuspendThread(th);
                    CloseHandle(th);
                }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

void rgba_to_bgra(const ui::ImageRGBA& src, std::vector<uint8_t>& dst) {
    const size_t n = size_t(src.width) * size_t(src.height);
    dst.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        dst[i * 4 + 0] = src.pixels[i * 4 + 2];
        dst[i * 4 + 1] = src.pixels[i * 4 + 1];
        dst[i * 4 + 2] = src.pixels[i * 4 + 0];
        dst[i * 4 + 3] = 255;
    }
}

bool load_font(ui::Font& font, const std::string& root_win) {
    const std::string candidates[] = {
        root_win + "scripts\\tahomabd.ttf",
        root_win + "scripts\\tahoma.ttf",
        root_win + "tahomabd.ttf",
        root_win + "tahoma.ttf",
        "C:\\windows\\Fonts\\tahomabd.ttf",
        "C:\\windows\\Fonts\\tahoma.ttf",
    };
    for (const auto& p : candidates) {
        if (font.load_ttf(p, 16.0f))
            return true;
    }
    return false;
}

LRESULT CALLBACK menu_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        g_input.cx = (short)LOWORD(lp);
        g_input.cy = (short)HIWORD(lp);
        g_input.click_pending = 1;
        g_input.mouse_down = 1;
        SetCapture(hwnd);
        /* #region agent log */
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "{\"cx\":%d,\"cy\":%d}", g_input.cx, g_input.cy);
            agent_log("H-LOAD", "ck_menu_host.cpp:WM_LBUTTONDOWN", "btn", buf);
        }
        /* #endregion */
        return 0;
    }
    case WM_LBUTTONUP:
        g_input.cx = (short)LOWORD(lp);
        g_input.cy = (short)HIWORD(lp);
        g_input.mouse_down = 0;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        g_input.cx = (short)LOWORD(lp);
        g_input.cy = (short)HIWORD(lp);
        if (wp & MK_LBUTTON)
            g_input.mouse_down = 1;
        return 0;
    case WM_MOUSEWHEEL:
        g_input.wheel_y += (short)HIWORD(wp) / WHEEL_DELTA;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            g_input.esc = 1;
        else if (wp == VK_RETURN)
            g_input.enter = 1;
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

HWND create_menu_hwnd(HWND tpw) {
    RECT crc = {};
    GetClientRect(tpw, &crc);
    POINT tl = {crc.left, crc.top};
    POINT br = {crc.right, crc.bottom};
    ClientToScreen(tpw, &tl);
    ClientToScreen(tpw, &br);
    const int w = br.x - tl.x;
    const int h = br.y - tl.y;

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = menu_wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "CKNativeMenu";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(WS_EX_TOPMOST, "CKNativeMenu", "Imperivm 2 - Menu", WS_POPUP,
                                tl.x, tl.y, w > 0 ? w : 1024, h > 0 ? h : 768, NULL, NULL,
                                wc.hInstance, NULL);
    /* #region agent log */
    {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "{\"menu\":%lu,\"tpw\":%lu,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"err\":%lu}",
                 (unsigned long)(ULONG_PTR)hwnd, (unsigned long)(ULONG_PTR)tpw, (int)tl.x, (int)tl.y,
                 w, h, (unsigned long)GetLastError());
        agent_log("H-D", "ck_menu_host.cpp:create", "menu-hwnd", buf);
    }
    /* #endregion */
    /* Do not ShowWindow yet — retail must be suspended first or gamescope gets two live surfaces. */
    return hwnd;
}

void letterbox_geom(int win_w, int win_h, int mw, int mh, int* ox, int* oy, int* dw, int* dh) {
    *dw = win_w;
    *dh = win_h;
    *ox = 0;
    *oy = 0;
    if (win_w <= 0 || win_h <= 0 || mw <= 0 || mh <= 0)
        return;
    if (mw * win_h <= mh * win_w) {
        *dh = win_h;
        *dw = mw * win_h / mh;
    } else {
        *dw = win_w;
        *dh = mh * win_w / mw;
    }
    if (*dw < 1)
        *dw = 1;
    if (*dh < 1)
        *dh = 1;
    *ox = (win_w - *dw) / 2;
    *oy = (win_h - *dh) / 2;
}

}  // namespace

extern "C" int ck_menu_run(const char* game_root_win) {
    if (!game_root_win || !game_root_win[0]) {
        log_msg("ck_menu: empty game_root");
        return 0;
    }

    log_msg("ck_menu: RE MainMenu + LoadGame");
    /* #region agent log */
    agent_log("H-LOAD", "ck_menu_host.cpp:entry", "RE menu+load", "{}");
    /* #endregion */

    HWND tpw = wait_tpw_hwnd(20000);
    if (!tpw) {
        log_msg("ck_menu: no tpw HWND");
        return 0;
    }
    RECT crc = {};
    GetClientRect(tpw, &crc);
    log_msg("ck_menu: tpw hwnd=%p client=%dx%d", (void*)tpw, (int)(crc.right - crc.left),
            (int)(crc.bottom - crc.top));

    /* CreateWindow needs Wine loader_section — do it BEFORE suspend (deadlock if a
     * frozen thread holds the lock). Then suspend immediately so retail cannot open
     * a second surface (cutscene/EDS) during pak/font load. Show only after freeze. */
    /* #region agent log */
    agent_log("H-LCK", "ck_menu_host.cpp:entry", "pre-create", "{}");
    /* #endregion */
    g_menu_hwnd = create_menu_hwnd(tpw);
    if (!g_menu_hwnd) {
        log_msg("ck_menu: CreateWindow failed (%lu)", GetLastError());
        return 0;
    }
    log_msg("ck_menu: menu hwnd=%p (hidden)", (void*)g_menu_hwnd);
    /* #region agent log */
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"menu\":%lu}", (unsigned long)(ULONG_PTR)g_menu_hwnd);
        agent_log("H-LCK", "ck_menu_host.cpp:entry", "post-create", buf);
    }
    /* #endregion */

    /* #region agent log */
    agent_log("H-W2", "ck_menu_host.cpp:entry", "pre-suspend", "{}");
    /* #endregion */
    suspend_other_threads();
    log_msg("ck_menu: suspended other threads (after CreateWindow, before Show)");
    /* #region agent log */
    agent_log("H-W2", "ck_menu_host.cpp:entry", "post-suspend", "{}");
    /* #endregion */

    std::string root_win = game_root_win;
    if (!root_win.empty() && root_win.back() != '\\' && root_win.back() != '/')
        root_win.push_back('\\');
    const std::filesystem::path root_fs(root_win);

    vfs::MountTable mounts;
    mounts.mount_dir_paks(root_win + "Packs");
    mounts.mount_dir_paks(root_win + "Local");

    ui::Font font;
    if (!load_font(font, root_win))
        log_msg("ck_menu: WARN no TTF");

    ui::LocTable loc;
    {
        vfs::Blob blob;
        if (mounts.read("CURRENTLANG\\GAME.LOC.XML", blob))
            loc.load_xml(blob.data.data(), blob.data.size());
        else
            log_msg("ck_menu: WARN no GAME.LOC.XML");
    }

    ui::MainMenu menu;
    if (!menu.load(mounts, font)) {
        log_msg("ck_menu: MainMenu::load failed");
        DestroyWindow(g_menu_hwnd);
        g_menu_hwnd = NULL;
        return 0;
    }
    log_msg("ck_menu: MainMenu load ok");

    ui::LoadGameMenu load_game;
    if (!load_game.load(mounts, loc, font, root_fs))
        log_msg("ck_menu: WARN LoadGameMenu::load failed");
    else
        log_msg("ck_menu: LoadGameMenu ready");

    ui::Dialog dialog;
    /* #region agent log */
    agent_log("H-LOAD", "ck_menu_host.cpp:load", "LoadGame ready", "{}");
    /* #endregion */

    ShowWindow(g_menu_hwnd, SW_SHOW);
    UpdateWindow(g_menu_hwnd);
    SetForegroundWindow(g_menu_hwnd);
    SetFocus(g_menu_hwnd);
    /* #region agent log */
    {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"menu_vis\":%d,\"tpw_vis\":%d,\"menu\":%lu,\"tpw\":%lu}",
                 IsWindowVisible(g_menu_hwnd) ? 1 : 0, IsWindowVisible(tpw) ? 1 : 0,
                 (unsigned long)(ULONG_PTR)g_menu_hwnd, (unsigned long)(ULONG_PTR)tpw);
        agent_log("H-W2", "ck_menu_host.cpp:entry", "show-menu", buf);
    }
    /* #endregion */
    log_msg("ck_menu: showed menu (tpw still visible, not SW_HIDE)");

    std::vector<uint8_t> bgra;
    ui::ImageRGBA display;
    int quit = 0;
    int handoff = 0;
    g_input = {};

    auto open_ok = [&](const std::string& msg) {
        if (!dialog.open(mounts, loc, font, "OKMSGBOX", msg))
            log_msg("ck_menu: OKMSGBOX failed");
        else
            menu.set_overlay_mode(true);
    };

    while (!quit) {
        if (!IsWindow(g_menu_hwnd)) {
            log_msg("ck_menu: menu HWND destroyed");
            break;
        }

        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                quit = 1;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (quit)
            break;

        RECT rc;
        GetClientRect(g_menu_hwnd, &rc);
        const int win_w = rc.right - rc.left;
        const int win_h = rc.bottom - rc.top;
        const int mw = menu.width();
        const int mh = menu.height();
        int ox = 0, oy = 0, dw = win_w, dh = win_h;
        letterbox_geom(win_w, win_h, mw, mh, &ox, &oy, &dw, &dh);
        int mx = 0, my = 0;
        if (dw > 0 && dh > 0) {
            mx = (g_input.cx - ox) * mw / dw;
            my = (g_input.cy - oy) * mh / dh;
        }
        const bool clicked = g_input.click_pending != 0;
        const bool mouse_down = g_input.mouse_down != 0;
        const int wheel_y = g_input.wheel_y;
        const bool esc = g_input.esc != 0;
        const bool enter = g_input.enter != 0;
        g_input.click_pending = 0;
        g_input.wheel_y = 0;
        g_input.esc = 0;
        g_input.enter = 0;

        display = menu.frame();

        if (dialog.is_open()) {
            const auto dr = dialog.update(mx, my, clicked);
            dialog.draw(display);
            if (dr == ui::DialogResult::Closed || dr == ui::DialogResult::Accepted ||
                dr == ui::DialogResult::QuitApp || esc) {
                dialog.close();
                if (!load_game.is_open())
                    menu.set_overlay_mode(false);
            }
        } else if (load_game.is_open()) {
            const auto lr =
                load_game.update(mx, my, clicked, mouse_down, wheel_y, esc, enter);
            load_game.draw(display);
            if (lr == ui::LoadGameResult::Cancelled) {
                load_game.close();
                menu.set_overlay_mode(false);
                /* #region agent log */
                agent_log("H-LOAD", "ck_menu_host.cpp:loadgame", "cancelled", "{}");
                /* #endregion */
                log_msg("ck_menu: LoadGame cancelled");
            } else if (lr == ui::LoadGameResult::Load) {
                const std::string title = load_game.selected_title();
                const std::string path = load_game.selected_path();
                load_game.close();
                /* #region agent log */
                {
                    char buf[320];
                    snprintf(buf, sizeof(buf), "{\"title\":\"%.80s\",\"path\":\"%.180s\"}",
                             title.c_str(), path.c_str());
                    agent_log("H-LOAD", "ck_menu_host.cpp:loadgame", "load-selected", buf);
                }
                /* #endregion */
                log_msg("ck_menu: LoadGame selected '%s'", title.c_str());
                open_ok(loc.get("MainMenu Params:LoadGame", "Load game") + "\n\n" + title +
                        "\n\n(game load engine next)");
            }
        } else {
            const ui::MenuAction act = menu.update(mx, my, clicked);
            display = menu.frame();
            /* #region agent log */
            if (clicked) {
                char buf[200];
                snprintf(buf, sizeof(buf),
                         "{\"mx\":%d,\"my\":%d,\"act\":%d,\"ox\":%d,\"oy\":%d,\"dw\":%d,\"dh\":%d}",
                         mx, my, (int)act, ox, oy, dw, dh);
                agent_log("H-LOAD", "ck_menu_host.cpp:click", "click", buf);
                log_msg("ck_menu: action=%d click=(%d,%d) lb=%dx%d@%d,%d", (int)act, mx, my, dw, dh,
                        ox, oy);
            }
            /* #endregion */

            if (act == ui::MenuAction::Quit) {
                log_msg("ck_menu: Quit");
                quit = 1;
            } else if (act == ui::MenuAction::Tutorial) {
                log_msg("ck_menu: Tutorial → handoff");
                /* #region agent log */
                agent_log("H1", "ck_menu_host.cpp:action", "Tutorial", "{}");
                /* #endregion */
                const int hr = handoff_tutorial_to_retail(tpw);
                return hr;
            } else if (act == ui::MenuAction::LoadGame) {
                menu.set_overlay_mode(true);
                if (!load_game.open()) {
                    menu.set_overlay_mode(false);
                    open_ok(loc.get("MainMenu Params:LoadGame", "Load game") +
                            "\n\n[LOADGAME failed]");
                    log_msg("ck_menu: LoadGame open failed");
                    /* #region agent log */
                    agent_log("H-LOAD", "ck_menu_host.cpp:loadgame", "open-fail", "{}");
                    /* #endregion */
                } else {
                    log_msg("ck_menu: LoadGame opened");
                    /* #region agent log */
                    agent_log("H-LOAD", "ck_menu_host.cpp:loadgame", "opened", "{}");
                    /* #endregion */
                }
            } else if (act != ui::MenuAction::None) {
                log_msg("ck_menu: action=%d (submenu TODO)", (int)act);
            } else if (esc) {
                log_msg("ck_menu: Escape → quit");
                quit = 1;
            }
        }

        rgba_to_bgra(display, bgra);
        const int presented =
            vk_present_menu_frame(g_menu_hwnd, bgra.data(), display.width, display.height);
        /* #region agent log */
        {
            static int s_f;
            if (s_f++ < 3) {
                char buf[96];
                snprintf(buf, sizeof(buf), "{\"n\":%d,\"presented\":%d,\"lg\":%d,\"dlg\":%d}", s_f,
                         presented, load_game.is_open() ? 1 : 0, dialog.is_open() ? 1 : 0);
                agent_log("H-LOAD", "ck_menu_host.cpp:frame", "frame", buf);
            }
        }
        /* #endregion */

        Sleep(1);
    }

    if (g_menu_hwnd) {
        DestroyWindow(g_menu_hwnd);
        g_menu_hwnd = NULL;
    }
    if (handoff) {
        log_msg("ck_menu: leave after handoff (no ExitProcess)");
        return handoff;
    }
    log_msg("ck_menu: leave (ExitProcess 0)");
    ExitProcess(0);
    return 1;
}
