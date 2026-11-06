/*
 * Gleep Remote — PS Vita client for GleepTV on PC.
 * Sends text commands over TCP (default port 8410) to the
 * RemoteServer in gleeptv.py and displays the returned status.
 */
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include <vita2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W     960
#define SCREEN_H     544
#define DEFAULT_PORT 8410
#define CONFIG_PATH  "ux0:data/gleep_remote.cfg"

#define COL_BG     RGBA8(8, 8, 8, 255)
#define COL_PANEL  RGBA8(26, 26, 26, 255)
#define COL_BORDER RGBA8(255, 0, 0, 255)
#define COL_TEXT   RGBA8(255, 255, 255, 255)
#define COL_DIM    RGBA8(140, 140, 140, 255)
#define COL_GREEN  RGBA8(0, 255, 0, 255)
#define COL_RED    RGBA8(255, 60, 60, 255)
#define COL_LCD    RGBA8(0, 48, 0, 255)
#define COL_LCD_T  RGBA8(0, 255, 0, 255)

typedef enum { SCR_REMOTE = 0, SCR_KEYPAD, SCR_CONFIG } Screen;

enum {
    ACT_SEND = 1,
    ACT_GOTO_REMOTE,
    ACT_GOTO_KEYPAD,
    ACT_GOTO_CONFIG,
    ACT_DIGIT,
    ACT_DOT,
    ACT_DEL,
    ACT_SAVE_IP,
    ACT_ENTER_CH
};

typedef struct {
    int x, y, w, h;
    const char *label;
    const char *cmd;   /* used by ACT_SEND */
    int action;
    char ch;           /* used by ACT_DIGIT */
} Button;

/* ---------------- state ---------------- */
static Screen screen = SCR_REMOTE;
static char pc_ip[32] = "";
static int  pc_port = DEFAULT_PORT;

static int  st_channel = 0, st_total = 0;
static int  st_paused = 0, st_vhs = 1, st_tinny = 0, st_mini = 0;
static char st_name[64] = "NO SIGNAL";
static char last_reply[160] = "";
static int  connected = 0;

static char ip_buf[32] = "";
static char ch_buf[8] = "";

static int net_inited = 0;

/* ---------------- buttons ---------------- */
static const Button remote_btns[] = {
    {  30, 170, 280, 90, "CH -",     "CH-",      ACT_SEND, 0 },
    { 340, 170, 280, 90, "PAUSE",    "PAUSE",    ACT_SEND, 0 },
    { 650, 170, 280, 90, "CH +",     "CH+",      ACT_SEND, 0 },
    {  30, 280, 280, 90, "VHS FX",   "VHS",      ACT_SEND, 0 },
    { 340, 280, 280, 90, "TINNY",    "TINNY",    ACT_SEND, 0 },
    { 650, 280, 280, 90, "MINI TV",  "MINI",     ACT_SEND, 0 },
    {  30, 390, 280, 90, "GUIDE",    "GUIDE",    ACT_SEND, 0 },
    { 340, 390, 280, 90, "SETTINGS", "SETTINGS", ACT_SEND, 0 },
    { 650, 390, 280, 90, "KEYPAD",   NULL,       ACT_GOTO_KEYPAD, 0 },
    {  30, 480, 200, 44, "SETUP",    NULL,       ACT_GOTO_CONFIG, 0 },
    { 650, 480, 200, 44, "REFRESH",  "REFRESH",  ACT_SEND, 0 },
};

static const Button keypad_btns[] = {
    { 180, 150, 180, 80, "1", NULL, ACT_DIGIT, '1' },
    { 390, 150, 180, 80, "2", NULL, ACT_DIGIT, '2' },
    { 600, 150, 180, 80, "3", NULL, ACT_DIGIT, '3' },
    { 180, 240, 180, 80, "4", NULL, ACT_DIGIT, '4' },
    { 390, 240, 180, 80, "5", NULL, ACT_DIGIT, '5' },
    { 600, 240, 180, 80, "6", NULL, ACT_DIGIT, '6' },
    { 180, 330, 180, 80, "7", NULL, ACT_DIGIT, '7' },
    { 390, 330, 180, 80, "8", NULL, ACT_DIGIT, '8' },
    { 600, 330, 180, 80, "9", NULL, ACT_DIGIT, '9' },
    { 180, 420, 180, 80, "0", NULL, ACT_DIGIT, '0' },
    { 390, 420, 180, 80, "ENTER", NULL, ACT_ENTER_CH, 0 },
    { 600, 420, 180, 80, "BACK",  NULL, ACT_GOTO_REMOTE, 0 },
};

static const Button config_btns[] = {
    { 180, 150, 180, 70, "1", NULL, ACT_DIGIT, '1' },
    { 390, 150, 180, 70, "2", NULL, ACT_DIGIT, '2' },
    { 600, 150, 180, 70, "3", NULL, ACT_DIGIT, '3' },
    { 180, 228, 180, 70, "4", NULL, ACT_DIGIT, '4' },
    { 390, 228, 180, 70, "5", NULL, ACT_DIGIT, '5' },
    { 600, 228, 180, 70, "6", NULL, ACT_DIGIT, '6' },
    { 180, 306, 180, 70, "7", NULL, ACT_DIGIT, '7' },
    { 390, 306, 180, 70, "8", NULL, ACT_DIGIT, '8' },
    { 600, 306, 180, 70, "9", NULL, ACT_DIGIT, '9' },
    { 180, 384, 180, 70, ".", NULL, ACT_DOT, 0 },
    { 390, 384, 180, 70, "0", NULL, ACT_DIGIT, '0' },
    { 600, 384, 180, 70, "DEL", NULL, ACT_DEL, 0 },
    { 180, 462, 280, 60, "SAVE & CONNECT", NULL, ACT_SAVE_IP, 0 },
    { 500, 462, 280, 60, "BACK", NULL, ACT_GOTO_REMOTE, 0 },
};

/* ---------------- networking ---------------- */
static int parse_ipv4(const char *s, unsigned int *out)
{
    unsigned int a, b, c, d;
    if (!s || sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return -1;
    *out = (a << 24) | (b << 16) | (c << 8) | d; /* big-endian, as SceNet wants */
    return 0;
}

static int init_net(void)
{
    if (net_inited)
        return 0;
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0)
        return -1;
    if (sceNetInit(1 * 1024 * 1024, NULL, NULL, NULL, NULL) < 0)
        return -1;
    sceNetCtlInit(); /* may fail if wifi is off; connect attempts will just fail */
    net_inited = 1;
    return 0;
}

static int send_command(const char *cmd, char *reply, int reply_size)
{
    unsigned int ip;
    int sock, ret, total = 0;
    SceNetSockaddrIn addr;
    char line[128];
    char *nl;

    reply[0] = '\0';
    if (init_net() < 0) { snprintf(reply, reply_size, "ERR=NET_INIT"); return -1; }
    if (parse_ipv4(pc_ip, &ip) < 0) { snprintf(reply, reply_size, "ERR=BAD_IP"); return -1; }

    sock = sceNetSocket("gleep", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (sock < 0) { snprintf(reply, reply_size, "ERR=SOCKET"); return -1; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port   = sceNetHtons((unsigned short)pc_port);
    addr.sin_addr.s_addr = ip;

    if (sceNetConnect(sock, (SceNetSockaddr *)&addr, sizeof(addr)) < 0) {
        sceNetClose(sock);
        connected = 0;
        snprintf(reply, reply_size, "ERR=CONNECT");
        return -1;
    }

    snprintf(line, sizeof(line), "%s\n", cmd);
    sceNetSend(sock, line, (unsigned int)strlen(line), 0);

    while (total < reply_size - 1) {
        ret = sceNetRecv(sock, reply + total, (unsigned int)(reply_size - 1 - total), 0);
        if (ret <= 0)
            break;
        total += ret;
        reply[total] = '\0';
        if (strchr(reply, '\n'))
            break;
    }
    nl = strchr(reply, '\n');
    if (nl) *nl = '\0';
    sceNetClose(sock);
    connected = 1;
    return 0;
}

/* reply format: CH=5;TOTAL=42;NAME=ADULT SWIM;PAUSED=0;VHS=1;TINNY=0;MINI=0 */
static void parse_status(const char *reply)
{
    char buf[256];
    char *tok;
    strncpy(buf, reply, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    tok = strtok(buf, ";");
    while (tok) {
        if      (!strncmp(tok, "CH=", 3))     st_channel = atoi(tok + 3);
        else if (!strncmp(tok, "TOTAL=", 6))  st_total    = atoi(tok + 6);
        else if (!strncmp(tok, "PAUSED=", 7)) st_paused   = atoi(tok + 7);
        else if (!strncmp(tok, "VHS=", 4))    st_vhs      = atoi(tok + 4);
        else if (!strncmp(tok, "TINNY=", 6)) st_tinny    = atoi(tok + 6);
        else if (!strncmp(tok, "MINI=", 5))   st_mini     = atoi(tok + 5);
        else if (!strncmp(tok, "NAME=", 5)) {
            strncpy(st_name, tok + 5, sizeof(st_name) - 1);
            st_name[sizeof(st_name) - 1] = '\0';
        }
        tok = strtok(NULL, ";");
    }
}

static void quick_send(const char *cmd)
{
    char reply[256];
    if (send_command(cmd, reply, sizeof(reply)) == 0 && strncmp(reply, "ERR=", 4) != 0)
        parse_status(reply);
    strncpy(last_reply, reply, sizeof(last_reply) - 1);
    last_reply[sizeof(last_reply) - 1] = '\0';
}

/* ---------------- config persistence ---------------- */
static void load_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    char line[64];
    if (!f) return;
    if (fgets(pc_ip, sizeof(pc_ip), f)) {
        pc_ip[strcspn(pc_ip, "\r\n")] = '\0';
        if (fgets(line, sizeof(line), f)) {
            int p = atoi(line);
            if (p > 0 && p < 65536) pc_port = p;
        }
    }
    fclose(f);
    if (!pc_ip[0]) screen = SCR_CONFIG;
}

static void save_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "%s\n%d\n", pc_ip, pc_port);
    fclose(f);
}

/* ---------------- actions ---------------- */
static void goto_config(void)
{
    strncpy(ip_buf, pc_ip, sizeof(ip_buf) - 1);
    ip_buf[sizeof(ip_buf) - 1] = '\0';
    screen = SCR_CONFIG;
}

static void do_action(const Button *b)
{
    char cmd[48];
    switch (b->action) {
    case ACT_SEND:
        quick_send(b->cmd);
        break;
    case ACT_GOTO_REMOTE:
        screen = SCR_REMOTE;
        break;
    case ACT_GOTO_KEYPAD:
        ch_buf[0] = '\0';
        screen = SCR_KEYPAD;
        break;
    case ACT_GOTO_CONFIG:
        goto_config();
        break;
    case ACT_DIGIT:
        if (screen == SCR_CONFIG) {
            size_t len = strlen(ip_buf);
            if (len < sizeof(ip_buf) - 1) { ip_buf[len] = b->ch; ip_buf[len + 1] = '\0'; }
        } else {
            size_t len = strlen(ch_buf);
            if (len < sizeof(ch_buf) - 1) { ch_buf[len] = b->ch; ch_buf[len + 1] = '\0'; }
        }
        break;
    case ACT_DOT: {
        size_t len = strlen(ip_buf);
        if (len < sizeof(ip_buf) - 1 && !strchr(ip_buf, '.')) {
            ip_buf[len] = '.'; ip_buf[len + 1] = '\0';
        }
        break;
    }
    case ACT_DEL:
        if (screen == SCR_CONFIG) {
            size_t len = strlen(ip_buf);
            if (len) ip_buf[len - 1] = '\0';
        } else {
            size_t len = strlen(ch_buf);
            if (len) ch_buf[len - 1] = '\0';
        }
        break;
    case ACT_SAVE_IP:
        strncpy(pc_ip, ip_buf, sizeof(pc_ip) - 1);
        pc_ip[sizeof(pc_ip) - 1] = '\0';
        save_config();
        screen = SCR_REMOTE
    case ACT_SAVE_IP:
        strncpy(pc_ip, ip_buf, sizeof(pc_pc_ip) - 1);
        pc_ip[sizeof(pc_ip) - 1] = '\0';
        save_config();
        screen = SCR_REMOTE;
        /* one-time status refresh; may block a few seconds if PC is off */
        quick_send("PING");
        break;
    case ACT_ENTER_CH:
        if (ch_buf[0]) {
            snprintf(cmd, sizeof(cmd), "CH:%s", ch_buf);
            quick_send(cmd);
        }
        ch_buf[0] = '\0';
        screen = SCR_REMOTE;
        break;
    }
}

/* ---------------- input helpers ---------------- */
static const Button *current_buttons(int *count)
{
    switch (screen) {
    case SCR_REMOTE: *count = sizeof(remote_btns) / sizeof(Button); return remote_btns;
    case SCR_KEYPAD: *count = sizeof(keypad_btns) / sizeof(Button); return keypad_btns;
    default:         *count = sizeof(config_btns) / sizeof(Button); return config_btns;
    }
}

static int hit_test(const Button *btns, int n, int x, int y)
{
    int i;
    for (i = 0; i < n; i++)
        if (x >= btns[i].x && x < btns[i].x + btns[i].w &&
            y >= btns[i].y && y < btns[i].y + btns[i].h)
            return i;
    return -1;
}

/* ---------------- drawing ---------------- */
static void draw_button(const Button *b, int highlight)
{
    unsigned int border = highlight ? COL_BORDER : COL_DIM;
    vita2d_draw_rectangle(b->x, b->y, b->w, b->h, COL_PANEL);
    vita2d_draw_rectangle(b->x, b->y, b->w, 2, border);
    vita2d_draw_rectangle(b->x, b->y + b->h - 2, b->w, 2, border);
    vita2d_draw_rectangle(b->x, b->y, 2, b->h, border);
    vita2d_draw_rectangle(b->x + b->w - 2, b->y, 2, b->h, border);
    draw_text(b->x + (b->w - text_width(2, b->label)) / 2,
              b->y + (b->h - 7 * 2) / 2,
              2, COL_TEXT, b->label);
}

static void draw_header(void)
{
    draw_text(30, 12, 3, COL_BORDER, "GLEEP REMOTE");
    draw_text(560, 16, 1, COL_DIM, "TCP LINK");
    draw_text(660, 16, 1, connected ? COL_GREEN : COL_RED,
              connected ? "ONLINE" : "OFFLINE");
    draw_text(30, 40, 1, COL_DIM, "PC:");
    draw_text(70, 40, 1, COL_TEXT, pc_ip[0] ? pc_ip : "NOT SET");
}

static void draw_footer(void)
{
    draw_text(30, 526, 1, COL_DIM, last_reply[0] ? last_reply : "READY");
}

static void draw_remote_screen(int sel)
{
    int n, i;
    const Button *btns = current_buttons(&n);
    char line[80];

    /* green LCD status panel */
    vita2d_draw_rectangle(30, 60, 900, 95, COL_LCD);
    vita2d_draw_rectangle(30, 60, 900, 2, COL_DIM);
    vita2d_draw_rectangle(30, 153, 900, 2, COL_DIM);
    snprintf(line, sizeof(line), "CH %02d / %02d   %s", st_channel, st_total, st_name);
    draw_text(50, 72, 2, COL_LCD_T, line);
    snprintf(line, sizeof(line), "PAUSED:%s   VHS:%s   TINNY:%s   MINI:%s",
             st_paused ? "YES" : "NO", st_vhs ? "ON" : "OFF",
             st_tinny ? "ON" : "OFF", st_mini ? "ON" : "OFF");
    draw_text(50, 118, 1, COL_LCD_T, line);

    for (i = 0; i < n; i++)
        draw_button(&btns[i], i == sel);
}

static void draw_keypad_screen(int sel)
{
    int n, i;
    const Button *btns = current_buttons(&n);
    char line[32];
    snprintf(line, sizeof(line), "CH - %s", ch_buf[0] ? ch_buf : "_");
    draw_text((SCREEN_W - text_width(4, line)) / 2, 70, 4, COL_TEXT, line);
    for (i = 0; i < n; i++)
        draw_button(&btns[i], i == sel);
}

static void draw_config_screen(int sel)
{
    int n, i;
    const Button *btns = current_buttons(&n);
    char line[48];
    snprintf(line, sizeof(line), "IP: %s", ip_buf[0] ? ip_buf : "_");
    draw_text((SCREEN_W - text_width(3, line)) / 2, 60, 3, COL_TEXT, line);
    draw_text(30, 120, 1, COL_DIM, "ENTER YOUR PC'S IPV4 ADDRESS, THEN SAVE");
    for (i = 0; i < n; i++)
        draw_button(&btns[i], i == sel);
}

/* ---------------- main ---------------- */
int main(void)
{
    SceTouchPanelInfo pinfo;
    unsigned prev_btns = 0;
    int prev_touch = 0;
    int sel = 0;
    int t_min_x = 0, t_min_y = 0, t_span_x = 1, t_span_y = 1;

    vita2d_init();
    vita2d_set_clear_color(COL_BG);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &pinfo) >= 0) {
        t_min_x = pinfo.minA.x; t_min_y = pinfo.minA.y;
        t_span_x = pinfo.maxA.x - pinfo.minA.x;
        t_span_y = pinfo.maxA.y - pinfo.minA.y;
        if (t_span_x <= 0) t_span_x = 1;
        if (t_span_y <= 0) t_span_y = 1;
    }

    load_config();

    for (;;) {
        SceCtrlData pad;
        SceTouchData td;
        int n, tx, ty, idx;
        unsigned pressed;
        const Button *btns;
        Screen prev_screen = screen;

        /* ---- input ---- */
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) ==
            (SCE_CTRL_SELECT | SCE_CTRL_START))
            break;

        pressed = pad.buttons & ~prev_btns;
        prev_btns = pad.buttons;

        btns = current_buttons(&n);

        if (pressed & SCE_CTRL_UP)    sel = (sel + n - 3) % n;
        if (pressed & SCE_CTRL_DOWN)  sel = (sel + 3) % n;
        if (pressed & SCE_CTRL_LEFT)  sel = (sel + n - 1) % n;
        if (pressed & SCE_CTRL_RIGHT) sel = (sel + 1) % n;
        if (pressed & SCE_CTRL_CROSS) do_action(&btns[sel]);
        if (pressed & SCE_CTRL_START) goto_config();
        if (pressed & SCE_CTRL_CIRCLE && screen != SCR_REMOTE) screen = SCR_REMOTE;

        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
        if (td.reportNum > 0 && !prev_touch) {
            tx = (td.report[0].x - t_min_x) * SCREEN_W / t_span_x;
            ty = (td.report[0].y - t_min_y) * SCREEN_H / t_span_y;
            idx = hit_test(btns, n, tx, ty);
            if (idx >= 0) { sel = idx; do_action(&btns[idx]); }
        }
        prev_touch = td.reportNum > 0;

        if (screen != prev_screen) sel = 0;

        /* ---- draw ---- */
        vita2d_start_drawing();
        vita2d_clear_screen();
        draw_header();
        switch (screen) {
        case SCR_REMOTE: draw_remote_screen(sel); break;
        case SCR_KEYPAD: draw_keypad_screen(sel); break;
        default:          draw_config_screen(sel); break;
        }
        draw_footer();
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    vita2d_fini();
    return 0;
}