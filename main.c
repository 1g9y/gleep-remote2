/* Gleep Remote - PS Vita client for GleepTV.
 * Sends one command per line over TCP to the PC (default port 5555).
 * Commands sent: PAUSE PLAY STOP VOLUP VOLDN MUTE SEEKB SEEKF FULL CHUP CHDN
 * Press START to quit.
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

#define SCREEN_W  480
#define SCREEN_H  272
#define PC_PORT   5555

#define COL_BG     RGBA8(8, 8, 8, 255)
#define COL_PANEL  RGBA8(16, 16, 16, 255)
#define COL_RED    RGBA8(255, 0, 60, 255)
#define COL_GREEN  RGBA8(0, 255, 120, 255)
#define COL_BTN    RGBA8(38, 38, 38, 255)
#define COL_TEXT   RGBA8(235, 235, 235, 255)

enum { SCR_IP = 0, SCR_REMOTE };

enum {
    ACT_NONE = 0,
    ACT_PAUSE, ACT_PLAY, ACT_STOP,
    ACT_VOLUP, ACT_VOLDN, ACT_MUTE,
    ACT_SEEKB, ACT_SEEKF, ACT_FULL,
    ACT_CHUP, ACT_CHDN,
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

static int t_min_x = 0, t_min_y = 0, t_span_x = 1, t_span_y = 1;

/* ---- IP screen: 4x3 keypad + connect/remote ---- */
static Button ip_buttons[] = {
    {108,  92, 60, 32, "1", ACT_DIGIT, 1},
    {176,  92, 60, 32, "2", ACT_DIGIT, 2},
    {244,  92, 60, 32, "3", ACT_DIGIT, 3},
    {312,  92, 60, 32, "4", ACT_DIGIT, 4},
    {108, 132, 60, 32, "5", ACT_DIGIT, 5},
    {176, 132, 60, 32, "6", ACT_DIGIT, 6},
    {244, 132, 60, 32, "7", ACT_DIGIT, 7},
    {312, 132, 60, 32, "8", ACT_DIGIT, 8},
    {108, 172, 60, 32, "9", ACT_DIGIT, 9},
    {176, 172, 60, 32, "0", ACT_DIGIT, 0},
    {244, 172, 60, 32, ".", ACT_DOT, 0},
    {312, 172, 60, 32, "DEL", ACT_DEL, 0},
    {116, 212, 120, 32, "CONNECT", ACT_CONNECT, 0},
    {244, 212, 120, 32, "REMOTE", ACT_REMOTE, 0},
};
#define IP_BTN_COUNT ((int)(sizeof(ip_buttons) / sizeof(ip_buttons[0])))

/* ---- Remote screen: 3x4 grid ---- */
static Button remote_buttons[] = {
    { 63,  48, 110, 40, "PAUSE",  ACT_PAUSE, 0},
    {185,  48, 110, 40, "PLAY",   ACT_PLAY, 0},
    {307,  48, 110, 40, "STOP",   ACT_STOP, 0},
    { 63,  98, 110, 40, "VOL +",  ACT_VOLUP, 0},
    {185,  98, 110, 40, "VOL -",  ACT_VOLDN, 0},
    {307,  98, 110, 40, "MUTE",   ACT_MUTE, 0},
    { 63, 148, 110, 40, "SEEK <", ACT_SEEKB, 0},
    {185, 148, 110, 40, "SEEK >", ACT_SEEKF, 0},
    {307, 148, 110, 40, "FULL",   ACT_FULL, 0},
    { 63, 198, 110, 40, "CH +",   ACT_CHUP, 0},
    {185, 198, 110, 40, "CH -",   ACT_CHDN, 0},
    {307, 198, 110, 40, "IP/SET", ACT_IPSCREEN, 0},
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
    case ACT_PAUSE: return "PAUSE";
    case ACT_PLAY:  return "PLAY";
    case ACT_STOP:  return "STOP";
    case ACT_VOLUP: return "VOLUP";
    case ACT_VOLDN: return "VOLDN";
    case ACT_MUTE:  return "MUTE";
    case ACT_SEEKB: return "SEEKB";
    case ACT_SEEKF: return "SEEKF";
    case ACT_FULL:  return "FULL";
    case ACT_CHUP:  return "CHUP";
    case ACT_CHDN:  return "CHDN";
    default:        return NULL;
    }
}

static int send_command(const char *cmd)
{
    int sock;
    SceNetSockaddrIn addr;
    char line[48];

    if (g_ip[0] == '\0')
        return -1;

    sock = sceNetSocket("gleep", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(PC_PORT);
    if (sceNetInetPton(SCE_NET_AF_INET, g_ip, &addr.sin_addr) <= 0) {
        sceNetSocketClose(sock);
        return -1;
    }
    if (sceNetConnect(sock, (SceNetSockaddr *)&addr, sizeof(addr)) < 0) {
        sceNetSocketClose(sock);
        return -1;
    }

    snprintf(line, sizeof(line), "%s\n", cmd);
    sceNetSend(sock, line, (unsigned int)strlen(line), 0);
    sceNetSocketClose(sock);
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
        set_status("SAVED. TAP A BUTTON TO SEND");
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
        if (cmd && send_command(cmd) == 0)
            set_status("SENT");
        else
            set_status("SEND FAILED - CHECK IP/PC");
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

static void draw_button(const Button *b)
{
    int tw;
    vita2d_draw_rectangle((float)b->x, (float)b->y, (float)b->w, (float)b->h, COL_RED);
    vita2d_draw_rectangle((float)b->x + 2, (float)b->y + 2,
                          (float)b->w - 4, (float)b->h - 4, COL_BTN);
    tw = text_width(2.0f, b->label);
    draw_text(2.0f, b->x + (b->w - tw) / 2, b->y + (b->h - 14) / 2, COL_TEXT, b->label);
}

static void draw_ip_screen(void)
{
    char buf[24];
    int tw;
    int i;

    draw_text(2.5f, 16, 24, COL_RED, "GLEEP REMOTE");

    vita2d_draw_rectangle(108, 52, 264, 30, COL_RED);
    vita2d_draw_rectangle(110, 54, 260, 26, COL_PANEL);
    snprintf(buf, sizeof(buf), "%s_", g_ip);
    tw = text_width(2.0f, buf);
    draw_text(2.0f, 108 + (264 - tw) / 2, 60, COL_GREEN, buf);

    for (i = 0; i < IP_BTN_COUNT; i++)
        draw_button(&ip_buttons[i]);
}

static void draw_remote_screen(void)
{
    int i;
    draw_text(2.5f, 16, 24, COL_RED, "GLEEP REMOTE");
    for (i = 0; i < REMOTE_BTN_COUNT; i++)
        draw_button(&remote_buttons[i]);
    draw_text(1.5f, 63, 248, COL_GREEN, "TAP A BUTTON TO SEND A COMMAND");
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

        draw_text(1.5f, 8, 6, COL_GREEN, g_status);

        vita2d_end_drawing();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();
    }

    vita2d_fini();
    sceNetTerm();
    sceKernelExitProcess(0);
    return 0;
}
