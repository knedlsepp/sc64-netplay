// N64 Chat – ein kleiner Chat-Client für das SummerCart64.
//
// Die Webseite (index.html) leitet Chatnachrichten aus dem Internet per USB an den N64 weiter
// und schickt alles, was hier getippt wird, an die anderen im Raum.
//
// Bedienung:
//   Stick / Steuerkreuz  Taste auf der Bildschirmtastatur wählen
//   A                    Taste tippen
//   B                    letztes Zeichen löschen
//   R                    Leerzeichen
//   Z                    Groß-/Kleinschreibung umschalten
//   START                Nachricht senden

#include <libdragon.h>
#include <usb.h>
#include <string.h>
#include <stdio.h>

#define COLS        40              // 320 px / 8 px Schrift
#define HIST_TOP    2               // erste Zeile des Verlaufs
#define HIST_ROWS   14              // Zeilen für den Verlauf (2..15)
#define INPUT_ROW   17
#define KB_TOP      19              // Tastenzeilen 19,21,23,25; Sondertasten 27
#define MAX_INPUT   120
#define MAX_LINES   64
#define LINE_LEN    (COLS - 2)

// ---------- Nachrichtenverlauf (bereits umgebrochen) ----------
typedef struct { char text[LINE_LEN + 1]; uint8_t own; } line_t;
static line_t lines[MAX_LINES];
static int line_count = 0;

static void push_line(const char *s, int len, uint8_t own) {
    if (line_count == MAX_LINES) {
        memmove(&lines[0], &lines[1], sizeof(line_t) * (MAX_LINES - 1));
        line_count--;
    }
    line_t *l = &lines[line_count++];
    if (len > LINE_LEN) len = LINE_LEN;
    memcpy(l->text, s, len);
    l->text[len] = 0;
    l->own = own;
}

// Bricht eine Nachricht an Wortgrenzen auf LINE_LEN Zeichen um.
static void add_message(const char *msg, uint8_t own) {
    int len = strlen(msg);
    int pos = 0;
    while (pos < len) {
        int take = len - pos;
        if (take > LINE_LEN) {
            take = LINE_LEN;
            for (int i = LINE_LEN; i > LINE_LEN / 2; i--) {
                if (msg[pos + i] == ' ') { take = i; break; }
            }
        }
        push_line(msg + pos, take, own);
        pos += take;
        while (pos < len && msg[pos] == ' ') pos++;
    }
    if (len == 0) push_line("", 0, own);
}

// UTF-8 → ASCII für die eingebaute 8x8-Schrift (Umlaute werden umschrieben).
static void utf8_to_ascii(const uint8_t *in, int n, char *out, int outsize) {
    int o = 0;
    for (int i = 0; i < n && o < outsize - 3; i++) {
        uint8_t c = in[i];
        if (c >= 0x20 && c < 0x7F) { out[o++] = c; continue; }
        if (c == '\n' || c == '\t') { out[o++] = ' '; continue; }
        if (c == 0xC3 && i + 1 < n) {
            uint8_t d = in[++i];
            const char *rep = "?";
            switch (d) {
                case 0xA4: rep = "ae"; break; case 0xB6: rep = "oe"; break; case 0xBC: rep = "ue"; break;
                case 0x84: rep = "Ae"; break; case 0x96: rep = "Oe"; break; case 0x9C: rep = "Ue"; break;
                case 0x9F: rep = "ss"; break;
            }
            while (*rep && o < outsize - 1) out[o++] = *rep++;
            continue;
        }
        if (c >= 0x80) {                       // andere Mehrbyte-Zeichen überspringen
            int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
            i += extra;
            out[o++] = '?';
        }
    }
    out[o] = 0;
}

// ---------- Bildschirmtastatur ----------
static const char *kb_rows[2][4] = {
    { "1234567890", "qwertzuiop", "asdfghjkl!", "yxcvbnm,.?" },
    { "1234567890", "QWERTZUIOP", "ASDFGHJKL:", "YXCVBNM-+'" },
};
#define KB_COLS 10
#define KB_ROWS 5                    // 4 Zeichenzeilen + 1 Zeile mit Sondertasten
enum { SP_SPACE, SP_DEL, SP_SEND, SP_SHIFT, SP_COUNT };
static const char *sp_labels[SP_COUNT] = { "LEER", "DEL", "SENDEN", "ABC" };

static int kb_x = 0, kb_y = 1, shift = 0;
static char input[MAX_INPUT + 1];
static int input_len = 0;

static void type_char(char c) {
    if (input_len < MAX_INPUT) { input[input_len++] = c; input[input_len] = 0; }
}

static void delete_char(void) {
    if (input_len > 0) input[--input_len] = 0;
}

static int usb_ok = 0;

static void send_input(void) {
    if (input_len == 0) return;
    if (usb_ok) usb_write(DATATYPE_TEXT, input, input_len);
    char buf[MAX_INPUT + 8];
    snprintf(buf, sizeof buf, "Du: %s", input);
    add_message(buf, 1);
    if (!usb_ok) add_message("(nicht gesendet: kein USB)", 1);
    input_len = 0; input[0] = 0;
}

static void press_key(void) {
    if (kb_y < 4) { type_char(kb_rows[shift][kb_y][kb_x]); return; }
    int sp = kb_x * SP_COUNT / KB_COLS;
    switch (sp) {
        case SP_SPACE: type_char(' '); break;
        case SP_DEL:   delete_char(); break;
        case SP_SEND:  send_input(); break;
        case SP_SHIFT: shift ^= 1; break;
    }
}

static void move_cursor(int dx, int dy) {
    if (dy) {
        kb_y = (kb_y + dy + KB_ROWS) % KB_ROWS;
    }
    if (dx) {
        if (kb_y == 4) {                       // Sondertasten springen ganze Felder
            int sp = kb_x * SP_COUNT / KB_COLS;
            sp = (sp + dx + SP_COUNT) % SP_COUNT;
            kb_x = sp * KB_COLS / SP_COUNT + 1;
        } else {
            kb_x = (kb_x + dx + KB_COLS) % KB_COLS;
        }
    }
}

// ---------- USB-Empfang ----------
static void poll_usb(void) {
    if (!usb_ok) return;
    for (int guard = 0; guard < 4; guard++) {
        unsigned long header = usb_poll();
        if (!header) return;
        int type = USBHEADER_GETTYPE(header);
        int size = USBHEADER_GETSIZE(header);
        static uint8_t raw[512];
        int n = size < (int)sizeof raw ? size : (int)sizeof raw;
        usb_read(raw, n);
        if (size > n) usb_skip(size - n);
        if (type == DATATYPE_TEXT) {
            char text[600];
            utf8_to_ascii(raw, n, text, sizeof text);
            add_message(text, 0);
        } else {
            char info[48];
            snprintf(info, sizeof info, "[%d Bytes Binaerdaten, Typ %d]", size, type);
            add_message(info, 0);
        }
    }
}

// ---------- Zeichnen ----------
static uint32_t C_BG, C_TEXT, C_DIM, C_OWN, C_ACCENT, C_KEY, C_KEYSEL, C_TITLE;

static void text_at(surface_t *d, int col, int row, const char *s, uint32_t fg, uint32_t bg) {
    graphics_set_color(fg, bg);
    graphics_draw_text(d, col * 8, row * 8, s);
}

static void draw(surface_t *d, int frame) {
    graphics_fill_screen(d, C_BG);

    // Titelzeile
    graphics_draw_box(d, 0, 0, 320, 10, C_TITLE);
    text_at(d, 1, 0, "N64 CHAT", C_TEXT, 0);
    text_at(d, 22, 0, usb_ok ? "USB: SC64 bereit" : "USB: nicht gefunden", usb_ok ? C_ACCENT : C_OWN, 0);

    // Verlauf (die letzten HIST_ROWS Zeilen)
    int start = line_count > HIST_ROWS ? line_count - HIST_ROWS : 0;
    for (int i = start; i < line_count; i++) {
        text_at(d, 1, HIST_TOP + (i - start), lines[i].text, lines[i].own ? C_ACCENT : C_TEXT, 0);
    }
    if (line_count == 0) {
        text_at(d, 1, HIST_TOP + 1, "Warte auf Nachrichten...", C_DIM, 0);
        text_at(d, 1, HIST_TOP + 3, "Webseite: SC64 verbinden und", C_DIM, 0);
        text_at(d, 1, HIST_TOP + 4, "einem Raum beitreten.", C_DIM, 0);
    }

    // Eingabezeile (zeigt das Ende, wenn der Text zu lang ist)
    graphics_draw_box(d, 0, INPUT_ROW * 8 - 2, 320, 12, C_KEY);
    char shown[COLS + 1];
    const char *tail = input_len > COLS - 4 ? input + input_len - (COLS - 4) : input;
    snprintf(shown, sizeof shown, "> %s%c", tail, (frame / 20) % 2 ? '_' : ' ');
    text_at(d, 0, INPUT_ROW, shown, C_TEXT, 0);

    // Tastatur: jede Taste 3 Spalten breit, ab Spalte 5
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            int sel = (r == kb_y && c == kb_x);
            int x = (5 + c * 3) * 8 - 4, y = (KB_TOP + r * 2) * 8 - 4;
            graphics_draw_box(d, x, y, 20, 14, sel ? C_KEYSEL : C_KEY);
            char k[2] = { kb_rows[shift][r][c], 0 };
            text_at(d, 5 + c * 3, KB_TOP + r * 2, k, sel ? C_BG : C_TEXT, 0);
        }
    }
    for (int s = 0; s < SP_COUNT; s++) {
        int sel = (kb_y == 4 && s == kb_x * SP_COUNT / KB_COLS);
        int col = 5 + s * 8;
        int x = col * 8 - 4, y = (KB_TOP + 8) * 8 - 4;
        graphics_draw_box(d, x, y, 60, 14, sel ? C_KEYSEL : C_KEY);
        const char *label = (s == SP_SHIFT) ? (shift ? "abc" : "ABC") : sp_labels[s];
        text_at(d, col, KB_TOP + 8, label, sel ? C_BG : C_TEXT, 0);
    }
    text_at(d, 1, 29, "A tippen B loeschen R leer START senden", C_DIM, 0);
}

int main(void) {
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    joypad_init();

    C_BG     = graphics_make_color(0x14, 0x16, 0x1c, 0xFF);
    C_TEXT   = graphics_make_color(0xEC, 0xEA, 0xE3, 0xFF);
    C_DIM    = graphics_make_color(0x8A, 0x88, 0x80, 0xFF);
    C_OWN    = graphics_make_color(0xF0, 0x99, 0x7B, 0xFF);
    C_ACCENT = graphics_make_color(0x5D, 0xCA, 0xA5, 0xFF);
    C_KEY    = graphics_make_color(0x2C, 0x2F, 0x38, 0xFF);
    C_KEYSEL = graphics_make_color(0x5D, 0xCA, 0xA5, 0xFF);
    C_TITLE  = graphics_make_color(0x26, 0x21, 0x5C, 0xFF);

    usb_ok = usb_initialize() && usb_getcart() == CART_SC64;
    if (usb_ok) {
        const char *hello = "N64 Chat ist bereit";
        usb_write(DATATYPE_TEXT, hello, strlen(hello));
    }

    int last_dir = JOYPAD_8WAY_NONE, repeat = 0, frame = 0;
    for (;;) {
        joypad_poll();
        joypad_buttons_t p = joypad_get_buttons_pressed(JOYPAD_PORT_1);

        // Richtung mit Tastenwiederholung
        int dir = joypad_get_direction(JOYPAD_PORT_1, JOYPAD_2D_LH);
        int fire = 0;
        if (dir != last_dir) { fire = (dir != JOYPAD_8WAY_NONE); repeat = 15; }
        else if (dir != JOYPAD_8WAY_NONE && --repeat <= 0) { fire = 1; repeat = 5; }
        last_dir = dir;
        if (fire) {
            int dx = 0, dy = 0;
            switch (dir) {
                case JOYPAD_8WAY_RIGHT: dx = 1; break;
                case JOYPAD_8WAY_LEFT:  dx = -1; break;
                case JOYPAD_8WAY_UP:    dy = -1; break;
                case JOYPAD_8WAY_DOWN:  dy = 1; break;
                case JOYPAD_8WAY_UP_RIGHT:   dx = 1;  dy = -1; break;
                case JOYPAD_8WAY_UP_LEFT:    dx = -1; dy = -1; break;
                case JOYPAD_8WAY_DOWN_RIGHT: dx = 1;  dy = 1;  break;
                case JOYPAD_8WAY_DOWN_LEFT:  dx = -1; dy = 1;  break;
            }
            move_cursor(dx, dy);
        }

        if (p.a) press_key();
        if (p.b) delete_char();
        if (p.r) type_char(' ');
        if (p.z) shift ^= 1;
        if (p.start) send_input();

        poll_usb();

        surface_t *d = display_get();
        draw(d, frame++);
        display_show(d);
    }
}
