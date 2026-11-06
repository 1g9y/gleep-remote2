/* Gleep Remote - PS Vita client for GleepTV.
 * Protocol (matches GleepTV's RemoteServer on port 8410):
 *   - Connect TCP, send one command + '\n', read one reply line.
 *   - Commands: PAUSE CH+ CH- VHS TINNY MINI GUIDE SETTINGS REFRESH PING
 *   - Reply:    CH=1;TOTAL=5;NAME=...;PAUSED=0;VHS=1;TINNY=0;MINI=0
 *               or ERR=... on failure.
 * The reply is parsed and shown on the LCD panel; PAUSE flips its label
 * between PAUSE/PLAY based on the server's reported state.
 * Press START to quit.
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
#include <sys/time.h>
#include "font.h"

#define SCREEN_W  960
#define SCREEN_H  544
#define PC_PORT   8410          /* VITA_REMOTE_PORT in GleepTV */

#define CONNECT_TIMEOUT_US (3 * 1000 * 1000)   /* 3s connect cap */
#define IO_TIMEOUT_SEC     2                    /* 2s send/recv cap */

/* UI text: 4x-scaled 5x7 font = 20x28 px glyphs. */
#define UI_TEXT_SCALE 4.0f
#define UI_TEXT_H     28

#define COL_BG     RGBA8(8, 8, 8, 255)
#define COL_PANEL  RGBA8(16, 16, 16, 255)
#define COL_RED    RGBA8(255, 0, 60, 255)
#define COL_GREEN  RGBA8(0, 255, 120, 255)
#define COL_BTN    RGBA8(38, 38, 38, 255)
#define COL_TEXT   RGBA8(235, 235, 235, 255)

enum { SCR_IP = 0, SCR_REMOTE };

enum {
    ACT_PAUSE = 1, ACT_CHUP, ACT_CHDN,
    ACT_VHS, ACT_TINNY, ACT_MINI,
    ACT_GUIDE, ACT_SETTINGS, ACT_REFRESH, ACT_PING,
    ACT_DIGIT, ACT_DOT, ACT_DEL,
    ACT_CONNECT, ACT_REMOTE, ACT_IPSCREEN
};

typedef struct {
    int x, y, w, h;
    const char *label;
    int action;
    int digit;
} Button;

static char g_ip[16] = "192.168.1.";
static int  g_screen = SCR_IP;
static char g_status[96] = "ENTER PC IP THEN TAP CONNECT";

/* Last reply from the server + parsed state for the LCD. */
static char g_reply[128] = "";
static int  g_ch = 0, g_total = 0, g_paused = 0;
static char g_name[40] = "NO SIGNAL";

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

/* ---- Remote screen: commands the GleepTV server actually handles ---- */
static Button remote_buttons[] = {
    {126, 200, 220, 72, "PAUSE",    ACT_PAUSE,    0},
    {370, 200, 220, 72, "CH +",     ACT_CHUP,     0},
    {614, 200, 220, 72, "CH -",     ACT_CHDN,     0},
    {126, 278, 220, 72, "VHS FX",   ACT_VHS,      0},
    {370, 278, 220, 72, "TINNY",    ACT_TINNY,    0},
    {614, 278, 220, 72, "MINI TV",  ACT_MINI,     0},
    {126, 356, 220, 72, "GUIDE",    ACT_GUIDE,    0},
    {370, 356, 220, 72, "SETTINGS", ACT_SETTINGS, 0},
    {614, 356, 220, 72, "REFRESH",  ACT_REFRESH,  0},
    {126, 434, 340, 72, "PING TEST",ACT_PING,     0},
    {500, 434, 340, 72, "IP/SET",   ACT_IPSCREEN, 0},
};
#define REMOTE_BTN_COUNT ((int)(sizeof(remote_buttons) / sizeof(remote_buttons[0])))

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
    case ACT_CHUP:     return "CH+";
    case ACT_CHDN:     return "CH-";
    case ACT_VHS:      return "VHS";
    case ACT_TINNY:    return "TINNY";
    case ACT_MINI:     return "MINI";
    case ACT_GUIDE:    return "GUIDE";
    case ACT_SETTINGS: return "SETTINGS";
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

/* Connect to the PC, send one command, read the one-line reply.
 * Returns 0 and fills g_reply on any reply; -1 with an ERR=... in
 * g_reply on failure. Never blocks longer than ~5s total. */
static int send_command(const char *cmd)
{
    int sock = -1, eid = -1;
    SceNetSockaddrIn addr;
    SceNetEpollEvent ev;
    SceNetEpollEvent out[1];
    char line[48];
    char buf[160];
    struct timeval tv;
    int on = 1, off = 0;
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

    /* Non-blocking connect so a wrong IP can't freeze the UI. */
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(PC_PORT);
    if (sceNetInetPton(SCE_NET_AF_INET, g_ip, &addr.sin_addr) <= 0) {
        snprintf(g_reply, sizeof(g_reply), "ERR=BAD IP");
        sceNetSocketClose(sock);
        return -1;
    }

    ret = sceNetConnect(sock, (SceNetSockaddr *)&addr, sizeof(addr));
    if (ret < 0 && ret != SCE_NET_ERROR_EINPROGRESS) {
        snprintf(g_reply, sizeof(g_reply), "ERR=REFUSED");
        sceNetSocketClose(sock);
        return -1;
    }
    if (ret == SCE_NET_ERROR_EINPROGRESS) {
        memset(&ev, 0, sizeof(ev));
        ev.events = SCE_NET_EPOLLOUT;
        ev.data.fd = sock;
        eid = sceNetEpollCreate("gleep", 0);
        if (eid < 0 ||
            sceNetEpollControl(eid, SCE_NET_EPOLL_CTL_ADD, sock, &ev) < 0) {
            snprintf(g_reply, sizeof(g_reply), "ERR=EPOLL");
            if (eid >= 0) sceNetEpollDestroy(eid);
            sceNetSocketClose(sock);
            return -1;
        }
        if (sceNetEpollWait(eid, out, 1, CONNECT_TIMEOUT_US) <= 0) {
            snprintf(g_reply, sizeof(g_reply), "ERR=TIMEOUT");
            sceNetEpollDestroy(eid);
            sceNetSocketClose(sock);
            return -1;
        }
        sceNetEpollDestroy(eid);
        eid = -1;
        sceNetGetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR,
                         &err, &errlen);
        if (err != 0) {
            snprintf(g_reply, sizeof(g_reply), "ERR=REFUSED");
            sceNetSocketClose(sock);
            return -1;
        }
    }

    /* Back to blocking for the data phase, with hard timeouts. */
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &off, sizeof(off));
    tv.tv_sec = IO_TIMEOUT_SEC;
    tv.tv_usec = 0;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &tv, sizeof(tv));
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &tv, sizeof(tv));

    snprintf(line, sizeof(line), "%s\n", cmd);
    if (sceNetSend(sock, line, (unsigned int)strlen(line), 0) < 0) {
        snprintf(g_reply, sizeof(g_reply), "ERR=SEND");
        sceNetSocketClose(sock);
        return -1;
    }

    /* Read one reply line (the server sends status + '\n'). */
    memset(buf, 0, sizeof(buf));
    while (total < (int)sizeof(buf) - 1) {
        len = sceNetRecv(sock, buf + total,
                         (unsigned int)(sizeof(buf) - 1 - total), 0);
        if (len <= 0)
            break;
        total += len;
        if (memchr(buf, '\n', (size_t)total))
            break;
    }
    sceNetSocketClose(sock);

    if (total <= 0) {
        snprintf(g_reply, sizeof(g_reply), "ERR=NO REPLY");
        return -1;
    }
    buf[total] = '\0';
    while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r'))
        buf[--total] = '\0';

    snprintf(g_reply, sizeof(g_reply), "%s", buf);

    if (strncmp(buf, "ERR", 3) == 0)
        return -1;

    parse_reply(buf);
    return 0;
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
        /* PING verifies the link and fills the LCD; it has no side
         * effects on the server. */
        if (send_command("PING") == 0)
            set_status("CONNECTED");
        else
            set_status(g_reply);
        break;
    case ACT_REMOTE:
        g_screen = SCR_REMOTE;
        break;
    case ACT_IPSCREEN:
        g_screen = SCR_IP;
        set_status("ENTER PC IP THEN TAP CONNECT");
        break;
    default: {
        const char *cmd = cmd_for_action(b->action);
        if (!cmd)
            break;
        if (send_command(cmd) == 0)
            set_status("OK");
        else
            set_status(g_reply);
        break;
    }
    }
}

static void handle_tap(int x, int y)
{
    const Button *btns = (g_screen == SCR_IP) ? ip_buttons : remote_buttons;
    int count = (g_screen == SCR_IP) ? IP_BTN_COUNT : REMOTE_BTN_COUNT;
    int i;

    for (i = 0; i < count; i++) {
        const Button *b = &btns[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
            do_action(b);
            return;
        }
    }
}

static void touch_calibrate(void)
{
    SceTouchPanelInfo pinfo;

    /* REQUIRED: the panel defaults to not sampling. Without this,
     * sceTouchPeek never returns any reports. */
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                             SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchEnableTouchForce(SCE_TOUCH_PORT_FRONT);

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

static void draw_button_l(const Button *b, const char *label)
{
    int tw;
    vita2d_draw_rectangle((float)b->x, (float)b->y, (float)b->w, (float)b->h, COL_RED);
    vita2d_draw_rectangle((float)b->x + 4, (float)b->y + 4,
                          (float)b->w - 8, (float)b->h - 8, COL_BTN);
    tw = text_width(UI_TEXT_SCALE, label);
    draw_text(UI_TEXT_SCALE, b->x + (b->w - tw) / 2, b->y + (b->h - UI_TEXT_H) / 2,
              COL_TEXT, label);
}

static void draw_button(const Button *b)
{
    draw_button_l(b, b->label);
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
        draw_button(&ip_buttons[i]);
}

static void draw_remote_screen(void)
{
    char l1[48];
    char stat[80];
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
        draw_button_l(b, lab);
    }

    snprintf(stat, sizeof(stat), "%s", g_status);
    draw_text(2.0f, 126, 516, COL_GREEN, stat);
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

        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START)
            break;

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
        else
            draw_remote_screen();

        vita2d_end_drawing();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();
    }

    vita2d_fini();
    sceNetTerm();
    sceKernelExitProcess(0);
    return 0;
}
