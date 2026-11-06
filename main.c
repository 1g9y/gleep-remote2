/* Gleep Remote - PS Vita client for GleepTV.
 * Protocol (matches GleepTV's RemoteServer on port 8410):
 *   - Connect TCP, send one command + '\n', read one reply line.
 *   - Commands: PAUSE MUTE CH+ CH- VHS TINNY MINI GUIDE REFRESH PING
 *               GETSET SETALL:k=v;k=v;...
 *   - Reply:    CH=1;TOTAL=5;NAME=...;PAUSED=0;VHS=1;TINNY=0;MINI=0
 *               SLD=k=v;k=v;...        (reply to GETSET)
 *               OK=SAVED N             (reply to SETALL)
 *               or ERR=... on failure.
 *
 * Controls:
 *   - Touch: tap buttons directly. On the settings screen, tap a slider
 *     bar to set that value directly, or tap the row to select it.
 *   - L / R shoulder: channel down / up (hold to repeat).
 *   - D-pad or left stick: move the green selection highlight. On the
 *     settings screen, up/down picks a slider, left/right adjusts it.
 *   - X: activate the highlighted button.
 *   - O (circle): back out of the settings screen without saving.
 *   - START: quit.
 *
 * Networking note: the socket stays NON-BLOCKING for its whole life.
 * We never rely on SO_NBIO=0 restoring blocking mode (that did not work
 * reliably and made the reply read return instantly with "would block",
 * so the status reply was lost and the LCD stayed at CH 00). Instead we
 * wait for readability with sceNetEpollWait, then recv -- data is
 * guaranteed ready when we read.
 *
 * Layout note: vita2d_init() creates a 960x544 framebuffer (the Vita's
 * native screen), so the UI is laid out at 960x544.
 */

#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/display.h>
#include <psp2/net/net.h>
#include <vita2d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "font.h"

#define SCREEN_W  960
#define SCREEN_H  544
#define PC_PORT   8410          /* VITA_REMOTE_PORT in GleepTV */

#define CONNECT_TIMEOUT_US (3 * 1000 * 1000)   /* 3s connect cap */
#define REPLY_TIMEOUT_US   (4 * 1000 * 1000)   /* 4s reply wait cap */

/* UI text: 4x-scaled 5x7 font = 20x28 px glyphs. */
#define UI_TEXT_SCALE 4.0f
#define UI_TEXT_H     28

/* Gamepad navigation timing (the main loop is vblank-synced, ~60fps). */
#define NAV_DEADZONE       40   /* stick deadzone, out of 128 */
#define NAV_DELAY_FRAMES   30   /* 0.5s before nav repeat kicks in */
#define NAV_REPEAT_FRAMES  12   /* nav repeat every 0.2s */
#define LR_DELAY_FRAMES    30   /* 0.5s before L/R repeat kicks in */
#define LR_REPEAT_FRAMES   18   /* L/R repeat every 0.3s */

#define COL_BG     RGBA8(8, 8, 8, 255)
#define COL_PANEL  RGBA8(16, 16, 16, 255)
#define COL_RED    RGBA8(255, 0, 60, 255)
#define COL_GREEN  RGBA8(0, 255, 120, 255)
#define COL_BTN    RGBA8(38, 38, 38, 255)
#define COL_TEXT   RGBA8(235, 235, 235, 255)

enum { SCR_IP = 0, SCR_REMOTE, SCR_SETTINGS };   /* [NEW] settings screen */

enum {
    ACT_PAUSE = 1, ACT_CHUP, ACT_CHDN, ACT_MUTE,   /* [NEW] ACT_MUTE */
    ACT_VHS, ACT_TINNY, ACT_MINI,
    ACT_GUIDE, ACT_SETTINGS, ACT_REFRESH, ACT_PING,
    ACT_DIGIT, ACT_DOT, ACT_DEL,
    ACT_CONNECT, ACT_REMOTE, ACT_IPSCREEN,
    ACT_SAVE, ACT_BACK                             /* [NEW] settings buttons */
};

typedef struct {
    int x, y, w, h;
    const char *label;
    int action;
    int digit;
} Button;

/* [NEW] ---- Settings sliders: must match REMOTE_SLIDER_KEYS order in
 * gleepTV.py exactly, or GETSET/SETALL values land on the wrong keys. */
#define SLIDER_COUNT 12
#define SLIDER_STEP  0.05f

static const char *slider_keys[SLIDER_COUNT] = {
    "vhs_intensity", "vhs_drift", "vhs_scanline_depth", "vhs_bloom",
    "vhs_tracking_bar", "vhs_aberration", "vhs_bleed", "vhs_grain",
    "vhs_scanlines", "vhs_white_lines_opacity",
    "vhs_white_lines_density", "vhs_static_opacity"
};

static const char *slider_labels[SLIDER_COUNT] = {
    "MASTER INTENSITY", "DRIFT / WOBBLE", "SCANLINE DEPTH", "BLOOM",
    "TRACKING BAR", "RGB ABERRATION", "COLOR BLEED", "FILM GRAIN",
    "OVERLAY SCANLINES", "WHITE LINES OPAC",
    "WHITE LINES DENS", "STATIC OPACITY"
};

static float g_vals[SLIDER_COUNT];      /* current slider values, 0.0-1.0 */
static int   g_vals_valid = 0;          /* set once GETSET has populated them */

/* [NEW] Settings screen geometry: 12 rows + SAVE/BACK buttons. */
#define SET_ROW_H   32
#define SET_TOP     64
#define SET_BAR_X   470
#define SET_BAR_W   340
#define SET_VAL_X   824
#define SET_ITEMS   (SLIDER_COUNT + 2)   /* sliders + SAVE + BACK */

static char g_ip[16] = "192.168.1.";
static int  g_screen = SCR_IP;
static char g_status[96] = "ENTER PC IP THEN TAP CONNECT";

/* Last reply from the server + parsed state for the LCD. */
static char g_reply[128] = "";
static int  g_ch = 0, g_total = 0, g_paused = 0;
static char g_name[40] = "NO SIGNAL";

/* Gamepad navigation state. */
static int g_sel[3] = {12, 0, 0};  /* [NEW] third slot: settings screen;
                                    * IP screen starts on CONNECT (12),
                                    * remote screen on PAUSE (0),
                                    * settings screen on first slider (0). */
static unsigned int g_prev_pad = 0;
static int g_nav_hold = 0;
static int g_lr_hold = 0;

static int t_min_x = 0, t_min_y = 0, t_span_x = 1, t_span_y = 1;

/* ---- IP screen: 4x3 keypad + connect/remote ---- */
static Button ip_buttons[] = {
    {216, 184, 120, 64, "1", ACT_DIGIT, 1},
    {352, 184, 120, 64, "2", ACT_DIGIT, 2},
    {488, 184, 120, 64, "3", ACT_DIGIT, 3},
    {624, 184, 120, 64, "4", ACT_DIGIT, 4},
    {216, 264, 120, 64, "5", ACT_DIGIT, 5},
    {352, 264, 120, 64, "6", ACT_DIGIT, 6},
    {488, 264, 120, 64, "7", ACT_DIGIT, 7},
    {624, 264, 120, 64, "8", ACT_DIGIT, 8},
    {216, 344, 120, 64, "9", ACT_DIGIT, 9},
    {352, 344, 120, 64, "0", ACT_DIGIT, 0},
    {488, 344, 120, 64, ".", ACT_DOT, 0},
    {624, 344, 120, 64, "DEL", ACT_DEL, 0},
    {232, 424, 240, 64, "CONNECT", ACT_CONNECT, 0},
    {488, 424, 240, 64, "REMOTE", ACT_REMOTE, 0},
};
#define IP_BTN_COUNT ((int)(sizeof(ip_buttons) / sizeof(ip_buttons[0])))
#define IP_SEL_DEFAULT 12   /* CONNECT */

/* ---- Remote screen: commands the GleepTV server actually handles.
 * [NEW] 4x3 grid with MUTE added next to PAUSE. ---- */
static Button remote_buttons[] = {
    {126, 200, 220, 72, "PAUSE",    ACT_PAUSE,    0},
    {370, 200, 220, 72, "MUTE",     ACT_MUTE,     0},   /* [NEW] */
    {614, 200, 220, 72, "CH +",     ACT_CHUP,     0},
    {126, 278, 220, 72, "CH -",     ACT_CHDN,     0},
    {370, 278, 220, 72, "VHS FX",   ACT_VHS,      0},
    {614, 278, 220, 72, "TINNY",    ACT_TINNY,    0},
    {126, 356, 220, 72, "MINI TV",  ACT_MINI,     0},
    {370, 356, 220, 72, "GUIDE",    ACT_GUIDE,    0},
    {614, 356, 220, 72, "SETTINGS", ACT_SETTINGS, 0},
    {126, 434, 220, 72, "REFRESH",  ACT_REFRESH,  0},
    {370, 434, 220, 72, "PING TEST",ACT_PING,     0},
    {614, 434, 220, 72, "IP/SET",   ACT_IPSCREEN, 0},
};
#define REMOTE_BTN_COUNT ((int)(sizeof(remote_buttons) / sizeof(remote_buttons[0])))
#define REMOTE_SEL_DEFAULT 0   /* PAUSE */

/* [NEW] ---- Settings screen: SAVE and BACK live at selection indices
 * SLIDER_COUNT and SLIDER_COUNT+1. ---- */
static Button settings_buttons[] = {
    {126, 456, 300, 60, "SAVE", ACT_SAVE, 0},
    {500, 456, 300, 60, "BACK", ACT_BACK, 0},
};
#define SET_BTN_COUNT ((int)(sizeof(settings_buttons) / sizeof(settings_buttons[0])))

static void set_status(const char *msg)
{
    snprintf(g_status, sizeof(g_status), "%s", msg);
}

static int valid_ip(const char *s)
{
    int a, b, c, d;
    char extra;
    return sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &extra) == 4
        && a >= 0 && a <= 255 && b >= 0 && b <= 255
        && c >= 0 && c <= 255 && d >= 0 && d <= 255;
}

static const char *cmd_for_action(int action)
{
    switch (action) {
    case ACT_PAUSE:    return "PAUSE";
    case ACT_MUTE:     return "MUTE";     /* [NEW] */
    case ACT_CHUP:     return "CH+";
    case ACT_CHDN:     return "CH-";
    case ACT_VHS:      return "VHS";
    case ACT_TINNY:    return "TINNY";
    case ACT_MINI:     return "MINI";
    case ACT_GUIDE:    return "GUIDE";
    case ACT_REFRESH:  return "REFRESH";
    case ACT_PING:     return "PING";
    default:           return NULL;
    }
}

/* Parse "CH=1;TOTAL=5;NAME=...;PAUSED=0;..." into the LCD globals. */
static void parse_reply(const char *r)
{
    const char *p;
    const char *e;
    size_t n;
    int v;

    if ((p = strstr(r, "CH=")) != NULL && sscanf(p + 3, "%d", &v) == 1)
        g_ch = v;
    if ((p = strstr(r, "TOTAL=")) != NULL && sscanf(p + 6, "%d", &v) == 1)
        g_total = v;
    if ((p = strstr(r, "PAUSED=")) != NULL && sscanf(p + 7, "%d", &v) == 1)
        g_paused = v;
    if ((p = strstr(r, "NAME=")) != NULL) {
        p += 5;
        e = strchr(p, ';');
        n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof(g_name))
            n = sizeof(g_name) - 1;
        memcpy(g_name, p, n);
        g_name[n] = '\0';
    }
}

/* [NEW] Parse "SLD=key=value;key=value;..." into g_vals.
 * Unknown keys are ignored; values are clamped to 0.0-1.0. */
static void parse_sld(const char *r)
{
    const char *p = r + 4;   /* skip "SLD=" */

    while (*p) {
        const char *semi = strchr(p, ';');
        size_t seg = semi ? (size_t)(semi - p) : strlen(p);
        char segbuf[64];
        const char *eq;
        int i;
        float v;

        if (seg >= sizeof(segbuf))
            seg = sizeof(segbuf) - 1;
        memcpy(segbuf, p, seg);
        segbuf[seg] = '\0';

        eq = strchr(segbuf, '=');
        if (eq != NULL) {
            *eq = '\0';
            if (sscanf(eq + 1, "%f", &v) == 1) {
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                for (i = 0; i < SLIDER_COUNT; i++) {
                    if (strcmp(segbuf, slider_keys[i]) == 0) {
                        g_vals[i] = v;
                        break;
                    }
                }
            }
        }

        if (!semi)
            break;
        p = semi + 1;
    }
    g_vals_valid = 1;
}

/* Connect to the PC, send one command, read the one-line reply.
 * The socket is non-blocking throughout; epoll governs all waiting so
 * nothing here can hang longer than the timeouts below, and the reply
 * is only read once epoll says it has arrived.
 * Returns 0 and fills g_reply on any reply; -1 with an ERR=... in
 * g_reply on failure.
 * [NEW] buffers enlarged: SLD replies carry all 12 slider values
 * (~380 bytes) and SETALL commands are ~370 bytes. */
static int send_command(const char *cmd)
{
    int sock = -1, eid = -1;
    SceNetSockaddrIn addr;
    SceNetEpollEvent ev;
    SceNetEpollEvent out[1];
    char line[512];
    char buf[512];
    int on = 1;
    int err = 0;
    unsigned int errlen = sizeof(err);
    int total = 0, len, ret;

    snprintf(g_reply, sizeof(g_reply), "ERR=NONE");

    if (!valid_ip(g_ip)) {
        snprintf(g_reply, sizeof(g_reply), "ERR=BAD IP");
        return -1;
    }

    sock = sceNetSocket("gleep", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (sock < 0) {
        snprintf(g_reply, sizeof(g_reply), "ERR=SOCKET");
        return -1;
    }

    /* Non-blocking for the whole session; we wait with epoll. */
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(PC_PORT);
    if (sceNetInetPton(SCE_NET_AF_INET, g_ip, &addr.sin_addr) <= 0) {
        snprintf(g_reply, sizeof(g_reply), "ERR=BAD IP");
        sceNetSocketClose(sock);
        return -1;
    }

    eid = sceNetEpollCreate("gleep", 0);
    if (eid < 0) {
        snprintf(g_reply, sizeof(g_reply), "ERR=EPOLL");
        sceNetSocketClose(sock);
        return -1;
    }

    ret = sceNetConnect(sock, (SceNetSockaddr *)&addr, sizeof(addr));
    if (ret < 0 && ret != SCE_NET_ERROR_EINPROGRESS) {
        snprintf(g_reply, sizeof(g_reply), "ERR=REFUSED");
        sceNetEpollDestroy(eid);
        sceNetSocketClose(sock);
        return -1;
    }

    if (ret == SCE_NET_ERROR_EINPROGRESS) {
        /* Wait for the connect to finish (socket becomes writable). */
        memset(&ev, 0, sizeof(ev));
        ev.events = SCE_NET_EPOLLOUT;
        ev.data.fd = sock;
        if (sceNetEpollControl(eid, SCE_NET_EPOLL_CTL_ADD, sock, &ev) < 0 ||
            sceNetEpollWait(eid, out, 1, CONNECT_TIMEOUT_US) <= 0) {
            snprintf(g_reply, sizeof(g_reply), "ERR=TIMEOUT");
            sceNetEpollDestroy(eid);
            sceNetSocketClose(sock);
            return -1;
        }
        sceNetGetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR,
                         &err, &errlen);
        if (err != 0) {
            snprintf(g_reply, sizeof(g_reply), "ERR=REFUSED");
            sceNetEpollDestroy(eid);
            sceNetSocketClose(sock);
            return -1;
        }
    }

    /* From here on we only ever wait for readability. */
    memset(&ev, 0, sizeof(ev));
    ev.events = SCE_NET_EPOLLIN;
    ev.data.fd = sock;
    if (sceNetEpollControl(eid, SCE_NET_EPOLL_CTL_MOD, sock, &ev) < 0) {
        snprintf(g_reply, sizeof(g_reply), "ERR=EPOLL");
        sceNetEpollDestroy(eid);
        sceNetSocketClose(sock);
        return -1;
    }

    snprintf(line, sizeof(line), "%s\n", cmd);
    if (sceNetSend(sock, line, (unsigned int)strlen(line), 0) < 0) {
        snprintf(g_reply, sizeof(g_reply), "ERR=SEND");
        sceNetEpollDestroy(eid);
        sceNetSocketClose(sock);
        return -1;
    }

    /* Wait for the reply, then read until '\n' (or the peer closes). */
    memset(buf, 0, sizeof(buf));
    while (total < (int)sizeof(buf) - 1) {
        if (sceNetEpollWait(eid, out, 1, REPLY_TIMEOUT_US) <= 0)
            break;   /* no reply within the timeout */
        len = sceNetRecv(sock, buf + total,
                         (unsigned int)(sizeof(buf) - 1 - total), 0);
        if (len < 0) {
            if (len == SCE_NET_ERROR_EWOULDBLOCK ||
                len == SCE_NET_ERROR_EAGAIN)
                continue;   /* spurious; wait again */
            break;          /* real error */
        }
        if (len == 0)
            break;          /* peer closed */
        total += len;
        if (memchr(buf, '\n', (size_t)total))
            break;
    }

    sceNetEpollDestroy(eid);
    sceNetSocketClose(sock);

    if (total <= 0) {
        snprintf(g_reply, sizeof(g_reply), "ERR=NO REPLY");
        return -1;
    }
    buf[total] = '\0';
    while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r'))
        buf[--total] = '\0';

    /* [NEW] SLD replies go to the slider values, not the LCD. */
    if (strncmp(buf, "SLD=", 4) == 0) {
        parse_sld(buf);
        snprintf(g_reply, sizeof(g_reply), "SLD LOADED");
        return 0;
    }

    snprintf(g_reply, sizeof(g_reply), "%s", buf);

    if (strncmp(buf, "ERR", 3) == 0)
        return -1;

    parse_reply(buf);
    return 0;
}

/* Channel step from L/R (and anything else that wants it). */
static void do_channel(int delta)
{
    send_command(delta > 0 ? "CH+" : "CH-");
    set_status(g_reply);
}

/* [NEW] Clamp a slider value into 0.0-1.0. */
static float clamp_val(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static void do_action(const Button *b)
{
    size_t len;

    switch (b->action) {
    case ACT_DIGIT:
        len = strlen(g_ip);
        if (g_screen == SCR_IP && len < 15) {
            g_ip[len] = (char)('0' + b->digit);
            g_ip[len + 1] = '\0';
        }
        break;
    case ACT_DOT:
        len = strlen(g_ip);
        if (g_screen == SCR_IP && len > 0 && len < 15 && g_ip[len - 1] != '.') {
            g_ip[len] = '.';
            g_ip[len + 1] = '\0';
        }
        break;
    case ACT_DEL:
        len = strlen(g_ip);
        if (len > 0)
            g_ip[len - 1] = '\0';
        break;
    case ACT_CONNECT:
        if (!valid_ip(g_ip)) {
            set_status("BAD IP ADDRESS");
            break;
        }
        g_screen = SCR_REMOTE;
        g_sel[SCR_REMOTE] = REMOTE_SEL_DEFAULT;
        /* PING verifies the link and fills the LCD; it has no side
         * effects on the server. The raw reply is shown as status. */
        send_command("PING");
        set_status(g_reply);
        break;
    case ACT_REMOTE:
        g_screen = SCR_REMOTE;
        g_sel[SCR_REMOTE] = REMOTE_SEL_DEFAULT;
        break;
    case ACT_IPSCREEN:
        g_screen = SCR_IP;
        g_sel[SCR_IP] = IP_SEL_DEFAULT;
        set_status("ENTER PC IP THEN TAP CONNECT");
        break;
    case ACT_SETTINGS:
        /* [NEW] SETTINGS now opens the Vita-side slider editor instead of
         * poking the PC's own settings dialog. GETSET fills the sliders
         * with the server's current values. */
        g_screen = SCR_SETTINGS;
        g_sel[SCR_SETTINGS] = 0;
        send_command("GETSET");
        set_status(g_vals_valid ? "SLIDERS LOADED - ADJUST THEN SAVE"
                                : g_reply);
        break;
    case ACT_SAVE: {
        /* [NEW] Push all 12 sliders back with SETALL. The server saves,
         * regenerates the shader, and restarts the stream. */
        char setbuf[512];
        int i, n;
        n = snprintf(setbuf, sizeof(setbuf), "SETALL:");
        for (i = 0; i < SLIDER_COUNT && n > 0 && n < (int)sizeof(setbuf); i++)
            n += snprintf(setbuf + n, sizeof(setbuf) - n,
                          "%s=%.3f;", slider_keys[i], g_vals[i]);
        send_command(setbuf);
        set_status(g_reply);
        if (strncmp(g_reply, "OK", 2) == 0) {
            g_screen = SCR_REMOTE;
            g_sel[SCR_REMOTE] = REMOTE_SEL_DEFAULT;
        }
        break;
    }
    case ACT_BACK:
        /* [NEW] Leave the settings screen without saving. */
        g_screen = SCR_REMOTE;
        g_sel[SCR_REMOTE] = REMOTE_SEL_DEFAULT;
        set_status("SETTINGS NOT SAVED");
        break;
    default: {
        const char *cmd = cmd_for_action(b->action);
        if (!cmd)
            break;
        send_command(cmd);
        set_status(g_reply);
        break;
    }
    }
}

/* [NEW] Hit-testing for the settings screen: slider rows first (tap on
 * the bar sets the value directly, tap elsewhere in the row selects),
 * then SAVE/BACK. */
static void handle_settings_tap(int x, int y)
{
    int i;

    for (i = 0; i < SLIDER_COUNT; i++) {
        int ry = SET_TOP + i * SET_ROW_H;
        if (y >= ry && y < ry + SET_ROW_H) {
            g_sel[SCR_SETTINGS] = i;
            if (x >= SET_BAR_X && x <= SET_BAR_X + SET_BAR_W) {
                g_vals[i] = clamp_val(
                    (float)(x - SET_BAR_X) / (float)SET_BAR_W);
            }
            return;
        }
    }

    for (i = 0; i < SET_BTN_COUNT; i++) {
        const Button *b = &settings_buttons[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
            do_action(b);
            return;
        }
    }
}

static void handle_tap(int x, int y)
{
    const Button *btns;
    int count;
    int i;

    if (g_screen == SCR_SETTINGS) {
        handle_settings_tap(x, y);
        return;
    }

    btns = (g_screen == SCR_IP) ? ip_buttons : remote_buttons;
    count = (g_screen == SCR_IP) ? IP_BTN_COUNT : REMOTE_BTN_COUNT;

    for (i = 0; i < count; i++) {
        const Button *b = &btns[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
            g_sel[g_screen] = i;   /* keep highlight in sync with touch */
            do_action(b);
            return;
        }
    }
}

/* Move the selection one "step" in direction (dx, dy), each -1..1.
 * Picks the nearest button center in that direction, penalizing
 * sideways offset so grids behave sensibly. */
static void nav_move(int screen, int dx, int dy)
{
    const Button *btns = (screen == SCR_IP) ? ip_buttons : remote_buttons;
    int count = (screen == SCR_IP) ? IP_BTN_COUNT : REMOTE_BTN_COUNT;
    int cur = g_sel[screen];
    int cx = btns[cur].x + btns[cur].w / 2;
    int cy = btns[cur].y + btns[cur].h / 2;
    int best = -1, best_score = 0;
    int i;

    for (i = 0; i < count; i++) {
        int nx = btns[i].x + btns[i].w / 2;
        int ny = btns[i].y + btns[i].h / 2;
        int ox = nx - cx, oy = ny - cy;
        int primary, perp, score;

        if (i == cur)
            continue;
        if (dx != 0) {
            if (ox * dx <= 8)       /* must be genuinely to the side */
                continue;
            primary = ox * dx;
            perp = oy > 0 ? oy : -oy;
        } else {
            if (oy * dy <= 8)       /* must be genuinely above/below */
                continue;
            primary = oy * dy;
            perp = ox > 0 ? ox : -ox;
        }
        score = primary + 3 * perp;
        if (best < 0 || score < best_score) {
            best = i;
            best_score = score;
        }
    }
    if (best >= 0)
        g_sel[screen] = best;
}

/* Current nav direction from d-pad or left stick: 0 left, 1 right,
 * 2 up, 3 down, -1 none. Stick uses a deadzone + dominant axis. */
static int nav_direction(const SceCtrlData *pad)
{
    int dx = (int)pad->lx - 128;
    int dy = (int)pad->ly - 128;
    int ax = dx > 0 ? dx : -dx;
    int ay = dy > 0 ? dy : -dy;

    if (pad->buttons & SCE_CTRL_LEFT)  return 0;
    if (pad->buttons & SCE_CTRL_RIGHT) return 1;
    if (pad->buttons & SCE_CTRL_UP)    return 2;
    if (pad->buttons & SCE_CTRL_DOWN)  return 3;

    if (ax < NAV_DEADZONE && ay < NAV_DEADZONE)
        return -1;
    if (ax >= ay)
        return dx > 0 ? 1 : 0;
    return dy > 0 ? 3 : 2;
}

static void touch_calibrate(void)
{
    SceTouchPanelInfo pinfo;

    /* REQUIRED: the panel defaults to not sampling. Without this,
     * sceTouchPeek never returns any reports. */
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                             SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchEnableTouchforce(SCE_TOUCH_PORT_FRONT);

    memset(&pinfo, 0, sizeof(pinfo));
    sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &pinfo);
    t_min_x = (int)pinfo.minAaX;
    t_min_y = (int)pinfo.minAaY;
    t_span_x = (int)pinfo.maxAaX - (int)pinfo.minAaX;
    t_span_y = (int)pinfo.maxAaY - (int)pinfo.minAaY;
    if (t_span_x <= 0) t_span_x = 1;
    if (t_span_y <= 0) t_span_y = 1;
}

static void touch_to_screen(int rx, int ry, int *sx, int *sy)
{
    *sx = (rx - t_min_x) * SCREEN_W / t_span_x;
    *sy = (ry - t_min_y) * SCREEN_H / t_span_y;
}

static void draw_button_sel(const Button *b, const char *label, int selected)
{
    int tw;
    vita2d_draw_rectangle((float)b->x, (float)b->y, (float)b->w, (float)b->h,
                          selected ? COL_GREEN : COL_RED);
    vita2d_draw_rectangle((float)b->x + 4, (float)b->y + 4,
                          (float)b->w - 8, (float)b->h - 8, COL_BTN);
    tw = text_width(UI_TEXT_SCALE, label);
    draw_text(UI_TEXT_SCALE, b->x + (b->w - tw) / 2, b->y + (b->h - UI_TEXT_H) / 2,
              selected ? COL_GREEN : COL_TEXT, label);
}

static void draw_ip_screen(void)
{
    char buf[24];
    int tw;
    int i;

    draw_text(5.0f, 32, 48, COL_RED, "GLEEP REMOTE");

    vita2d_draw_rectangle(216, 104, 528, 60, COL_RED);
    vita2d_draw_rectangle(220, 108, 520, 52, COL_PANEL);
    snprintf(buf, sizeof(buf), "%s_", g_ip);
    tw = text_width(UI_TEXT_SCALE, buf);
    draw_text(UI_TEXT_SCALE, 216 + (528 - tw) / 2, 120, COL_GREEN, buf);

    for (i = 0; i < IP_BTN_COUNT; i++)
        draw_button_sel(&ip_buttons[i], ip_buttons[i].label,
                        i == g_sel[SCR_IP]);
}

static void draw_remote_screen(void)
{
    char l1[48];
    int i;

    draw_text(5.0f, 32, 24, COL_RED, "GLEEP REMOTE");

    /* LCD panel mirroring the server's status reply. */
    vita2d_draw_rectangle(126, 88, 708, 96, COL_RED);
    vita2d_draw_rectangle(130, 92, 700, 88, COL_PANEL);
    snprintf(l1, sizeof(l1), "CH %02d/%02d  %s",
             g_ch, g_total, g_paused ? "PAUSED" : "PLAYING");
    draw_text(3.0f, 146, 104, COL_GREEN, l1);
    draw_text(2.0f, 146, 144, COL_GREEN, g_name);

    for (i = 0; i < REMOTE_BTN_COUNT; i++) {
        const Button *b = &remote_buttons[i];
        /* The pause button mirrors the server's state, like the PC remote. */
        const char *lab = (b->action == ACT_PAUSE)
                          ? (g_paused ? "PLAY" : "PAUSE") : b->label;
        draw_button_sel(b, lab, i == g_sel[SCR_REMOTE]);
    }
}

/* [NEW] Settings screen: 12 slider rows + SAVE/BACK. The selected row
 * gets a green border; the filled part of each bar is the value. */
static void draw_settings_screen(void)
{
    int i;

    draw_text(5.0f, 32, 16, COL_RED, "VHS SETTINGS");
    draw_text(2.0f, 480, 36, COL_GREEN,
              "UP/DOWN SELECT  LEFT/RIGHT ADJUST");

    for (i = 0; i < SLIDER_COUNT; i++) {
        int ry = SET_TOP + i * SET_ROW_H;
        int sel = (i == g_sel[SCR_SETTINGS]);
        char val[8];

        draw_text(2.0f, 16, ry + 9, sel ? COL_GREEN : COL_TEXT,
                  slider_labels[i]);

        if (sel)   /* green border behind the bar for the selected row */
            vita2d_draw_rectangle(SET_BAR_X - 2, ry + 5, SET_BAR_W + 4, 22,
                                  COL_GREEN);
        vita2d_draw_rectangle(SET_BAR_X, ry + 7, SET_BAR_W, 18, COL_BTN);
        vita2d_draw_rectangle(SET_BAR_X, ry + 7,
                              (int)(SET_BAR_W * g_vals[i]), 18, COL_RED);

        snprintf(val, sizeof(val), "%.2f", g_vals[i]);
        draw_text(2.0f, SET_VAL_X, ry + 9, COL_GREEN, val);
    }

    draw_button_sel(&settings_buttons[0], "SAVE",
                    g_sel[SCR_SETTINGS] == SLIDER_COUNT);
    draw_button_sel(&settings_buttons[1], "BACK",
                    g_sel[SCR_SETTINGS] == SLIDER_COUNT + 1);

    if (!g_vals_valid)
        draw_text(2.0f, 640, 476, COL_TEXT, "NO DATA - REOPEN");
}

static void init_net(void)
{
    SceNetInitParam param;

    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0)
        return;

    memset(&param, 0, sizeof(param));
    param.memory = malloc(1 * 1024 * 1024);
    if (!param.memory)
        return;
    param.size = 1 * 1024 * 1024;
    param.flags = 0;
    sceNetInit(&param);
}

int main(void)
{
    SceCtrlData pad;
    int prev_touch = 0;
    int last_x = 0, last_y = 0;

    init_net();

    vita2d_init();
    vita2d_set_clear_color(COL_BG);
    touch_calibrate();

    memset(&pad, 0, sizeof(pad));

    for (;;) {
        SceTouchData td;
        int touched = 0;
        int tx = 0, ty = 0;
        int pressed;
        int dir;

        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START)
            break;

        pressed = (int)(pad.buttons & ~g_prev_pad);

        /* L / R shoulder: channel down / up, with hold-to-repeat. */
        if (pressed & SCE_CTRL_RTRIGGER) {
            do_channel(+1);
            g_lr_hold = 0;
        } else if (pressed & SCE_CTRL_LTRIGGER) {
            do_channel(-1);
            g_lr_hold = 0;
        } else if (pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
            g_lr_hold++;
            if (g_lr_hold > LR_DELAY_FRAMES &&
                ((g_lr_hold - LR_DELAY_FRAMES) % LR_REPEAT_FRAMES) == 0)
                do_channel((pad.buttons & SCE_CTRL_RTRIGGER) ? +1 : -1);
        } else {
            g_lr_hold = 0;
        }

        /* D-pad / left stick: move the selection, with repeat.
         * [NEW] On the settings screen, up/down picks a slider (or SAVE/
         * BACK) and left/right nudges the selected value by 0.05. */
        dir = nav_direction(&pad);
        if (dir >= 0) {
            static const int ndx[4] = {-1, 1, 0, 0};
            static const int ndy[4] = {0, 0, -1, 1};
            g_nav_hold++;
            if (g_nav_hold == 1 ||
                (g_nav_hold > NAV_DELAY_FRAMES &&
                 ((g_nav_hold - NAV_DELAY_FRAMES) % NAV_REPEAT_FRAMES) == 0)) {
                if (g_screen == SCR_SETTINGS) {
                    int s = g_sel[SCR_SETTINGS];
                    if (dir == 2 && s > 0)
                        g_sel[SCR_SETTINGS] = s - 1;
                    else if (dir == 3 && s < SET_ITEMS - 1)
                        g_sel[SCR_SETTINGS] = s + 1;
                    else if (dir == 0 && s < SLIDER_COUNT)
                        g_vals[s] = clamp_val(g_vals[s] - SLIDER_STEP);
                    else if (dir == 1 && s < SLIDER_COUNT)
                        g_vals[s] = clamp_val(g_vals[s] + SLIDER_STEP);
                } else {
                    nav_move(g_screen, ndx[dir], ndy[dir]);
                }
            }
        } else {
            g_nav_hold = 0;
        }

        /* X: activate the highlighted button. [NEW] On the settings screen
         * X only fires SAVE/BACK; sliders are adjusted with left/right. */
        if (pressed & SCE_CTRL_CROSS) {
            if (g_screen == SCR_SETTINGS) {
                int s = g_sel[SCR_SETTINGS];
                if (s >= SLIDER_COUNT)
                    do_action(&settings_buttons[s - SLIDER_COUNT]);
            } else {
                const Button *btns = (g_screen == SCR_IP) ? ip_buttons
                                                          : remote_buttons;
                do_action(&btns[g_sel[g_screen]]);
            }
        }

        /* [NEW] O (circle): back out of settings without saving. */
        if ((pressed & SCE_CTRL_CIRCLE) && g_screen == SCR_SETTINGS) {
            g_screen = SCR_REMOTE;
            g_sel[SCR_REMOTE] = REMOTE_SEL_DEFAULT;
            set_status("SETTINGS NOT SAVED");
        }

        g_prev_pad = pad.buttons;

        memset(&td, 0, sizeof(td));
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) > 0 && td.reportNum > 0) {
            touched = 1;
            touch_to_screen((int)td.report[0].x, (int)td.report[0].y, &tx, &ty);
            last_x = tx;
            last_y = ty;
        }

        /* Fire on finger release, using the last known position. */
        if (prev_touch && !touched)
            handle_tap(last_x, last_y);
        prev_touch = touched;

        vita2d_start_drawing();
        vita2d_clear_screen();

        if (g_screen == SCR_IP)
            draw_ip_screen();
        else if (g_screen == SCR_SETTINGS)
            draw_settings_screen();
        else
            draw_remote_screen();

        /* Raw last reply from the PC -- the ground truth for debugging. */
        draw_text(2.0f, 16, 524, COL_GREEN, g_status);

        vita2d_end_drawing();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();
    }

    vita2d_fini();
    sceNetTerm();
    sceKernelExitProcess(0);
    return 0;
}
