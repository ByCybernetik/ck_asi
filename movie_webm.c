/*
 * WebM cutscenes: own decode (Linux ck_webm_dec over TCP) → Vulkan present + waveOut audio.
 * Avoids Wine DirectDraw / StretchDIBits paths for VP9.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include "movie_player.h"
#include "vk_present.h"
#include "log.h"
#include "hitch.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CK_WEBM_DEC_PORT
#define CK_WEBM_DEC_PORT 38471
#endif

static void webm_agent(const char *hid, const char *msg, const char *data_json)
{
    (void)hid;
    (void)msg;
    (void)data_json;
}

static int g_wsa_ready;

static int ensure_wsa(void)
{
    WSADATA wsa;
    if (g_wsa_ready)
        return 1;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;
    g_wsa_ready = 1;
    return 1;
}

/* Z:\home\foo\bar → /home/foo/bar ; already-unix paths pass through. */
static void path_to_unix(const char *in, char *out, size_t out_sz)
{
    size_t i = 0;
    const char *p = in;
    if (!in || !out || out_sz < 2)
        return;
    if ((p[0] == 'Z' || p[0] == 'z') && p[1] == ':') {
        p += 2;
        while (*p == '\\' || *p == '/')
            p++;
        out[i++] = '/';
        for (; *p && i + 1 < out_sz; ++p)
            out[i++] = (*p == '\\') ? '/' : *p;
        out[i] = 0;
        return;
    }
    strncpy(out, in, out_sz - 1);
    out[out_sz - 1] = 0;
    for (i = 0; out[i]; ++i)
        if (out[i] == '\\')
            out[i] = '/';
}

/*
 * Same policy as AVI StretchDIBits pump: ESC / Enter / LMB only.
 * Ignore stale clicks/keys from previous intros for a short grace period —
 * otherwise Intro3 starts with frames=0 ("webm skip after 0 frames").
 */
static int user_wants_skip(DWORD grace_until_tick)
{
    MSG msg;
    DWORD now = GetTickCount();
    int in_grace = (now < grace_until_tick);

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return 1;
        if (!in_grace) {
            if (msg.message == WM_KEYDOWN &&
                (msg.wParam == VK_ESCAPE || msg.wParam == VK_RETURN)) {
                /* #region agent log */
                webm_agent("H95", "webm-own-skip", "{\"why\":\"keydown\"}");
                /* #endregion */
                return 1;
            }
            if (msg.message == WM_LBUTTONDOWN) {
                /* #region agent log */
                webm_agent("H95", "webm-own-skip", "{\"why\":\"lmb\"}");
                /* #endregion */
                return 1;
            }
        }
        /* Always dispatch non-skip traffic so the game queue stays healthy. */
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (!in_grace && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
        /* #region agent log */
        webm_agent("H95", "webm-own-skip", "{\"why\":\"async-esc\"}");
        /* #endregion */
        return 1;
    }
    return 0;
}

static int recv_all(SOCKET s, void *buf, int n)
{
    char *p = (char *)buf;
    while (n > 0) {
        int r = recv(s, p, n, 0);
        if (r <= 0)
            return -1;
        p += r;
        n -= r;
    }
    return 0;
}

static int send_all(SOCKET s, const void *buf, int n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        int w = send(s, p, n, 0);
        if (w <= 0)
            return -1;
        p += w;
        n -= w;
    }
    return 0;
}

enum { WAVE_BUFS = 8, WAVE_BUF_BYTES = 48000 * 2 * 2 / 4 }; /* ~250ms at 48k stereo */

struct wave_q {
    HWAVEOUT out;
    WAVEHDR hdrs[WAVE_BUFS];
    BYTE *mem[WAVE_BUFS];
    int next;
    int ready;
};

static void wave_q_close(struct wave_q *q)
{
    int i;
    if (!q->ready)
        return;
    waveOutReset(q->out);
    for (i = 0; i < WAVE_BUFS; ++i) {
        if (q->hdrs[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(q->out, &q->hdrs[i], sizeof(WAVEHDR));
        free(q->mem[i]);
        q->mem[i] = NULL;
    }
    waveOutClose(q->out);
    q->ready = 0;
}

static int wave_q_open(struct wave_q *q, int rate, int ch)
{
    WAVEFORMATEX fmt;
    int i;
    memset(q, 0, sizeof(*q));
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = (WORD)ch;
    fmt.nSamplesPerSec = (DWORD)rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(ch * 2);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    if (waveOutOpen(&q->out, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return 0;
    for (i = 0; i < WAVE_BUFS; ++i) {
        q->mem[i] = (BYTE *)malloc(WAVE_BUF_BYTES);
        if (!q->mem[i]) {
            wave_q_close(q);
            return 0;
        }
        memset(&q->hdrs[i], 0, sizeof(WAVEHDR));
        q->hdrs[i].lpData = (LPSTR)q->mem[i];
        q->hdrs[i].dwBufferLength = WAVE_BUF_BYTES;
        if (waveOutPrepareHeader(q->out, &q->hdrs[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            wave_q_close(q);
            return 0;
        }
    }
    q->ready = 1;
    return 1;
}

static void wave_q_write(struct wave_q *q, const BYTE *data, int n)
{
    WAVEHDR *h;
    if (!q->ready || n <= 0)
        return;
    if (n > WAVE_BUF_BYTES)
        n = WAVE_BUF_BYTES;
    h = &q->hdrs[q->next];
    while (h->dwFlags & WHDR_INQUEUE)
        Sleep(1);
    memcpy(q->mem[q->next], data, (size_t)n);
    h->dwBufferLength = (DWORD)n;
    h->dwBytesRecorded = (DWORD)n;
    waveOutWrite(q->out, h, sizeof(WAVEHDR));
    q->next = (q->next + 1) % WAVE_BUFS;
}

static SOCKET connect_dec(int port, int *out_wsa)
{
    SOCKET s;
    struct sockaddr_in addr;
    DWORD to;

    if (out_wsa)
        *out_wsa = 0;
    if (!ensure_wsa())
        return INVALID_SOCKET;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (out_wsa)
            *out_wsa = (int)WSAGetLastError();
        return INVALID_SOCKET;
    }
    to = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&to, sizeof(to));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (out_wsa)
            *out_wsa = (int)WSAGetLastError();
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

/* Wine: start.exe /unix launches host ELF (same present path as AVI → vk_present_movie_frame). */
static int spawn_webm_dec(DWORD port)
{
    char bin[MAX_PATH];
    char cmdline[MAX_PATH + 96];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD n;

    n = GetEnvironmentVariableA("CK_WEBM_DEC", bin, sizeof(bin));
    if (n == 0 || n >= sizeof(bin))
        strncpy(bin, "/home/cybernetik/Games/Imperivm/ck_asi/tools/ck_webm_dec", sizeof(bin) - 1);
    bin[sizeof(bin) - 1] = 0;

    snprintf(cmdline, sizeof(cmdline), "start.exe /unix %s --daemon --port %lu", bin,
             (unsigned long)port);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        log_msg("movie: spawn ck_webm_dec failed err=%lu cmd='%s'", (unsigned long)GetLastError(),
                cmdline);
        /* #region agent log */
        {
            char js[128];
            snprintf(js, sizeof(js), "{\"ok\":0,\"err\":%lu}", (unsigned long)GetLastError());
            webm_agent("H91", "webm-own-spawn", js);
        }
        /* #endregion */
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log_msg("movie: spawned ck_webm_dec via start /unix");
    /* #region agent log */
    webm_agent("H91", "webm-own-spawn", "{\"ok\":1}");
    /* #endregion */
    return 1;
}

static SOCKET connect_dec_retry(int port, int *out_wsa, int *spawned)
{
    SOCKET s;
    int wsa = 0;
    int attempt;

    if (spawned)
        *spawned = 0;
    s = connect_dec(port, &wsa);
    if (s != INVALID_SOCKET) {
        if (out_wsa)
            *out_wsa = 0;
        return s;
    }
    if (spawn_webm_dec((DWORD)port)) {
        if (spawned)
            *spawned = 1;
        for (attempt = 0; attempt < 25; ++attempt) {
            Sleep(100);
            s = connect_dec(port, &wsa);
            if (s != INVALID_SOCKET) {
                if (out_wsa)
                    *out_wsa = 0;
                return s;
            }
        }
    }
    if (out_wsa)
        *out_wsa = wsa;
    return INVALID_SOCKET;
}

int play_webm_own_vulkan(const char *path_a, HWND hwnd)
{
    char unix_path[MAX_PATH * 2];
    char cmd[MAX_PATH * 2 + 16];
    char line[256];
    SOCKET s;
    int w = 0, h = 0, fps_n = 24, fps_d = 1, ar = 48000, ch = 2;
    int frames = 0, audio_pkts = 0;
    BYTE *payload = NULL;
    uint32_t cap = 0;
    struct wave_q wave;
    LARGE_INTEGER freq, t0, now;
    double frame_dur_ms;
    int i;
    DWORD port = CK_WEBM_DEC_PORT;
    char envport[32];
    int wsa_err = 0, spawned = 0;

    memset(&wave, 0, sizeof(wave));
    if (!path_a || !path_a[0])
        return 0;

    if (GetEnvironmentVariableA("CK_WEBM_DEC_PORT", envport, sizeof(envport)) > 0)
        port = (DWORD)atoi(envport);

    path_to_unix(path_a, unix_path, sizeof(unix_path));
    log_msg("movie: webm own-decode → Vulkan '%s' (unix '%s') port=%lu", path_a, unix_path,
            (unsigned long)port);
    /* #region agent log */
    {
        char js[360];
        snprintf(js, sizeof(js), "{\"port\":%lu,\"path\":\"%.200s\"}", (unsigned long)port,
                 unix_path);
        webm_agent("H90", "webm-own-connect", js);
    }
    /* #endregion */

    s = connect_dec_retry((int)port, &wsa_err, &spawned);
    if (s == INVALID_SOCKET) {
        log_msg("movie: ck_webm_dec not reachable 127.0.0.1:%lu wsa=%d spawned=%d",
                (unsigned long)port, wsa_err, spawned);
        /* #region agent log */
        {
            char js[96];
            snprintf(js, sizeof(js), "{\"ok\":0,\"wsa\":%d,\"spawned\":%d}", wsa_err, spawned);
            webm_agent("H90", "webm-own-connect-fail", js);
        }
        /* #endregion */
        return 0;
    }
    /* #region agent log */
    {
        char js[64];
        snprintf(js, sizeof(js), "{\"ok\":1,\"spawned\":%d}", spawned);
        webm_agent("H90", "webm-own-connected", js);
    }
    /* #endregion */

    snprintf(cmd, sizeof(cmd), "PLAY %s\n", unix_path);
    if (send_all(s, cmd, (int)strlen(cmd)) < 0) {
        closesocket(s);
        return 0;
    }

    /* read HDR line */
    i = 0;
    while (i + 1 < (int)sizeof(line)) {
        char c;
        if (recv_all(s, &c, 1) < 0) {
            closesocket(s);
            return 0;
        }
        if (c == '\n')
            break;
        line[i++] = c;
    }
    line[i] = 0;
    if (sscanf(line, "HDR %d %d %d %d %d %d", &w, &h, &fps_n, &fps_d, &ar, &ch) != 6 || w < 2 ||
        h < 2) {
        log_msg("movie: bad HDR from decoder: '%s'", line);
        webm_agent("H90", "webm-own-bad-hdr", "{}");
        closesocket(s);
        return 0;
    }
    if (fps_n <= 0 || fps_d <= 0) {
        fps_n = 24;
        fps_d = 1;
    }
    frame_dur_ms = 1000.0 * (double)fps_d / (double)fps_n;
    log_msg("movie: webm HDR %dx%d fps=%d/%d audio=%dHz %dch", w, h, fps_n, fps_d, ar, ch);
    /* #region agent log */
    {
        char js[160];
        snprintf(js, sizeof(js),
                 "{\"w\":%d,\"h\":%d,\"fps_n\":%d,\"fps_d\":%d,\"ar\":%d,\"ch\":%d}", w, h, fps_n,
                 fps_d, ar, ch);
        webm_agent("H90", "webm-own-hdr", js);
    }
    /* #endregion */

    if (ar > 0 && ch > 0)
        wave_q_open(&wave, ar, ch);

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    {
        DWORD grace_until = GetTickCount() + 500;
        MSG msg;
        /* Discard leftover intro-skip input without aborting this movie. */
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        for (;;) {
            uint8_t type;
            uint32_t size = 0;
            uint8_t sh[4];

            if (user_wants_skip(grace_until)) {
                log_msg("movie: webm skip after %d frames", frames);
                break;
            }
            if (recv_all(s, &type, 1) < 0)
                break;
            if (recv_all(s, sh, 4) < 0)
                break;
            size = (uint32_t)sh[0] | ((uint32_t)sh[1] << 8) | ((uint32_t)sh[2] << 16) |
                   ((uint32_t)sh[3] << 24);
            if (size > 64u * 1024u * 1024u)
                break;
            if (size > cap) {
                BYTE *nbuf = (BYTE *)realloc(payload, size ? size : 1);
                if (!nbuf)
                    break;
                payload = nbuf;
                cap = size;
            }
            if (size && recv_all(s, payload, (int)size) < 0)
                break;

            if (type == 'E')
                break;
            if (type == '!') {
                log_msg("movie: decoder error: %.*s", (int)size, payload ? (char *)payload : "");
                /* #region agent log */
                {
                    char js[96];
                    snprintf(js, sizeof(js), "{\"err\":\"%.*s\"}", size > 40 ? 40 : (int)size,
                             payload ? (char *)payload : "");
                    webm_agent("H94", "webm-own-dec-err", js);
                }
                /* #endregion */
                break;
            }
            if (type == 'A' && size > 0) {
                if (frames == 0 && audio_pkts == 0) {
                    /* #region agent log */
                    webm_agent("H94", "webm-own-first-pkt", "{\"type\":\"A\"}");
                    /* #endregion */
                }
                wave_q_write(&wave, payload, (int)size);
                audio_pkts++;
                continue;
            }
            if (type == 'V' && size >= (uint32_t)w * (uint32_t)h * 4u) {
                if (frames == 0) {
                    /* #region agent log */
                    webm_agent("H94", "webm-own-first-pkt", "{\"type\":\"V\"}");
                    /* #endregion */
                }
                vk_present_movie_frame(hwnd, payload, w, h);
                frames++;
                /* pace to container fps */
                {
                    double target_ms = (double)frames * frame_dur_ms;
                    double elapsed_ms;
                    QueryPerformanceCounter(&now);
                    elapsed_ms =
                        1000.0 * (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
                    if (target_ms > elapsed_ms + 1.0)
                        Sleep((DWORD)(target_ms - elapsed_ms));
                }
            }
        }
    }

    closesocket(s);
    free(payload);
    Sleep(100);
    wave_q_close(&wave);

    log_msg("movie: webm own-decode done frames=%d audio_pkts=%d", frames, audio_pkts);
    /* #region agent log */
    {
        char js[96];
        snprintf(js, sizeof(js), "{\"frames\":%d,\"audio_pkts\":%d,\"ok\":%d}", frames, audio_pkts,
                 frames > 0 ? 1 : 0);
        webm_agent("H90", "webm-own-done", js);
    }
    /* #endregion */
    return frames > 0 ? 1 : 0;
}
