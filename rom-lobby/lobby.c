// Lobby demo – online lobbies for the N64, driven entirely from the console.
//
// The phone/PC only runs the modem page (index.html); this ROM talks to it over the
// SummerCart64's USB port using the protocol in PROTOCOL.md (USB datatype 0x40).
//
// Screens: name entry → lobby browser → create lobby / join → waiting room.
// In the waiting room every player steers a dot with the stick (real-time data through the
// modem), A waves, and the host starts a countdown with START.

#include <libdragon.h>
#include <usb.h>
#include <string.h>
#include <stdio.h>

// ---------- protocol (see PROTOCOL.md) ----------
#define PROTO_VERSION  1
#define USB_TYPE_MODEM 0x40
enum { M_HELLO = 0x01, M_BROWSE, M_CREATE, M_JOIN, M_LEAVE, M_SEND, M_SET_INFO };
enum { M_STATUS = 0x81, M_LOBBY_LIST, M_JOINED, M_PLAYER_JOINED, M_PLAYER_LEFT, M_DATA, M_LEFT, M_ERROR };
enum { CH_POS = 1, CH_WAVE = 2, CH_START = 3 };
#define APP_NAME "lobbydemo"

#define COLS 40
#define MAX_LOBBIES 12
#define MAX_PLAYERS 8
#define STR_MAX 64

static const char *cups[] = { "Mushroom Cup", "Flower Cup", "Star Cup", "Special Cup", "Free play" };
#define CUP_COUNT 5

// ---------- state ----------
typedef struct { char id[STR_MAX], name[STR_MAX], host[STR_MAX], info[STR_MAX]; int players, max, open; } lobby_t;
typedef struct { int present, is_host; char name[STR_MAX]; float x, y, tx, ty; } player_t;

static lobby_t lobbies[MAX_LOBBIES];
static int lobby_count = 0, lobby_sel = 0;

static player_t players[MAX_PLAYERS];          // slot 0 = me
static char lobby_name[STR_MAX], lobby_info[STR_MAX];
static int lobby_max = 8, i_am_host = 0;

static char my_name[17] = "";
static int usb_ok = 0;
static uint64_t last_rx_ms = 0, last_hello_ms = 0, last_browse_ms = 0;
static int modem_flags = 0, players_online = 0, ever_heard_modem = 0;

typedef enum { SCR_NAME, SCR_BROWSE, SCR_CREATE, SCR_JOINING, SCR_LOBBY, SCR_MESSAGE } screen_t;
static screen_t screen = SCR_NAME, after_message = SCR_BROWSE;
static char message[2][STR_MAX];
static uint64_t message_until = 0;

static char toast[STR_MAX];
static uint64_t toast_until = 0;
static uint64_t countdown_end = 0;              // 0 = no countdown running

static uint64_t now_ms(void) { return get_ticks_ms(); }
static int modem_alive(void) { return ever_heard_modem && now_ms() - last_rx_ms < 5000; }

static void show_message(const char *a, const char *b, screen_t next) {
    strncpy(message[0], a, STR_MAX - 1); message[0][STR_MAX - 1] = 0;
    strncpy(message[1], b ? b : "", STR_MAX - 1); message[1][STR_MAX - 1] = 0;
    after_message = next; message_until = now_ms() + 2500; screen = SCR_MESSAGE;
}
static void show_toast(const char *s) { strncpy(toast, s, STR_MAX - 1); toast[STR_MAX - 1] = 0; toast_until = now_ms() + 2000; }

// UTF-8 → ASCII for the 8x8 font (German umlauts transliterated, other characters become '?').
static void to_ascii(const uint8_t *in, int n, char *out, int outsize) {
    int o = 0;
    for (int i = 0; i < n && o < outsize - 3; i++) {
        uint8_t c = in[i];
        if (c >= 0x20 && c < 0x7F) { out[o++] = c; continue; }
        if (c == 0xC3 && i + 1 < n) {
            const char *rep = "?";
            switch (in[++i]) {
                case 0xA4: rep = "ae"; break; case 0xB6: rep = "oe"; break; case 0xBC: rep = "ue"; break;
                case 0x84: rep = "Ae"; break; case 0x96: rep = "Oe"; break; case 0x9C: rep = "Ue"; break;
                case 0x9F: rep = "ss"; break;
            }
            while (*rep && o < outsize - 1) out[o++] = *rep++;
            continue;
        }
        if (c >= 0x80) { i += (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0; out[o++] = '?'; }
    }
    out[o] = 0;
}

// ---------- sending ----------
static uint8_t txbuf[1100];
static int txlen;
static void tx_begin(uint8_t type) { txbuf[0] = type; txlen = 1; }
static void tx_u8(int v) { if (txlen < (int)sizeof txbuf) txbuf[txlen++] = (uint8_t)v; }
static void tx_str(const char *s) {
    int n = strlen(s); if (n > 63) n = 63;
    tx_u8(n);
    for (int i = 0; i < n; i++) tx_u8(s[i]);
}
static void tx_send(void) { if (usb_ok) usb_write(USB_TYPE_MODEM, txbuf, txlen); }

static void send_hello(void) { tx_begin(M_HELLO); tx_u8(PROTO_VERSION); tx_str(APP_NAME); tx_str(my_name); tx_send(); last_hello_ms = now_ms(); }
static void send_browse(void) { tx_begin(M_BROWSE); tx_send(); last_browse_ms = now_ms(); }

// ---------- receiving ----------
typedef struct { const uint8_t *p; int n, o, bad; } rd_t;
static int rd_u8(rd_t *r) { if (r->o >= r->n) { r->bad = 1; return 0; } return r->p[r->o++]; }
static void rd_str(rd_t *r, char *out) {
    int len = rd_u8(r);
    if (r->o + len > r->n) { r->bad = 1; out[0] = 0; return; }
    to_ascii(r->p + r->o, len, out, STR_MAX);
    r->o += len;
}

static void reset_lobby_state(void) {
    memset(players, 0, sizeof players);
    players[0].present = 1;
    strncpy(players[0].name, my_name, STR_MAX - 1);
    players[0].x = players[0].tx = 160; players[0].y = players[0].ty = 160;
    countdown_end = 0;
}

static void handle_message(const uint8_t *p, int n) {
    rd_t r = { p, n, 0, 0 };
    int type = rd_u8(&r);
    switch (type) {
    case M_STATUS: {
        rd_u8(&r); modem_flags = rd_u8(&r); players_online = rd_u8(&r);
        if (screen == SCR_LOBBY && !(modem_flags & 4)) show_message("Connection to the lobby was lost.", NULL, SCR_BROWSE);
        if ((screen == SCR_BROWSE || screen == SCR_CREATE) && !(modem_flags & 2) && (modem_flags & 1) && now_ms() - last_browse_ms > 2000) send_browse();
        break;
    }
    case M_LOBBY_LIST: {
        int count = rd_u8(&r);
        if (count > MAX_LOBBIES) count = MAX_LOBBIES;
        int i;
        for (i = 0; i < count && !r.bad; i++) {
            lobby_t *l = &lobbies[i];
            rd_str(&r, l->id); rd_str(&r, l->name); rd_str(&r, l->host);
            l->players = rd_u8(&r); l->max = rd_u8(&r); l->open = rd_u8(&r);
            rd_str(&r, l->info);
        }
        lobby_count = r.bad ? 0 : i;
        if (lobby_sel >= lobby_count) lobby_sel = lobby_count ? lobby_count - 1 : 0;
        break;
    }
    case M_JOINED: {
        i_am_host = rd_u8(&r);
        char id[STR_MAX]; rd_str(&r, id);
        rd_str(&r, lobby_name); lobby_max = rd_u8(&r); rd_str(&r, lobby_info);
        reset_lobby_state();
        players[0].is_host = i_am_host;
        screen = SCR_LOBBY;
        break;
    }
    case M_PLAYER_JOINED: {
        int slot = rd_u8(&r), host = rd_u8(&r);
        char name[STR_MAX]; rd_str(&r, name);
        if (slot < 1 || slot >= MAX_PLAYERS || r.bad) break;
        player_t *pl = &players[slot];
        if (!pl->present) {
            char t[STR_MAX + 16]; snprintf(t, sizeof t, "%.24s joined", name); show_toast(t);
            pl->x = pl->tx = 40 + slot * 30; pl->y = pl->ty = 160;
        }
        pl->present = 1; pl->is_host = host;
        memcpy(pl->name, name, STR_MAX);
        break;
    }
    case M_PLAYER_LEFT: {
        int slot = rd_u8(&r);
        if (slot < 1 || slot >= MAX_PLAYERS) break;
        if (players[slot].present) { char t[STR_MAX + 16]; snprintf(t, sizeof t, "%.24s left", players[slot].name); show_toast(t); }
        players[slot].present = 0;
        break;
    }
    case M_DATA: {
        int slot = rd_u8(&r), ch = rd_u8(&r);
        if (slot < 1 || slot >= MAX_PLAYERS || r.bad || !players[slot].present) break;
        const uint8_t *d = p + r.o; int len = n - r.o;
        if (ch == CH_POS && len >= 4) {
            players[slot].tx = (d[0] << 8) | d[1];
            players[slot].ty = (d[2] << 8) | d[3];
        } else if (ch == CH_WAVE) {
            char t[STR_MAX + 16]; snprintf(t, sizeof t, "%.24s waves!", players[slot].name); show_toast(t);
        } else if (ch == CH_START && len >= 1) {
            countdown_end = now_ms() + d[0] * 1000;
        }
        break;
    }
    case M_LEFT: {
        int reason = rd_u8(&r);
        static const char *why[] = { "You left the lobby.", "The host left the lobby.", "That lobby is full.", "Lobby not found.", "Connection lost." };
        lobby_count = 0;
        if (reason == 0 && screen != SCR_LOBBY && screen != SCR_JOINING) break;
        show_message(why[reason < 5 ? reason : 4], NULL, SCR_BROWSE);
        send_browse();
        break;
    }
    case M_ERROR: {
        char e[STR_MAX]; rd_str(&r, e);
        show_toast(e);
        break;
    }
    }
}

static uint8_t rxbuf[2304];
static void poll_usb(void) {
    if (!usb_ok) return;
    for (int guard = 0; guard < 4; guard++) {
        uint32_t header = usb_poll();
        if (!header) return;
        int type = USBHEADER_GETTYPE(header), size = USBHEADER_GETSIZE(header);
        int n = size < (int)sizeof rxbuf ? size : (int)sizeof rxbuf;
        usb_read(rxbuf, n);
        if (size > n) usb_skip(size - n);
        if (type != USB_TYPE_MODEM) continue;
        last_rx_ms = now_ms(); ever_heard_modem = 1;
        for (int o = 0; o + 2 <= n; ) {                // frames: u16 length + message
            int len = (rxbuf[o] << 8) | rxbuf[o + 1];
            o += 2;
            if (len <= 0 || o + len > n) break;
            handle_message(rxbuf + o, len);
            o += len;
        }
    }
}

// ---------- name storage (EEPROM 4 kbit, set by the modem page when uploading) ----------
static void load_name(void) {
    if (eeprom_present() == EEPROM_NONE) return;
    uint8_t buf[24];
    eeprom_read_bytes(buf, 0, sizeof buf);
    if (memcmp(buf, "NAME", 4) != 0) return;
    memcpy(my_name, buf + 4, 16); my_name[16] = 0;
    for (int i = 0; my_name[i]; i++) if (my_name[i] < 0x20 || my_name[i] > 0x7E) { my_name[0] = 0; break; }
}
static void save_name(void) {
    if (eeprom_present() == EEPROM_NONE) return;
    uint8_t buf[24] = { 'N', 'A', 'M', 'E' };
    memcpy(buf + 4, my_name, 16);
    eeprom_write_bytes(buf, 0, sizeof buf);
}

// ---------- on-screen keyboard ----------
static const char *kb_rows[2][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-_." },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL!", "ZXCVBNM&+?" },
};
enum { SP_SPACE, SP_DEL, SP_DONE, SP_SHIFT, SP_COUNT };
static int kb_x = 0, kb_y = 1, kb_shift = 1;
static char *kb_target; static int kb_max; static const char *kb_title;
static screen_t kb_return;

static void kb_open(char *target, int max, const char *title) { kb_target = target; kb_max = max; kb_title = title; kb_x = 0; kb_y = 1; }
static void kb_type(char c) { int n = strlen(kb_target); if (n < kb_max) { kb_target[n] = c; kb_target[n + 1] = 0; } kb_shift = 0; }
static void kb_del(void) { int n = strlen(kb_target); if (n) kb_target[n - 1] = 0; }
static int kb_press(void) {          // returns 1 when DONE was pressed
    if (kb_y < 4) { kb_type(kb_rows[kb_shift][kb_y][kb_x]); return 0; }
    switch (kb_x * SP_COUNT / 10) {
        case SP_SPACE: kb_type(' '); break;
        case SP_DEL:   kb_del(); break;
        case SP_DONE:  return 1;
        case SP_SHIFT: kb_shift ^= 1; break;
    }
    return 0;
}
static void kb_move(int dx, int dy) {
    if (dy) kb_y = (kb_y + dy + 5) % 5;
    if (dx) {
        if (kb_y == 4) { int sp = (kb_x * SP_COUNT / 10 + dx + SP_COUNT) % SP_COUNT; kb_x = sp * 10 / SP_COUNT + 1; }
        else kb_x = (kb_x + dx + 10) % 10;
    }
}

// ---------- drawing ----------
static uint32_t C_BG, C_TEXT, C_DIM, C_ACCENT, C_WARN, C_PANEL, C_SEL, C_TITLE;
static uint32_t C_SLOT[MAX_PLAYERS];
static surface_t *disp;

static void text(int col, int row, const char *s, uint32_t fg) { graphics_set_color(fg, 0); graphics_draw_text(disp, col * 8, row * 8, s); }
static void textn(int col, int row, const char *s, int maxcols, uint32_t fg) {
    char buf[COLS + 1]; int n = strlen(s); if (n > maxcols) n = maxcols; if (n > COLS) n = COLS;
    memcpy(buf, s, n); buf[n] = 0; text(col, row, buf, fg);
}
static void text_center(int row, const char *s, uint32_t fg) { int n = strlen(s); if (n > COLS) n = COLS; textn((COLS - n) / 2, row, s, COLS, fg); }

static void draw_header(const char *title) {
    graphics_draw_box(disp, 0, 0, 320, 10, C_TITLE);
    text(1, 0, title, C_TEXT);
    const char *st = modem_alive() ? ((modem_flags & 1) ? "MODEM OK" : "NO INTERNET") : "NO MODEM";
    text(COLS - 1 - strlen(st), 0, st, modem_alive() && (modem_flags & 1) ? C_ACCENT : C_WARN);
}
static void draw_footer(const char *s) { textn(1, 29, s, COLS - 2, C_DIM); }

static void draw_keyboard(int top) {
    char line[COLS + 8];
    snprintf(line, sizeof line, "%s:", kb_title);
    text(2, top - 4, line, C_DIM);
    graphics_draw_box(disp, 12, (top - 2) * 8 - 2, 296, 12, C_PANEL);
    int n = strlen(kb_target);
    snprintf(line, sizeof line, "%.30s%c", kb_target, (now_ms() / 400) % 2 && n < kb_max ? '_' : ' ');
    text(2, top - 2, line, C_TEXT);
    for (int r = 0; r < 4; r++) for (int c = 0; c < 10; c++) {
        int sel = (r == kb_y && c == kb_x);
        graphics_draw_box(disp, (5 + c * 3) * 8 - 4, (top + r * 2) * 8 - 4, 20, 14, sel ? C_SEL : C_PANEL);
        char k[2] = { kb_rows[kb_shift][r][c], 0 };
        text(5 + c * 3, top + r * 2, k, sel ? C_BG : C_TEXT);
    }
    static const char *labels[SP_COUNT] = { "SPACE", "DEL", "DONE", "ABC" };
    for (int s = 0; s < SP_COUNT; s++) {
        int sel = (kb_y == 4 && s == kb_x * SP_COUNT / 10);
        int col = 5 + s * 8;
        graphics_draw_box(disp, col * 8 - 4, (top + 8) * 8 - 4, 60, 14, sel ? C_SEL : C_PANEL);
        text(col, top + 8, s == SP_SHIFT ? (kb_shift ? "abc" : "ABC") : labels[s], sel ? C_BG : C_TEXT);
    }
}

static void draw_no_modem_hint(int row) {
    text_center(row,     "Open the modem page on your phone:", C_WARN);
    text_center(row + 1, "knedlsepp.github.io/sc64-netplay", C_TEXT);
    text_center(row + 2, "and connect the SC64 via USB.", C_WARN);
}

// create-lobby form
static int form_field = 0, form_max = 4, form_cup = 0;
static char form_name[25];

static void draw(void) {
    graphics_fill_screen(disp, C_BG);
    char line[96];
    switch (screen) {
    case SCR_NAME:
        draw_header("N64 ONLINE");
        text_center(3, "Welcome! What's your name?", C_TEXT);
        draw_keyboard(16);
        draw_footer("A type  B delete  START done");
        break;

    case SCR_BROWSE:
        draw_header("ONLINE LOBBIES");
        snprintf(line, sizeof line, "You: %.16s   %d online", my_name, players_online);
        text(1, 2, line, C_DIM);
        if (!modem_alive()) { draw_no_modem_hint(10); }
        else if (!lobby_count) {
            text_center(9, "No open lobbies right now.", C_TEXT);
            text_center(11, "Press START to create one!", C_ACCENT);
        } else {
            for (int i = 0; i < lobby_count; i++) {
                lobby_t *l = &lobbies[i];
                int row = 4 + i, sel = (i == lobby_sel);
                if (sel) graphics_draw_box(disp, 4, row * 8 - 1, 312, 10, C_PANEL);
                uint32_t fg = l->open ? (sel ? C_ACCENT : C_TEXT) : C_DIM;
                textn(1, row, l->name, 24, fg);
                snprintf(line, sizeof line, l->open ? "%d/%d" : "%d/%d busy", l->players, l->max);
                text(l->open ? 34 : 29, row, line, fg);
            }
            lobby_t *l = &lobbies[lobby_sel];
            snprintf(line, sizeof line, "Host: %.24s", l->host); textn(1, 18, line, COLS - 2, C_DIM);
            textn(1, 19, l->info, COLS - 2, C_DIM);
        }
        draw_footer("A join  START create  R rename");
        break;

    case SCR_CREATE: {
        draw_header("CREATE LOBBY");
        const char *labels[] = { "Name", "Players", "Cup", "" };
        for (int f = 0; f < 4; f++) {
            int row = 5 + f * 3, sel = (form_field == f);
            if (sel) graphics_draw_box(disp, 4, row * 8 - 3, 312, 14, C_PANEL);
            if (f == 0) { text(2, row, labels[f], C_DIM); textn(12, row, form_name, 26, sel ? C_ACCENT : C_TEXT); }
            if (f == 1) { text(2, row, labels[f], C_DIM); snprintf(line, sizeof line, "< %d >", form_max); text(12, row, line, sel ? C_ACCENT : C_TEXT); }
            if (f == 2) { text(2, row, labels[f], C_DIM); snprintf(line, sizeof line, "< %s >", cups[form_cup]); text(12, row, line, sel ? C_ACCENT : C_TEXT); }
            if (f == 3) text_center(row, "[ CREATE ]", sel ? C_ACCENT : C_TEXT);
        }
        if (!modem_alive()) draw_no_modem_hint(19);
        draw_footer("A edit/select  <> change  B back");
        break;
    }

    case SCR_JOINING:
        draw_header("JOINING");
        text_center(12, "Joining lobby...", C_TEXT);
        { static const char *dots[] = { ".", "..", "...", "....", "....." }; text_center(14, dots[(now_ms() / 300) % 5], C_DIM); }
        draw_footer("B cancel");
        break;

    case SCR_LOBBY: {
        draw_header(i_am_host ? "YOUR LOBBY" : "LOBBY");
        textn(1, 2, lobby_name, COLS - 2, C_ACCENT);
        textn(1, 3, lobby_info, COLS - 2, C_DIM);
        int count = 0;
        for (int s = 0; s < MAX_PLAYERS; s++) {
            if (!players[s].present) continue;
            int row = 5 + count, col = 1 + (count >= 4 ? 20 : 0);
            if (count >= 4) row -= 4;
            graphics_draw_box(disp, col * 8, row * 8 + 1, 6, 6, C_SLOT[s]);
            snprintf(line, sizeof line, "%.12s%s%s", players[s].name, players[s].is_host ? " *" : "", s == 0 ? " (you)" : "");
            textn(col + 1, row, line, 19, C_TEXT);
            count++;
        }
        snprintf(line, sizeof line, "%d/%d", count, lobby_max);
        text(COLS - 1 - strlen(line), 2, line, C_DIM);
        // play field: everyone steers a dot
        graphics_draw_box(disp, 8, 80, 304, 140, C_PANEL);
        for (int s = MAX_PLAYERS - 1; s >= 0; s--) {
            if (!players[s].present) continue;
            int x = (int)players[s].x, y = (int)players[s].y;
            graphics_draw_box(disp, x - 4, y - 4, 9, 9, C_SLOT[s]);
            if (s == 0) graphics_draw_box(disp, x - 1, y - 1, 3, 3, C_BG);
        }
        if (countdown_end) {
            int64_t left = (int64_t)countdown_end - (int64_t)now_ms();
            if (left > 0) { snprintf(line, sizeof line, "RACE STARTS IN %d", (int)(left / 1000) + 1); text_center(13, line, C_WARN); }
            else if (left > -1500) text_center(13, "GO! GO! GO!", C_ACCENT);
            else countdown_end = 0;
        }
        draw_footer(i_am_host ? "Stick move  A wave  START race  B leave" : "Stick move  A wave  B leave");
        break;
    }

    case SCR_MESSAGE:
        draw_header("N64 ONLINE");
        text_center(13, message[0], C_TEXT);
        text_center(15, message[1], C_DIM);
        break;
    }
    if (toast_until > now_ms()) {
        graphics_draw_box(disp, 0, 222, 320, 12, C_TITLE);
        text_center(28, toast, C_TEXT);
    }
}

// ---------- input ----------
static int dir_repeat(int *last, int *rep, int dir) {
    int fire = 0;
    if (dir != *last) { fire = (dir != JOYPAD_8WAY_NONE); *rep = 15; }
    else if (dir != JOYPAD_8WAY_NONE && --(*rep) <= 0) { fire = 1; *rep = 5; }
    *last = dir;
    return fire;
}
static void dir_to_dxdy(int dir, int *dx, int *dy) {
    *dx = *dy = 0;
    switch (dir) {
        case JOYPAD_8WAY_RIGHT: *dx = 1; break;  case JOYPAD_8WAY_LEFT: *dx = -1; break;
        case JOYPAD_8WAY_UP: *dy = -1; break;    case JOYPAD_8WAY_DOWN: *dy = 1; break;
        case JOYPAD_8WAY_UP_RIGHT: *dx = 1; *dy = -1; break;   case JOYPAD_8WAY_UP_LEFT: *dx = -1; *dy = -1; break;
        case JOYPAD_8WAY_DOWN_RIGHT: *dx = 1; *dy = 1; break;  case JOYPAD_8WAY_DOWN_LEFT: *dx = -1; *dy = 1; break;
    }
}

static void start_create(void) {
    snprintf(form_name, sizeof form_name, "%.16s's lobby", my_name);
    form_field = 0; screen = SCR_CREATE;
}

static void do_create(void) {
    if (!modem_alive()) { show_toast("No modem connected"); return; }
    char info[48]; snprintf(info, sizeof info, "%s", cups[form_cup]);
    tx_begin(M_CREATE); tx_u8(form_max); tx_str(form_name[0] ? form_name : "Lobby"); tx_str(info); tx_send();
    screen = SCR_JOINING;
}

static uint64_t last_pos_ms = 0;
static int last_sent_x = -1, last_sent_y = -1;

int main(void) {
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    joypad_init();
    C_BG = graphics_make_color(0x14, 0x16, 0x1c, 0xFF);
    C_TEXT = graphics_make_color(0xEC, 0xEA, 0xE3, 0xFF);
    C_DIM = graphics_make_color(0x8A, 0x88, 0x80, 0xFF);
    C_ACCENT = graphics_make_color(0x5D, 0xCA, 0xA5, 0xFF);
    C_WARN = graphics_make_color(0xF0, 0xB7, 0x5A, 0xFF);
    C_PANEL = graphics_make_color(0x2C, 0x2F, 0x38, 0xFF);
    C_SEL = graphics_make_color(0x5D, 0xCA, 0xA5, 0xFF);
    C_TITLE = graphics_make_color(0x26, 0x21, 0x5C, 0xFF);
    const uint8_t pal[MAX_PLAYERS][3] = { {0x5D,0xCA,0xA5}, {0xF0,0x99,0x7B}, {0x85,0xB7,0xEB}, {0xED,0x93,0xB1}, {0xEF,0x9F,0x27}, {0xAF,0xA9,0xEC}, {0x97,0xC4,0x59}, {0xF7,0xC1,0xC1} };
    for (int i = 0; i < MAX_PLAYERS; i++) C_SLOT[i] = graphics_make_color(pal[i][0], pal[i][1], pal[i][2], 0xFF);

    usb_ok = usb_initialize() && usb_getcart() == CART_SC64;
    load_name();
    if (my_name[0]) screen = SCR_BROWSE; else kb_open(my_name, 16, "Name");
    kb_return = SCR_BROWSE;

    int last_dir = JOYPAD_8WAY_NONE, rep = 0, was_alive = 0;
    for (;;) {
        joypad_poll();
        joypad_buttons_t b = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        int dir = joypad_get_direction(JOYPAD_PORT_1, JOYPAD_2D_LH);
        int fire = dir_repeat(&last_dir, &rep, dir), dx, dy;
        dir_to_dxdy(dir, &dx, &dy);
        uint64_t t = now_ms();

        // talk to the modem: say hello until it answers, and again when it comes back
        int alive = modem_alive();
        if (!alive && my_name[0] && t - last_hello_ms > 1000) send_hello();
        if (alive && !was_alive) {
            if (screen == SCR_LOBBY || screen == SCR_JOINING) show_message("Modem reconnected.", "The lobby was closed.", SCR_BROWSE);
            send_browse();
        }
        if (!alive && was_alive && (screen == SCR_LOBBY || screen == SCR_JOINING)) show_message("Lost the modem.", "Is the phone still connected?", SCR_BROWSE);
        was_alive = alive;
        poll_usb();

        switch (screen) {
        case SCR_NAME:
            if (fire) kb_move(dx, dy);
            if (b.a && kb_press()) b.start = 1;
            if (b.b) kb_del();
            if (b.z) kb_shift ^= 1;
            if (b.start && kb_target[0]) {
                screen = kb_return;
                if (kb_target == my_name) {             // a new player name means a new modem session
                    save_name();
                    strncpy(players[0].name, my_name, STR_MAX - 1);
                    send_hello();
                    send_browse();
                }
            }
            break;

        case SCR_BROWSE:
            if (fire && dy && lobby_count) lobby_sel = (lobby_sel + dy + lobby_count) % lobby_count;
            if (b.a && lobby_count) {
                lobby_t *l = &lobbies[lobby_sel];
                if (!l->open) show_toast("That lobby is busy");
                else if (l->players >= l->max) show_toast("That lobby is full");
                else { tx_begin(M_JOIN); tx_str(l->id); tx_send(); screen = SCR_JOINING; }
            }
            if (b.start) start_create();
            if (b.r) { kb_open(my_name, 16, "Name"); kb_return = SCR_BROWSE; screen = SCR_NAME; }
            break;

        case SCR_CREATE:
            if (fire && dy) form_field = (form_field + dy + 4) % 4;
            if (fire && dx && form_field == 1) { form_max += dx; if (form_max < 2) form_max = 8; if (form_max > 8) form_max = 2; }
            if (fire && dx && form_field == 2) form_cup = (form_cup + dx + CUP_COUNT) % CUP_COUNT;
            if (b.a && form_field == 0) { kb_open(form_name, 24, "Lobby name"); screen = SCR_NAME; kb_return = SCR_CREATE; }
            if ((b.a && form_field == 3) || b.start) do_create();
            if (b.b) screen = SCR_BROWSE;
            break;

        case SCR_JOINING:
            if (b.b) { tx_begin(M_LEAVE); tx_send(); screen = SCR_BROWSE; }
            break;

        case SCR_LOBBY: {
            joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
            player_t *me = &players[0];
            me->x += in.stick_x / 16.0f; me->y -= in.stick_y / 16.0f;
            if (me->x < 14) me->x = 14;
            if (me->x > 306) me->x = 306;
            if (me->y < 86) me->y = 86;
            if (me->y > 214) me->y = 214;
            if (t - last_pos_ms > 66 && ((int)me->x != last_sent_x || (int)me->y != last_sent_y)) {   // ~15 updates/s
                last_pos_ms = t; last_sent_x = (int)me->x; last_sent_y = (int)me->y;
                tx_begin(M_SEND); tx_u8(0xFF); tx_u8(CH_POS);
                tx_u8(last_sent_x >> 8); tx_u8(last_sent_x); tx_u8(last_sent_y >> 8); tx_u8(last_sent_y);
                tx_send();
            }
            for (int s = 1; s < MAX_PLAYERS; s++) {            // smooth the others towards their last position
                players[s].x += (players[s].tx - players[s].x) * 0.3f;
                players[s].y += (players[s].ty - players[s].y) * 0.3f;
            }
            if (b.a) { tx_begin(M_SEND); tx_u8(0xFF); tx_u8(CH_WAVE); tx_send(); show_toast("You wave!"); }
            if (b.start && i_am_host && !countdown_end) {
                tx_begin(M_SEND); tx_u8(0xFF); tx_u8(CH_START); tx_u8(3); tx_send();
                countdown_end = t + 3000;
            }
            if (b.b) { tx_begin(M_LEAVE); tx_send(); screen = SCR_BROWSE; lobby_count = 0; send_browse(); }
            break;
        }

        case SCR_MESSAGE:
            if (t > message_until || b.a || b.b) screen = after_message;
            break;
        }

        disp = display_get();
        draw();
        display_show(disp);
    }
}
