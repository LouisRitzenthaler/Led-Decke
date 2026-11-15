/*
 * LED-Decke – Mini-System für den Raspberry Pi 3
 *
 * Dieses Programm ist das komplette "Betriebssystem" nach dem Linux-Kernel:
 * Es läuft als /init aus einer kleinen RAM-Disk (initramfs), lädt die nötigen
 * Kernel-Module (USB-MIDI für das Launchpad, Bluetooth), verbindet sich per
 * eingebautem Bluetooth mit der Decke und macht dasselbe wie launchpad.py.
 * Kein systemd, kein bluetoothd, kein Python, die SD-Karte wird nicht beschrieben.
 *
 * Zurück zum normalen Raspberry Pi OS: Datei "normal.txt" auf die Boot-Partition
 * der SD-Karte legen (geht am Windows-PC).
 *
 * Bedienung (wie launchpad.py):
 *   1-7 / a-i = Kästen auswählen, Spalte 8 = Farben, 9b An, 9c Aus,
 *   9d/9e Helligkeit +/-, 9f-9i Effekte.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Einstellungen                                                      */
/* ------------------------------------------------------------------ */
#define BLE_NAME "LED-Decke"
static const char *UUID_SERVICE = "6e1d0001-8a3c-4c5b-9b1e-1ed0c0dec0de";
static const char *UUID_CMD = "6e1d0002-8a3c-4c5b-9b1e-1ed0c0dec0de";
static const char *UUID_STATE = "6e1d0003-8a3c-4c5b-9b1e-1ed0c0dec0de";

#define ROWS 9
#define COLS 7
#define STATE_LEN (10 + ROWS * COLS * 6)
#define BRIGHTNESS_STEP 16
#define POLL_S 0.1
#define DESELECT_AFTER 2.5

#define N_COLORS 5 /* Spalte 8, a-e: Rot, Grün, Blau, Warmweiß, Reinweiß */
static const uint8_t COLORS[N_COLORS][3] = {
    {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 217, 160}, {200, 220, 255}};
#define N_EFFECTS 4
/* Funktionstasten (Spalte 9 und Effekte in Spalte 8) */
#define F_ON 0
#define F_OFF 1
#define F_BRI_UP 2
#define F_BRI_DOWN 3
/* 4..7 = Effekte */
#define F_TEMPO_UP 8
#define F_TEMPO_DOWN 9
#define F_ALL 10
#define F_GAME 11
static const char *EFFECT_NAME[N_EFFECTS] = {"rainbow", "colorfade", "launch", "sparkle"};
static const uint8_t EFFECT_COLOR[N_EFFECTS][3] = {
    {255, 0, 255}, {0, 255, 255}, {255, 120, 0}, {255, 255, 255}};
#define N_ESP_EFFECTS 12
static const char *ESP_EFFECTS[N_ESP_EFFECTS] = {
    "rainbow", "colorfade", "wave", "breathe", "sparkle", "rain", "wiring_test",
    "music_pulse", "music_vu", "music_beat", "music_switch", "launch"};

/* ------------------------------------------------------------------ */
/*  Kleinkram                                                          */
/* ------------------------------------------------------------------ */
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[%6.2f] ", now());
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

static void msleep(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

static void uuid_le(const char *s, uint8_t out[16]) {
    uint8_t be[16];
    int n = 0;
    for (const char *p = s; *p && n < 16; p++) {
        if (*p == '-') continue;
        unsigned v;
        sscanf(p, "%2x", &v);
        be[n++] = v;
        p++;
    }
    for (int i = 0; i < 16; i++) out[i] = be[15 - i];
}

/* ------------------------------------------------------------------ */
/*  Gemeinsamer Zustand (Bluetooth-Thread <-> Launchpad)              */
/* ------------------------------------------------------------------ */
typedef struct {
    int mode, brightness, max, speed10;
    uint8_t boxes[ROWS][COLS][3];
    uint8_t frame[ROWS][COLS][3];
} State;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static State g_state;
static int g_have_state;
static double g_last_ok;
static int g_ever_online;

#define QLEN 128
#define CMDLEN 520
static char q_buf[QLEN][CMDLEN];
static int q_head, q_tail; /* head = nächster zu senden, tail = frei */

static void post(const char *cmd) {
    pthread_mutex_lock(&mu);
    snprintf(q_buf[q_tail], CMDLEN, "%s", cmd);
    q_tail = (q_tail + 1) % QLEN;
    if (q_tail == q_head) q_head = (q_head + 1) % QLEN; /* voll: ältesten verwerfen */
    pthread_mutex_unlock(&mu);
}

static int pop(char *out) {
    int ok = 0;
    pthread_mutex_lock(&mu);
    if (q_head != q_tail) {
        memcpy(out, q_buf[q_head], CMDLEN);
        q_head = (q_head + 1) % QLEN;
        ok = 1;
    }
    pthread_mutex_unlock(&mu);
    return ok;
}

/* Spielbild für die Decke: nur das neueste zählt, ältere werden überschrieben */
static char g_frame_cmd[CMDLEN];
static int g_frame_pending;

static void post_frame(const char *cmd) {
    pthread_mutex_lock(&mu);
    snprintf(g_frame_cmd, CMDLEN, "%s", cmd);
    g_frame_pending = 1;
    pthread_mutex_unlock(&mu);
}

static int pop_frame(char *out) {
    int ok = 0;
    pthread_mutex_lock(&mu);
    if (g_frame_pending) {
        memcpy(out, g_frame_cmd, CMDLEN);
        g_frame_pending = 0;
        ok = 1;
    }
    pthread_mutex_unlock(&mu);
    return ok;
}

static void q_clear(void) {
    pthread_mutex_lock(&mu);
    q_head = q_tail;
    pthread_mutex_unlock(&mu);
}

static int synced(void) {
    pthread_mutex_lock(&mu);
    /* kurze Funk-Aussetzer (bis 10 s) nicht anzeigen, Tasten werden danach nachgeholt */
    int s = g_have_state && now() - g_last_ok < 10;
    pthread_mutex_unlock(&mu);
    return s;
}

static int parse_state(const uint8_t *d, int len, State *st) {
    if (len != STATE_LEN || d[0] != 1) return 0;
    st->mode = (int8_t)d[1];
    st->brightness = d[2];
    st->max = d[3] ? d[3] : 1;
    st->speed10 = d[4];
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            for (int k = 0; k < 3; k++) {
                st->boxes[r][c][k] = d[10 + (r * COLS + c) * 3 + k];
                st->frame[r][c][k] = d[10 + ROWS * COLS * 3 + (r * COLS + c) * 3 + k];
            }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Bluetooth (Kernel direkt: mgmt-Schnittstelle + L2CAP/ATT)          */
/* ------------------------------------------------------------------ */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_L2CAP 0
#define BTPROTO_HCI 1
#define HCI_DEV_NONE 0xffff
#define HCI_CHANNEL_CONTROL 3
#define BDADDR_LE_PUBLIC 1
#define BDADDR_LE_RANDOM 2

struct sockaddr_hci {
    sa_family_t hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

struct sockaddr_l2 {
    sa_family_t l2_family;
    unsigned short l2_psm;
    uint8_t l2_bdaddr[6];
    unsigned short l2_cid;
    uint8_t l2_bdaddr_type;
};

#define MGMT_OP_READ_INDEX_LIST 0x0003
#define MGMT_OP_SET_POWERED 0x0005
#define MGMT_OP_SET_LE 0x000D
#define MGMT_OP_START_DISCOVERY 0x0023
#define MGMT_OP_STOP_DISCOVERY 0x0024
#define MGMT_EV_CMD_COMPLETE 0x0001
#define MGMT_EV_CMD_STATUS 0x0002
#define MGMT_EV_DEVICE_FOUND 0x0012
#define MGMT_EV_DISCOVERING 0x0013

static int mgmt_fd = -1;

static int mgmt_open(void) {
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) return -1;
    struct sockaddr_hci a = {AF_BLUETOOTH, HCI_DEV_NONE, HCI_CHANNEL_CONTROL};
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int mgmt_send(uint16_t op, uint16_t idx, const void *p, uint16_t len) {
    uint8_t b[64];
    b[0] = op; b[1] = op >> 8; b[2] = idx; b[3] = idx >> 8; b[4] = len; b[5] = len >> 8;
    memcpy(b + 6, p, len);
    return write(mgmt_fd, b, 6 + len) == 6 + len ? 0 : -1;
}

typedef void (*mgmt_event_cb)(uint16_t ev, const uint8_t *p, int len);

/* Wartet auf Antwort zu op. Rückgabe: Status (0 = ok), -1 = Zeit abgelaufen/Fehler. */
static int mgmt_cmd(uint16_t op, uint16_t idx, const void *p, uint16_t len, int timeout_ms,
                    uint8_t *rsp, int *rsp_len, mgmt_event_cb cb) {
    if (mgmt_send(op, idx, p, len) < 0) return -1;
    double end = now() + timeout_ms / 1000.0;
    for (;;) {
        int left = (int)((end - now()) * 1000);
        if (left <= 0) return -1;
        struct pollfd pf = {mgmt_fd, POLLIN, 0};
        if (poll(&pf, 1, left) <= 0) continue;
        uint8_t b[1024];
        int n = read(mgmt_fd, b, sizeof b);
        if (n < 6) return -1;
        uint16_t ev = b[0] | b[1] << 8;
        int plen = b[4] | b[5] << 8;
        const uint8_t *pl = b + 6;
        if ((ev == MGMT_EV_CMD_COMPLETE || ev == MGMT_EV_CMD_STATUS) && plen >= 3 &&
            (pl[0] | pl[1] << 8) == op) {
            if (rsp && rsp_len) {
                *rsp_len = plen - 3;
                memcpy(rsp, pl + 3, plen - 3);
            }
            return pl[2];
        }
        if (cb) cb(ev, pl, plen);
    }
}

static int bt_power_on(void) {
    uint8_t rsp[256];
    int rl = 0;
    /* warten, bis der Kernel den Bluetooth-Chip angemeldet hat */
    for (;;) {
        int st = mgmt_cmd(MGMT_OP_READ_INDEX_LIST, HCI_DEV_NONE, NULL, 0, 2000, rsp, &rl, NULL);
        if (st == 0 && rl >= 2 && (rsp[0] | rsp[1] << 8) > 0) break;
        msleep(50);
    }
    uint8_t on = 1;
    mgmt_cmd(MGMT_OP_SET_LE, 0, &on, 1, 5000, NULL, NULL, NULL);
    int st = mgmt_cmd(MGMT_OP_SET_POWERED, 0, &on, 1, 20000, NULL, NULL, NULL);
    if (st != 0) {
        say("Bluetooth einschalten fehlgeschlagen (Status %d)", st);
        return -1;
    }
    mgmt_cmd(MGMT_OP_SET_LE, 0, &on, 1, 5000, NULL, NULL, NULL);
    say("Bluetooth an");
    return 0;
}

static uint8_t found_addr[6], found_type;
static int found;
static uint8_t svc_le[16];

static void on_found(uint16_t ev, const uint8_t *p, int len) {
    if (ev != MGMT_EV_DEVICE_FOUND || len < 14 || found) return;
    int eir_len = p[12] | p[13] << 8;
    const uint8_t *e = p + 14;
    if (14 + eir_len > len) eir_len = len - 14;
    int match = 0;
    for (int i = 0; i + 1 < eir_len;) {
        int l = e[i];
        if (l == 0 || i + 1 + l > eir_len) break;
        int t = e[i + 1];
        const uint8_t *v = e + i + 2;
        int vl = l - 1;
        if ((t == 0x08 || t == 0x09) && vl == (int)strlen(BLE_NAME) && !memcmp(v, BLE_NAME, vl))
            match = 1;
        if (t == 0x06 || t == 0x07)
            for (int k = 0; k + 16 <= vl; k += 16)
                if (!memcmp(v + k, svc_le, 16)) match = 1;
        i += 1 + l;
    }
    if (match) {
        memcpy(found_addr, p, 6);
        found_type = p[6];
        found = 1;
    }
}

static int bt_find_deck(int timeout_ms) {
    uint8_t type = 0x06; /* LE öffentlich + zufällig */
    found = 0;
    mgmt_cmd(MGMT_OP_STOP_DISCOVERY, 0, &type, 1, 1000, NULL, NULL, NULL);
    double end = now() + timeout_ms / 1000.0;
    int st = mgmt_cmd(MGMT_OP_START_DISCOVERY, 0, &type, 1, 3000, NULL, NULL, on_found);
    if (st != 0) {
        say("Bluetooth-Suche startet nicht (Status %d)", st);
        return -1;
    }
    while (!found && now() < end) {
        struct pollfd pf = {mgmt_fd, POLLIN, 0};
        if (poll(&pf, 1, 100) <= 0) continue;
        uint8_t b[1024];
        int n = read(mgmt_fd, b, sizeof b);
        if (n < 6) break;
        uint16_t ev = b[0] | b[1] << 8;
        int plen = b[4] | b[5] << 8;
        if (ev == MGMT_EV_DISCOVERING && plen >= 2 && b[7] == 0) /* Suche zu Ende: neu starten */
            mgmt_send(MGMT_OP_START_DISCOVERY, 0, &type, 1);
        on_found(ev, b + 6, plen);
    }
    mgmt_cmd(MGMT_OP_STOP_DISCOVERY, 0, &type, 1, 1000, NULL, NULL, NULL);
    return found ? 0 : -1;
}

static int l2_connect(int timeout_ms) {
    int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
    if (fd < 0) return -1;
    struct sockaddr_l2 a;
    memset(&a, 0, sizeof a);
    a.l2_family = AF_BLUETOOTH;
    a.l2_cid = 4; /* ATT */
    a.l2_bdaddr_type = BDADDR_LE_PUBLIC;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        say("Bluetooth bind: %s", strerror(errno));
        close(fd);
        return -1;
    }
    memcpy(a.l2_bdaddr, found_addr, 6);
    a.l2_bdaddr_type = found_type == 2 ? BDADDR_LE_RANDOM : BDADDR_LE_PUBLIC;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) {
        say("Bluetooth verbinden: %s", strerror(errno));
        close(fd);
        return -1;
    }
    struct pollfd pf = {fd, POLLOUT, 0};
    if (poll(&pf, 1, timeout_ms) <= 0) {
        say("Bluetooth verbinden: keine Antwort");
        close(fd);
        return -1;
    }
    int err = 0;
    socklen_t el = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) {
        say("Bluetooth verbinden: %s", strerror(err));
        close(fd);
        return -1;
    }
    return fd;
}

/* --- ATT (GATT-Protokoll) --- */
static int att_fd = -1;
static int att_mtu = 23;

/* Schickt eine Anfrage und wartet auf die passende Antwort. Rückgabe: Länge, -1 Fehler,
 * -2 = Fehlerantwort der Decke (Code in *att_err). */
static int att_err;
static int att_req(const uint8_t *req, int rl, uint8_t want, uint8_t *rsp, int rsp_max, int timeout_ms) {
    if (send(att_fd, req, rl, 0) != rl) return -1;
    double end = now() + timeout_ms / 1000.0;
    for (;;) {
        int left = (int)((end - now()) * 1000);
        if (left <= 0) return -1;
        struct pollfd pf = {att_fd, POLLIN, 0};
        int pr = poll(&pf, 1, left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) return -1;
        if (pf.revents & (POLLERR | POLLHUP)) return -1;
        uint8_t b[600];
        int n = recv(att_fd, b, sizeof b, 0);
        if (n <= 0) return -1;
        uint8_t op = b[0];
        if (op == want) {
            if (n > rsp_max) n = rsp_max;
            memcpy(rsp, b, n);
            return n;
        }
        if (op == 0x01 && n >= 5 && b[1] == req[0]) {
            att_err = b[4];
            return -2;
        }
        if (op == 0x1D) { /* Indication bestätigen */
            uint8_t c = 0x1E;
            send(att_fd, &c, 1, 0);
        } else if (op == 0x02) { /* MTU-Anfrage der Decke */
            uint8_t r[3] = {0x03, 517 & 0xff, 517 >> 8};
            send(att_fd, r, 3, 0);
        } else if (op != 0x1B && op != 0x1E && !(op & 0x01) && op != 0x52 && op != 0xD2) {
            /* andere Anfrage der Decke: "nicht unterstützt" */
            uint8_t r[5] = {0x01, op, 0, 0, 0x06};
            send(att_fd, r, 5, 0);
        }
    }
}

static int h_cmd, h_state;

static int att_setup(void) {
    uint8_t b[600];
    uint8_t mreq[3] = {0x02, 517 & 0xff, 517 >> 8};
    int n = att_req(mreq, 3, 0x03, b, sizeof b, 5000);
    if (n >= 3) {
        int m = b[1] | b[2] << 8;
        att_mtu = m < 517 ? m : 517;
        if (att_mtu < 23) att_mtu = 23;
    }
    uint8_t ucmd[16], ust[16];
    uuid_le(UUID_CMD, ucmd);
    uuid_le(UUID_STATE, ust);
    h_cmd = h_state = 0;
    int start = 1;
    while (start <= 0xffff && !(h_cmd && h_state)) {
        uint8_t r[7] = {0x08, start, start >> 8, 0xff, 0xff, 0x03, 0x28};
        n = att_req(r, 7, 0x09, b, sizeof b, 5000);
        if (n == -2) break; /* keine weiteren Merkmale */
        if (n < 2) return -1;
        int el = b[1];
        if (el < 5) return -1;
        int last = start;
        for (int i = 2; i + el <= n; i += el) {
            int decl = b[i] | b[i + 1] << 8;
            int val = b[i + 3] | b[i + 4] << 8;
            last = decl;
            if (el == 21) {
                if (!memcmp(b + i + 5, ucmd, 16)) h_cmd = val;
                if (!memcmp(b + i + 5, ust, 16)) h_state = val;
            }
        }
        if (last < start) return -1;
        start = last + 1;
    }
    if (!h_cmd || !h_state) {
        say("Merkmale der Decke nicht gefunden");
        return -1;
    }
    say("Decke: MTU %d", att_mtu);
    return 0;
}

static int att_read_long(int h, uint8_t *out, int max) {
    uint8_t b[600];
    uint8_t r[5] = {0x0A, h, h >> 8, 0, 0};
    int n = att_req(r, 3, 0x0B, b, sizeof b, 6000);
    if (n < 1) return -1;
    int total = n - 1;
    if (total > max) total = max;
    memcpy(out, b + 1, total);
    while (n - 1 == att_mtu - 1 && total < max) {
        uint8_t rb[5] = {0x0C, h, h >> 8, total, total >> 8};
        n = att_req(rb, 5, 0x0D, b, sizeof b, 6000);
        if (n == -2) break;
        if (n < 1) return -1;
        int part = n - 1;
        if (total + part > max) part = max - total;
        memcpy(out + total, b + 1, part);
        total += part;
        if (n - 1 < att_mtu - 1) break;
    }
    return total;
}

static int att_write(int h, const uint8_t *v, int len) {
    uint8_t b[600];
    if (len <= att_mtu - 3) {
        uint8_t r[600];
        r[0] = 0x12; r[1] = h; r[2] = h >> 8;
        memcpy(r + 3, v, len);
        return att_req(r, 3 + len, 0x13, b, sizeof b, 6000) >= 1 ? 0 : -1;
    }
    /* lange Befehle in Stücken (Prepare Write + Execute Write) */
    int part = att_mtu - 5;
    for (int off = 0; off < len; off += part) {
        int l = len - off < part ? len - off : part;
        uint8_t r[600];
        r[0] = 0x16; r[1] = h; r[2] = h >> 8; r[3] = off; r[4] = off >> 8;
        memcpy(r + 5, v + off, l);
        if (att_req(r, 5 + l, 0x17, b, sizeof b, 6000) < 1) return -1;
    }
    uint8_t ex[2] = {0x18, 0x01};
    return att_req(ex, 2, 0x19, b, sizeof b, 6000) >= 1 ? 0 : -1;
}

static int read_state(void) {
    uint8_t d[STATE_LEN + 8];
    int n = att_read_long(h_state, d, sizeof d);
    if (n < 0) return -1;
    State st;
    if (!parse_state(d, n, &st)) {
        say("Zustand der Decke unlesbar (%d Bytes)", n);
        return 0;
    }
    pthread_mutex_lock(&mu);
    g_state = st;
    g_have_state = 1;
    g_last_ok = now();
    g_ever_online = 1;
    pthread_mutex_unlock(&mu);
    return 0;
}

/* Wo steht die Bluetooth-Verbindung? Das Launchpad zeigt es beim Warten als Farbe:
 * 0 rot = Bluetooth startet, 1 blau = suche die Decke, 2 lila = Decke gefunden, verbinde,
 * 3 gelb = verbunden, frage die Decke ab, 4 türkis = lese den Zustand, 5 weiß = Zustand unlesbar */
static volatile int g_stage;

/* Bluetooth-Chip aus- und wieder einschalten (wenn sich nichts mehr verbinden lässt) */
static void bt_restart(void) {
    say("Bluetooth wird neu gestartet");
    uint8_t off = 0, type = 0x06;
    g_stage = 0;
    mgmt_cmd(MGMT_OP_STOP_DISCOVERY, 0, &type, 1, 1000, NULL, NULL, NULL);
    mgmt_cmd(MGMT_OP_SET_POWERED, 0, &off, 1, 10000, NULL, NULL, NULL);
    msleep(500);
    while (bt_power_on() < 0) msleep(500);
}

static void *ble_thread(void *arg) {
    (void)arg;
    int fails = 0;
    uuid_le(UUID_SERVICE, svc_le);
    while ((mgmt_fd = mgmt_open()) < 0) {
        msleep(100);
    }
    while (bt_power_on() < 0) msleep(500);
    char pending[CMDLEN];
    for (;;) {
        if (fails >= 3) { /* dreimal hintereinander nichts: Bluetooth frisch starten */
            bt_restart();
            fails = 0;
        }
        g_stage = 1;
        if (bt_find_deck(10000) < 0) {
            say("Decke per Bluetooth nicht gefunden, suche weiter");
            fails++;
            continue;
        }
        g_stage = 2;
        say("Decke gefunden: %02X:%02X:%02X:%02X:%02X:%02X", found_addr[5], found_addr[4],
            found_addr[3], found_addr[2], found_addr[1], found_addr[0]);
        att_fd = l2_connect(8000);
        if (att_fd < 0) fails++;
        if (att_fd >= 0) {
            g_stage = 3;
            fcntl(att_fd, F_SETFL, 0);
            att_mtu = 23;
            double t_conn = now();
            int got_state = 0;
            if (att_setup() == 0) {
                g_stage = 4;
                say("Decke per Bluetooth verbunden");
                pthread_mutex_lock(&mu);
                int stale = now() - g_last_ok > 10;
                pthread_mutex_unlock(&mu);
                if (stale) q_clear(); /* lange weg: alte Befehle verwerfen (Tasten holt das Hauptprogramm nach) */
                int have = 0;
                double last_read = 0;
                if (read_state() == 0) {
                    last_read = now();
                    for (;;) {
                        if (!have) have = pop(pending);
                        if (!have) have = pop_frame(pending);
                        if (have) {
                            if (att_write(h_cmd, (uint8_t *)pending, strlen(pending)) < 0) break;
                            have = 0;
                            continue;
                        }
                        if (now() - last_read >= POLL_S) {
                            last_read = now();
                            if (read_state() < 0) break;
                            pthread_mutex_lock(&mu);
                            int fresh = g_have_state && now() - g_last_ok < 1;
                            pthread_mutex_unlock(&mu);
                            if (fresh) got_state = 1;
                            else g_stage = 5;
                            if (!fresh && now() - t_conn > 5) break; /* 5 s lang nichts Lesbares: neu verbinden */
                        } else {
                            msleep(10);
                        }
                    }
                }
                say("Bluetooth-Verbindung weg");
            }
            close(att_fd);
            att_fd = -1;
            /* Verbindung hat lange gehalten: alles gut. Sonst zählt es als Fehlversuch. */
            if (got_state && now() - t_conn > 30) fails = 0;
            else fails++;
        }
        msleep(200); /* gleich neu verbinden; das Launchpad zeigt bis 10 s weiter den letzten Stand */
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Launchpad (USB-MIDI über ALSA rawmidi, /dev/snd/midiC*D*)          */
/* ------------------------------------------------------------------ */
static int midi_fd = -1;
static int lp_mk2 = 1, lp_dev = 0x18, lp_scale = 63, lp_top = 104;
static int shown_valid[128];
static uint8_t shown[128][3];

static int lp_open(void) {
    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f) return -1;
    char line[256];
    int card = -1;
    char name[256] = "";
    while (fgets(line, sizeof line, f)) {
        int n;
        if (sscanf(line, " %d [", &n) == 1 && strcasestr(line, "launchpad")) {
            card = n;
            snprintf(name, sizeof name, "%s", line);
            break;
        }
        if (sscanf(line, " %d [", &n) == 1 && (strcasestr(line, "LPMiniMK3") || strcasestr(line, "LPX"))) {
            card = n;
            snprintf(name, sizeof name, "%s", line);
            break;
        }
    }
    fclose(f);
    if (card < 0) return -1;
    int sub = 0;
    if (strcasestr(name, "mk2")) {
        lp_mk2 = 1; lp_dev = 0x18; lp_scale = 63; lp_top = 104;
    } else {
        lp_mk2 = 0; lp_scale = 127; lp_top = 91;
        lp_dev = (strcasestr(name, "LPX") || strcasestr(name, "Launchpad X")) ? 0x0C : 0x0D;
        sub = 1; /* Mini MK3 / X: zweiter Anschluss = MIDI */
    }
    char path[64];
    snprintf(path, sizeof path, "/dev/snd/midiC%dD%d", card, sub);
    midi_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (midi_fd < 0 && sub) {
        snprintf(path, sizeof path, "/dev/snd/midiC%dD0", card);
        midi_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    }
    if (midi_fd < 0) return -1;
    char *nl = strchr(name, '\n');
    if (nl) *nl = 0;
    say("Launchpad gefunden: %s (%s)", name, path);
    return 0;
}

static void midi_write(const uint8_t *b, int n) {
    int off = 0;
    double end = now() + 1.0;
    while (off < n) {
        int w = write(midi_fd, b + off, n - off);
        if (w > 0) {
            off += w;
            continue;
        }
        if (w < 0 && errno == EAGAIN && now() < end) {
            msleep(1);
            continue;
        }
        say("Launchpad weg (schreiben: %s)", strerror(errno));
        exit(1);
    }
}

static void lp_sysex(const uint8_t *data, int n) {
    uint8_t b[128];
    int k = 0;
    b[k++] = 0xF0; b[k++] = 0x00; b[k++] = 0x20; b[k++] = 0x29; b[k++] = 0x02; b[k++] = lp_dev;
    memcpy(b + k, data, n);
    k += n;
    b[k++] = 0xF7;
    midi_write(b, k);
}

static int led_index(int x, int y) {
    if (y == 9) return lp_top + x - 1;
    return y * 10 + x;
}

/* leds: Farbe je Index, want[i] = 1 wenn gesetzt */
static uint8_t leds[128][3];
static int want[128];

static void lp_show(void) {
    int items[128], n = 0;
    for (int i = 0; i < 128; i++)
        if (want[i] && (!shown_valid[i] || memcmp(shown[i], leds[i], 3))) {
            items[n++] = i;
            memcpy(shown[i], leds[i], 3);
            shown_valid[i] = 1;
        }
    for (int k = 0; k < n; k += 8) {
        uint8_t d[64];
        int m = 0;
        d[m++] = lp_mk2 ? 0x0B : 0x03;
        for (int j = k; j < n && j < k + 8; j++) {
            int i = items[j];
            if (!lp_mk2) d[m++] = 0x03;
            d[m++] = i;
            for (int c = 0; c < 3; c++) d[m++] = leds[i][c] * lp_scale / 255;
        }
        lp_sysex(d, m);
    }
}

/* MIDI-Eingang lesen, gedrückte Tasten in presses[] */
static int presses[256], n_presses;
static uint8_t m_status, m_data[2];
static int m_have, m_in_sysex;

static void lp_read(void) {
    n_presses = 0;
    uint8_t b[256];
    for (;;) {
        int n = read(midi_fd, b, sizeof b);
        if (n < 0 && errno == EAGAIN) return;
        if (n <= 0) {
            say("Launchpad weg (lesen: %s)", n < 0 ? strerror(errno) : "Ende");
            exit(1);
        }
        for (int i = 0; i < n; i++) {
            uint8_t c = b[i];
            if (c >= 0xF8) continue; /* Echtzeit-Bytes */
            if (c == 0xF0) { m_in_sysex = 1; continue; }
            if (c == 0xF7) { m_in_sysex = 0; continue; }
            if (c & 0x80) { m_in_sysex = 0; m_status = c; m_have = 0; continue; }
            if (m_in_sysex || !m_status) continue;
            m_data[m_have++] = c;
            int hi = m_status & 0xF0;
            int need = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            if (m_status >= 0xF0) { m_status = 0; m_have = 0; continue; }
            if (m_have < need) continue;
            m_have = 0;
            if ((hi == 0x90 || hi == 0xB0) && m_data[1] > 0 && n_presses < 256)
                presses[n_presses++] = m_data[0];
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Bedienlogik (wie launchpad.py)                                     */
/* ------------------------------------------------------------------ */
typedef struct { int used; uint8_t c[3]; } MaybeColor;

static int box_r[128], box_c[128], is_box[128];
static int color_of[128];           /* -1 = keine Farbtaste, sonst Index in COLORS */
static int func_of[128];            /* -1 = keine, 0 on 1 off 2 bri+ 3 bri- 4.. Effekte */
static int sel[ROWS][COLS], n_sel;
static MaybeColor remembered[ROWS][COLS];
static MaybeColor saved[ROWS][COLS];
static int have_saved;
static double deselect_at;

static void sel_clear(void) {
    memset(sel, 0, sizeof sel);
    n_sel = 0;
}

static void hexcolor(const uint8_t c[3], char *out) { sprintf(out, "#%02x%02x%02x", c[0], c[1], c[2]); }

static int maxc(const uint8_t c[3]) {
    int m = c[0];
    if (c[1] > m) m = c[1];
    if (c[2] > m) m = c[2];
    return m;
}

/* Farbe -> Grundfarbe (volle Helligkeit) + Helligkeit 0..1. Rückgabe 0 = schwarz. */
static int split(const uint8_t c[3], uint8_t base[3], double *level) {
    int m = maxc(c);
    if (m == 0) {
        *level = 0;
        return 0;
    }
    for (int k = 0; k < 3; k++) base[k] = c[k] * 255 / m;
    *level = round(m / 255.0 * 10) / 10;
    return 1;
}

static void scale_col(const uint8_t base[3], double level, uint8_t out[3]) {
    for (int k = 0; k < 3; k++) out[k] = (int)(base[k] * level);
}

/* Änderungen sammeln und an die Decke schicken (gleiche Farben in einem Befehl) */
static int ch_used[ROWS][COLS];
static uint8_t ch_col[ROWS][COLS][3];

/* Was wir gerade geschickt haben, gilt 2 s lang lokal, auch wenn die Decke den neuen
 * Stand noch nicht zurückgemeldet hat. So zählt jeder schnelle Tastendruck. */
#define LOCAL_HOLD 2.0
static uint8_t lb[ROWS][COLS][3];
static double lb_until[ROWS][COLS];
static int l_bri, l_speed10, l_mode;
static double l_bri_until, l_speed_until, l_mode_until;

static void set_local_mode(int m) {
    l_mode = m;
    l_mode_until = now() + LOCAL_HOLD;
}

static void ch_reset(void) { memset(ch_used, 0, sizeof ch_used); }
static void ch_set(int r, int c, const uint8_t col[3]) {
    ch_used[r][c] = 1;
    memcpy(ch_col[r][c], col, 3);
}

static void ch_apply(void) {
    int done[ROWS][COLS];
    memset(done, 0, sizeof done);
    for (int r0 = 0; r0 < ROWS; r0++)
        for (int c0 = 0; c0 < COLS; c0++) {
            if (!ch_used[r0][c0] || done[r0][c0]) continue;
            uint8_t col[3];
            memcpy(col, ch_col[r0][c0], 3);
            for (int r = 0; r < ROWS; r++)
                for (int c = 0; c < COLS; c++)
                    if (ch_used[r][c] && !memcmp(ch_col[r][c], col, 3)) {
                        memcpy(lb[r][c], col, 3);
                        lb_until[r][c] = now() + LOCAL_HOLD;
                    }
            set_local_mode(-1);
            int cells[ROWS * COLS][2], n = 0;
            for (int r = 0; r < ROWS; r++)
                for (int c = 0; c < COLS; c++)
                    if (ch_used[r][c] && !done[r][c] && !memcmp(ch_col[r][c], col, 3)) {
                        cells[n][0] = r;
                        cells[n][1] = c;
                        n++;
                        done[r][c] = 1;
                    }
            char hx[8], cmd[CMDLEN];
            hexcolor(col, hx);
            if (n == ROWS * COLS) {
                snprintf(cmd, sizeof cmd, "{\"op\":\"fill\",\"color\":\"%s\"}", hx);
                post(cmd);
                continue;
            }
            for (int k = 0; k < n; k += 20) { /* der ESP32 nimmt max. 512 Zeichen pro Befehl */
                int m = snprintf(cmd, sizeof cmd, "{\"op\":\"boxes\",\"color\":\"%s\",\"cells\":[", hx);
                for (int j = k; j < n && j < k + 20; j++)
                    m += snprintf(cmd + m, sizeof cmd - m, "%s[%d,%d]", j > k ? "," : "", cells[j][0], cells[j][1]);
                snprintf(cmd + m, sizeof cmd - m, "]}");
                post(cmd);
            }
        }
}

static State cur; /* Kopie des Zustands für diese Runde */

static void on_color(int idx) {
    const uint8_t *col = COLORS[idx];
    if (!n_sel) { /* nichts ausgewählt: Farbe für Effekte wie Welle/Atmen */
        char hx[8], cmd[CMDLEN];
        hexcolor(col, hx);
        snprintf(cmd, sizeof cmd, "{\"op\":\"settings\",\"color\":\"%s\"}", hx);
        post(cmd);
        return;
    }
    ch_reset();
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (sel[r][c]) {
                uint8_t base[3], out[3];
                double level;
                if (!split(cur.boxes[r][c], base, &level) || level == 0) level = 1.0;
                scale_col(col, level, out);
                ch_set(r, c, out);
            }
    ch_apply();
    sel_clear();
}

static void on_off(int on) {
    static const uint8_t black[3] = {0, 0, 0}, white[3] = {255, 255, 255};
    ch_reset();
    if (n_sel) {
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) {
                if (!sel[r][c]) continue;
                uint8_t base[3];
                double level;
                int lit = split(cur.boxes[r][c], base, &level);
                if (on && !lit) {
                    ch_set(r, c, remembered[r][c].used ? remembered[r][c].c : white);
                    remembered[r][c].used = 0;
                } else if (!on && lit) {
                    remembered[r][c].used = 1;
                    memcpy(remembered[r][c].c, cur.boxes[r][c], 3);
                    ch_set(r, c, black);
                }
            }
        ch_apply();
        return;
    }
    int any = 0;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (maxc(cur.boxes[r][c]) > 0) any = 1;
    if (!on) { /* ganze Decke aus, vorher merken */
        if (any) {
            have_saved = 1;
            for (int r = 0; r < ROWS; r++)
                for (int c = 0; c < COLS; c++) {
                    saved[r][c].used = maxc(cur.boxes[r][c]) > 0;
                    memcpy(saved[r][c].c, cur.boxes[r][c], 3);
                }
        }
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) ch_set(r, c, black);
        ch_apply();
        return;
    }
    if (any) { /* Decke leuchtet schon: nur Effekt aus */
        if (cur.mode >= 0) {
            post("{\"op\":\"settings\",\"mode\":\"static\"}");
            set_local_mode(-1);
        }
        return;
    }
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            if (have_saved) {
                if (saved[r][c].used) ch_set(r, c, saved[r][c].c);
            } else {
                ch_set(r, c, white);
            }
        }
    ch_apply();
}

static void on_function(int f) {
    if (f == 0 || f == 1) {
        on_off(f == 0);
        sel_clear();
        deselect_at = 0;
    } else if (f == 2 || f == 3) {
        int step = f == 2 ? 1 : -1;
        if (!n_sel) { /* ganze Decke heller/dunkler */
            int bri = cur.brightness + step * BRIGHTNESS_STEP;
            if (bri > cur.max) bri = cur.max;
            if (bri < 0) bri = 0;
            l_bri = bri;
            l_bri_until = now() + LOCAL_HOLD;
            char cmd[CMDLEN];
            snprintf(cmd, sizeof cmd, "{\"op\":\"settings\",\"brightness\":%d}", bri);
            post(cmd);
            return;
        }
        ch_reset();
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) {
                if (!sel[r][c]) continue;
                uint8_t base[3], out[3];
                double level;
                if (!split(cur.boxes[r][c], base, &level)) {
                    static const uint8_t white[3] = {255, 255, 255};
                    memcpy(base, remembered[r][c].used ? remembered[r][c].c : white, 3);
                }
                level = round(fmin(1.0, fmax(0.0, level + step * 0.1)) * 10) / 10;
                if (level == 0) {
                    remembered[r][c].used = 1;
                    memcpy(remembered[r][c].c, base, 3);
                }
                scale_col(base, level, out);
                ch_set(r, c, out);
            }
        ch_apply();
        deselect_at = now() + DESELECT_AFTER;
    } else if (f == F_TEMPO_UP || f == F_TEMPO_DOWN) { /* Tempo der Effekte */
        int sp = cur.speed10 ? cur.speed10 : 10;
        sp = f == F_TEMPO_UP ? (int)lround(sp * 1.25 + 0.5) : (int)lround(sp / 1.25);
        if (sp > 50) sp = 50;
        if (sp < 1) sp = 1;
        l_speed10 = sp;
        l_speed_until = now() + LOCAL_HOLD;
        char cmd[CMDLEN];
        snprintf(cmd, sizeof cmd, "{\"op\":\"settings\",\"speed\":%.1f}", sp / 10.0);
        post(cmd);
    } else if (f == F_ALL) { /* alle auswählen, nochmal = alle abwählen */
        int all = n_sel == ROWS * COLS;
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) sel[r][c] = !all;
        n_sel = all ? 0 : ROWS * COLS;
        deselect_at = 0;
    } else if (f >= 4 && f < 4 + N_EFFECTS) { /* Effekt: an, oder aus wenn er schon läuft */
        const char *name = EFFECT_NAME[f - 4];
        int running = cur.mode >= 0 && cur.mode < N_ESP_EFFECTS && !strcmp(ESP_EFFECTS[cur.mode], name);
        char cmd[CMDLEN];
        snprintf(cmd, sizeof cmd, "{\"op\":\"settings\",\"mode\":\"%s\"}", running ? "static" : name);
        post(cmd);
        int m = -1;
        for (int k = 0; k < N_ESP_EFFECTS && !running; k++)
            if (!strcmp(ESP_EFFECTS[k], name)) m = k;
        set_local_mode(m);
        sel_clear();
        deselect_at = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Spiele (laufen hier, Bild geht an Launchpad und Decke)             */
/*  Spielfeld wie auf dem Launchpad gesehen: gy 0..8 oben->unten,      */
/*  gx 0..6 links->rechts                                              */
/* ------------------------------------------------------------------ */
#define GH ROWS
#define GW COLS
static int game = -1;              /* -1 kein Spiel, 0 Tetris, 1 Ball-Spiel */
#define N_GAMES 2
static uint8_t gpix[GH][GW][3];    /* aktuelles Spielbild */
static double game_over_until;      /* rotes Blinken nach "verloren" */
static unsigned rng_state = 12345;

static int rnd(int n) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (int)((rng_state >> 16) % (unsigned)n);
}

static void gset(int y, int x, int r, int g, int b) {
    if (y < 0 || y >= GH || x < 0 || x >= GW) return;
    gpix[y][x][0] = r; gpix[y][x][1] = g; gpix[y][x][2] = b;
}

/* ---------- Tetris ---------- */
static uint8_t tb[GH][GW];          /* 0 leer, sonst Farbnummer 1..7 */
static int tp, trot, tx, ty;        /* Teil, Drehung, Position (4x4-Box) */
static double t_next_fall;
static int t_lines;
static const uint16_t TSHAPE[7] = {0x0F00, 0x0660, 0x0E40, 0x06C0, 0x0C60, 0x08E0, 0x02E0}; /* I O T S Z J L */
static const uint8_t TCOL[8][3] = {{0, 0, 0}, {0, 255, 255}, {255, 210, 0}, {160, 0, 255},
                                    {0, 255, 0}, {255, 0, 0}, {0, 60, 255}, {255, 110, 0}};

static int t_cell(int p, int rot, int cx, int cy) { /* ist (cx,cy) in der 4x4-Box belegt? */
    int x = cx, y = cy;
    for (int k = 0; k < (rot & 3); k++) { /* rückwärts drehen */
        int nx = y, ny = 3 - x;
        x = nx; y = ny;
    }
    if (p == 1) return (TSHAPE[p] >> (15 - (cy * 4 + cx))) & 1; /* O dreht nicht */
    return (TSHAPE[p] >> (15 - (y * 4 + x))) & 1;
}

static int t_fits(int p, int rot, int px, int py) {
    for (int cy = 0; cy < 4; cy++)
        for (int cx = 0; cx < 4; cx++) {
            if (!t_cell(p, rot, cx, cy)) continue;
            int x = px + cx, y = py + cy;
            if (x < 0 || x >= GW || y >= GH) return 0;
            if (y >= 0 && tb[y][x]) return 0;
        }
    return 1;
}

static void t_spawn(void) {
    tp = rnd(7);
    trot = 0;
    tx = 1;
    ty = tp == 0 ? -1 : 0;
    if (!t_fits(tp, trot, tx, ty)) { /* kein Platz mehr: verloren */
        game_over_until = now() + 1.5;
    }
}

static void t_reset(void) {
    memset(tb, 0, sizeof tb);
    t_lines = 0;
    t_spawn();
    t_next_fall = now() + 0.8;
}

static void t_lock(void) {
    for (int cy = 0; cy < 4; cy++)
        for (int cx = 0; cx < 4; cx++)
            if (t_cell(tp, trot, cx, cy) && ty + cy >= 0) tb[ty + cy][tx + cx] = tp + 1;
    for (int y = GH - 1; y >= 0; y--) { /* volle Reihen entfernen */
        int full = 1;
        for (int x = 0; x < GW; x++)
            if (!tb[y][x]) full = 0;
        if (full) {
            memmove(tb[1], tb[0], sizeof(tb[0]) * y);
            memset(tb[0], 0, sizeof tb[0]);
            t_lines++;
            y++;
        }
    }
    t_spawn();
}

static double t_interval(void) { return fmax(0.15, 0.8 - t_lines * 0.04); }

static void t_key(int k) { /* 0 drehen, 1 links, 2 rechts, 3 runter, 4 fallen lassen */
    if (k == 0) {
        int nr = (trot + 1) & 3;
        if (t_fits(tp, nr, tx, ty)) trot = nr;
        else if (t_fits(tp, nr, tx - 1, ty)) { trot = nr; tx--; }
        else if (t_fits(tp, nr, tx + 1, ty)) { trot = nr; tx++; }
    } else if (k == 1 && t_fits(tp, trot, tx - 1, ty)) tx--;
    else if (k == 2 && t_fits(tp, trot, tx + 1, ty)) tx++;
    else if (k == 3) {
        if (t_fits(tp, trot, tx, ty + 1)) ty++;
        else t_lock();
        t_next_fall = now() + t_interval();
    } else if (k == 4) {
        while (t_fits(tp, trot, tx, ty + 1)) ty++;
        t_lock();
        t_next_fall = now() + t_interval();
    }
}

static void t_tick(double t) {
    if (t >= t_next_fall) {
        if (t_fits(tp, trot, tx, ty + 1)) ty++;
        else t_lock();
        t_next_fall = t + t_interval();
    }
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            const uint8_t *c = TCOL[tb[y][x]];
            gset(y, x, c[0], c[1], c[2]);
        }
    for (int cy = 0; cy < 4; cy++)
        for (int cx = 0; cx < 4; cx++)
            if (t_cell(tp, trot, cx, cy)) {
                const uint8_t *c = TCOL[tp + 1];
                gset(ty + cy, tx + cx, c[0], c[1], c[2]);
            }
}

/* ---------- Ball-Spiel: Blöcke rücken von oben nach, Bälle prallen ab ---------- */
#define MAX_BALLS 40
static int bhp[GH][GW];             /* Trefferpunkte der Blöcke (Reihe 8 bleibt frei) */
static int b_turn, b_hpmax;
static double b_lx;                 /* Abschussstelle (x, unten) */
static double b_first_x;
static int b_first_set;
static int b_flying;                /* Bälle unterwegs? */
static int b_n, b_launched;
static double b_next_launch, b_dirx, b_diry, b_last;
static double bx[MAX_BALLS], by[MAX_BALLS], bvx[MAX_BALLS], bvy[MAX_BALLS];
static int b_alive[MAX_BALLS];
#define BALL_SPEED 9.0
#define LAUNCH_Y (GH - 0.5)

static void b_new_row(void) {
    for (int y = GH - 2; y > 0; y--) memcpy(bhp[y], bhp[y - 1], sizeof bhp[0]);
    memset(bhp[0], 0, sizeof bhp[0]);
    int n = 2 + rnd(3) + (b_turn > 10 ? 1 : 0);
    for (int k = 0; k < n; k++) {
        int x = rnd(GW);
        bhp[0][x] = b_turn + (rnd(4) == 0 ? b_turn : 0);
        if (bhp[0][x] > b_hpmax) b_hpmax = bhp[0][x];
    }
}

static int b_lost(void) {
    for (int x = 0; x < GW; x++)
        if (bhp[GH - 2][x]) return 1;
    return 0;
}

static void b_reset(void) {
    memset(bhp, 0, sizeof bhp);
    b_turn = 1;
    b_hpmax = 1;
    b_lx = GW / 2.0;
    b_flying = 0;
    b_new_row();
    b_turn++;
    b_new_row();
}

static void b_aim(int gy, int gx) {
    if (b_flying) return;
    double dx = gx + 0.5 - b_lx, dy = gy + 0.5 - LAUNCH_Y;
    if (dy > -0.6) return; /* muss nach oben gehen */
    double len = sqrt(dx * dx + dy * dy);
    b_dirx = dx / len;
    b_diry = dy / len;
    b_n = b_turn < MAX_BALLS ? b_turn : MAX_BALLS;
    b_launched = 0;
    b_first_set = 0;
    b_flying = 1;
    b_next_launch = now();
    b_last = now();
    memset(b_alive, 0, sizeof b_alive);
}

static void b_step(double dt) {
    for (int k = 0; k < b_launched; k++) {
        if (!b_alive[k]) continue;
        double ox = bx[k], oy = by[k];
        double nx = ox + bvx[k] * dt, ny = oy + bvy[k] * dt;
        if (nx < 0.05) { nx = 0.05; bvx[k] = fabs(bvx[k]); }
        if (nx > GW - 0.05) { nx = GW - 0.05; bvx[k] = -fabs(bvx[k]); }
        if (ny < 0.05) { ny = 0.05; bvy[k] = fabs(bvy[k]); }
        if (ny >= LAUNCH_Y && bvy[k] > 0) { /* wieder unten angekommen */
            b_alive[k] = 0;
            if (!b_first_set) { b_first_set = 1; b_first_x = nx; }
            continue;
        }
        int cx = (int)nx, cy = (int)ny;
        if (cy >= 0 && cy < GH - 1 && cx >= 0 && cx < GW && bhp[cy][cx] > 0) {
            int ocx = (int)ox, ocy = (int)oy;
            if (ocx != cx) bvx[k] = -bvx[k];
            if (ocy != cy) bvy[k] = -bvy[k];
            if (ocx == cx && ocy == cy) bvy[k] = -bvy[k];
            bhp[cy][cx]--;
            nx = ox;
            ny = oy;
        }
        bx[k] = nx;
        by[k] = ny;
    }
}

static void b_tick(double t) {
    if (b_flying) {
        while (b_launched < b_n && t >= b_next_launch) {
            int k = b_launched++;
            bx[k] = b_lx; by[k] = LAUNCH_Y - 0.01;
            bvx[k] = b_dirx * BALL_SPEED; bvy[k] = b_diry * BALL_SPEED;
            b_alive[k] = 1;
            b_next_launch += 0.12;
        }
        double dt = t - b_last;
        if (dt > 0.1) dt = 0.1;
        b_last = t;
        for (int s = 0; s < 10; s++) b_step(dt / 10);
        int any = b_launched < b_n;
        for (int k = 0; k < b_launched; k++)
            if (b_alive[k]) any = 1;
        if (!any) { /* alle zurück: nächste Runde, alles rückt nach unten */
            b_flying = 0;
            if (b_first_set) b_lx = fmin(GW - 0.5, fmax(0.5, b_first_x));
            b_turn++;
            if (b_lost()) game_over_until = t + 1.5; /* Blöcke würden unten ankommen */
            else b_new_row();
        }
    }
    memset(gpix, 0, sizeof gpix);
    for (int y = 0; y < GH - 1; y++)
        for (int x = 0; x < GW; x++) {
            int hp = bhp[y][x];
            if (!hp) continue;
            double f = fmin(1.0, (double)hp / (b_turn > 1 ? b_turn : 1)); /* grün (wenig) -> rot (viel) */
            gset(y, x, (int)(255 * f), (int)(255 * (1 - f)), 0);
        }
    if (!b_flying) gset(GH - 1, (int)b_lx, 255, 255, 255); /* Abschussstelle */
    for (int k = 0; k < b_launched; k++)
        if (b_alive[k]) gset((int)by[k], (int)bx[k], 255, 255, 255);
}

/* ---------- gemeinsam ---------- */
static void game_start(int g) {
    game = g;
    rng_state = (unsigned)(now() * 1000);
    game_over_until = 0;
    if (g == 0) t_reset();
    else b_reset();
    sel_clear();
    deselect_at = 0;
}

static void game_stop(void) {
    game = -1;
    post("{\"op\":\"settings\",\"mode\":\"static\"}");
    set_local_mode(-1);
}

static void game_tick(double t) {
    if (game_over_until) {
        if (t < game_over_until) {
            int on = ((long)(t * 4)) % 2 == 0;
            for (int y = 0; y < GH; y++)
                for (int x = 0; x < GW; x++) gset(y, x, on ? 255 : 0, 0, 0);
            return;
        }
        game_start(game); /* nochmal von vorn */
    }
    if (game == 0) t_tick(t);
    else b_tick(t);
}

/* Spielbild an die Decke (höchstens ~12 Bilder pro Sekunde, nur bei Änderung) */
static double last_frame_sent;
static char last_frame_hex[ROWS * COLS * 6 + 1];

static void game_send(double t) {
    if (t - last_frame_sent < 0.08) return;
    char hex[ROWS * COLS * 6 + 1];
    int m = 0;
    for (int r = 0; r < ROWS; r++)       /* Decke: Reihe r, Spalte c; Launchpad ist gespiegelt */
        for (int c = 0; c < COLS; c++) {
            const uint8_t *p = gpix[r][COLS - 1 - c];
            m += sprintf(hex + m, "%02x%02x%02x", p[0], p[1], p[2]);
        }
    if (!strcmp(hex, last_frame_hex) && t - last_frame_sent < 2) return;
    memcpy(last_frame_hex, hex, sizeof hex);
    last_frame_sent = t;
    char cmd[CMDLEN];
    snprintf(cmd, sizeof cmd, "{\"op\":\"frame\",\"px\":\"%s\"}", hex);
    post_frame(cmd);
}

/* Taste im Spiel. Rückgabe 1 = vom Spiel verbraucht */
static int game_press(int i) {
    if (game < 0) return 0;
    if (game_over_until) return is_box[i] || color_of[i] >= 0;
    if (game == 0 && color_of[i] >= 0) {
        t_key(color_of[i]);
        return 1;
    }
    if (game == 1 && is_box[i]) {
        b_aim(box_r[i], COLS - 1 - box_c[i]);
        return 1;
    }
    return is_box[i] || color_of[i] >= 0; /* Kästen/Farben machen im Spiel sonst nichts */
}

static void on_press(int i) {
    if (i < 0 || i >= 128) return;
    if (func_of[i] == F_GAME) { /* Spiel an, nochmal = nächstes Spiel */
        game_start(game < 0 ? 0 : (game + 1) % N_GAMES);
        return;
    }
    if (game >= 0 && func_of[i] == F_OFF) { /* Aus beendet das Spiel */
        game_stop();
        return;
    }
    if (game_press(i)) return;
    if (is_box[i]) {
        if (deselect_at) { /* nach Helligkeit: neue Auswahl beginnt */
            sel_clear();
            deselect_at = 0;
        }
        int r = box_r[i], c = box_c[i];
        sel[r][c] = !sel[r][c];
        n_sel += sel[r][c] ? 1 : -1;
    } else if (color_of[i] >= 0) {
        on_color(color_of[i]);
        deselect_at = 0;
    } else if (func_of[i] >= 0) {
        if (game >= 0 && (func_of[i] == F_ON || func_of[i] == F_ALL || (func_of[i] >= 4 && func_of[i] < 4 + N_EFFECTS)))
            return; /* im Spiel ohne Wirkung */
        on_function(func_of[i]);
    }
}

/* Farbe der Decke fürs Launchpad: jede Helligkeitsstufe sieht dort sichtbar anders aus */
static void on_pad(const uint8_t c[3], double level, uint8_t out[3]) {
    int m = maxc(c);
    if (m == 0) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    double light = pow(m / 255.0 * (0.4 + 0.6 * level), 1.4);
    int top = (int)lround(63 * light);
    if (top < 1) top = 1;
    for (int k = 0; k < 3; k++) {
        int v = (int)ceil(lround((double)top * c[k] / m) * 255.0 / 63);
        out[k] = v > 255 ? 255 : v;
    }
}

static void set_led(int i, int r, int g, int b) {
    if (i < 0 || i >= 128) return;
    want[i] = 1;
    leds[i][0] = r; leds[i][1] = g; leds[i][2] = b;
}

static void app_main(void) {
    say("Mini-System: Launchpad-Programm startet");
    pthread_t th;
    pthread_create(&th, NULL, ble_thread, NULL);

    while (lp_open() < 0) msleep(20);
    if (lp_mk2) {
        uint8_t d[2] = {0x22, 0x00}; /* Session-Layout */
        lp_sysex(d, 2);
    } else {
        uint8_t d[2] = {0x0E, 0x01}; /* Programmer-Modus */
        lp_sysex(d, 2);
    }

    /* Belegung: Spalten 1-7 Kästen, Spalte 8 a-e Farben, f-i Effekte,
     * Spalte 9 b Hell+, c Hell-, d Tempo+, e Tempo-, f An, g Aus, h Alle, i Spiele */
    for (int i = 0; i < 128; i++) {
        color_of[i] = -1;
        func_of[i] = -1;
    }
    for (int r = 0; r < ROWS; r++) {
        int y = 9 - r;
        for (int c = 0; c < COLS; c++) {
            int i = led_index(c + 1, y);
            is_box[i] = 1;
            box_r[i] = r;
            box_c[i] = COLS - 1 - c; /* Launchpad ist zur Decke gespiegelt */
        }
        if (r < N_COLORS) color_of[led_index(8, y)] = r;
        else func_of[led_index(8, y)] = 4 + (r - N_COLORS); /* Effekte */
    }
    static const int col9[8] = {F_BRI_UP, F_BRI_DOWN, F_TEMPO_UP, F_TEMPO_DOWN, F_ON, F_OFF, F_ALL, F_GAME};
    for (int r = 1; r < ROWS; r++) func_of[led_index(9, 9 - r)] = col9[r - 1];

    int early[50], n_early = 0;
    double started = now(), last_warn = now() - 25;
    for (;;) {
        struct pollfd pf = {midi_fd, POLLIN, 0};
        poll(&pf, 1, 10);
        lp_read();
        double t = now();
        int sync = synced();
        pthread_mutex_lock(&mu);
        cur = g_state;
        double last_ok = g_last_ok;
        int ever = g_ever_online;
        pthread_mutex_unlock(&mu);
        /* eigene, noch nicht bestätigte Änderungen gelten vorerst */
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++)
                if (t < lb_until[r][c]) {
                    memcpy(cur.boxes[r][c], lb[r][c], 3);
                    if (cur.mode < 0) memcpy(cur.frame[r][c], lb[r][c], 3);
                }
        if (t < l_bri_until) cur.brightness = l_bri;
        if (t < l_speed_until) cur.speed10 = l_speed10;
        if (t < l_mode_until) cur.mode = l_mode;

        int ingame = game >= 0;
        if (n_early && (sync || ingame)) { /* Tasten von vor der Verbindung jetzt nachholen */
            for (int k = 0; k < n_early; k++) on_press(early[k]);
            n_early = 0;
        }
        for (int k = 0; k < n_presses; k++) {
            int i = presses[k];
            if (!sync && !ingame && !(i >= 0 && i < 128 && func_of[i] == F_GAME)) {
                if (n_early == 50) {
                    memmove(early, early + 1, sizeof(int) * 49);
                    n_early--;
                }
                early[n_early++] = i;
                continue;
            }
            on_press(i);
        }
        ingame = game >= 0;

        if (deselect_at && t >= deselect_at) {
            sel_clear();
            deselect_at = 0;
        }
        if (ever && !ingame && t - fmax(last_ok, started) > 60) {
            say("Decke weg, starte neu");
            exit(1);
        }
        memset(want, 0, sizeof want);
        int blink_on = ((long)(t * 3)) % 2 == 0;

        if (ingame) { /* Spiel: Feld direkt aufs Launchpad, Bild an die Decke */
            game_tick(t);
            game_send(t);
            for (int i = 0; i < 128; i++) {
                if (is_box[i]) {
                    const uint8_t *p = gpix[box_r[i]][COLS - 1 - box_c[i]];
                    set_led(i, p[0], p[1], p[2]);
                } else if (color_of[i] >= 0) {
                    static const uint8_t keys[5][3] = {{160, 0, 255}, {255, 255, 255}, {255, 255, 255}, {0, 60, 255}, {255, 0, 0}};
                    const uint8_t *c = keys[color_of[i]];
                    if (game == 0) set_led(i, c[0], c[1], c[2]); /* Tetris: drehen, links, rechts, runter, fallen */
                    else set_led(i, 0, 0, 0);
                } else if (func_of[i] >= 0) {
                    int f = func_of[i];
                    if (f == F_GAME) {
                        int on = blink_on;
                        if (game == 0) set_led(i, 0, on ? 255 : 60, on ? 255 : 60);
                        else set_led(i, on ? 255 : 60, on ? 120 : 30, 0);
                    } else if (f == F_OFF) set_led(i, 255, 0, 0);
                    else if (f == F_BRI_UP) set_led(i, 255, 255, 255);
                    else if (f == F_BRI_DOWN) set_led(i, 40, 40, 40);
                    else set_led(i, 0, 0, 0);
                }
            }
            lp_show();
            continue;
        }

        if (!sync) { /* warten: Kästen blinken langsam blau */
            if (t - last_warn > 30) {
                last_warn = t;
                say("Warte auf die Decke ...");
            }
            sel_clear();
            deselect_at = 0;
            int on = ((long)t) % 2 == 0;
            static const uint8_t stage_col[6][3] = {{60, 0, 0}, {0, 0, 40}, {40, 0, 40}, {50, 40, 0}, {0, 40, 40}, {40, 40, 40}};
            const uint8_t *sc = stage_col[g_stage % 6];
            for (int i = 0; i < 128; i++) {
                if (is_box[i]) set_led(i, on ? sc[0] : 0, on ? sc[1] : 0, on ? sc[2] : 0);
                else if (func_of[i] == F_GAME) set_led(i, 255, 0, 255); /* Spiele gehen auch ohne Decke */
                else if (color_of[i] >= 0 || func_of[i] >= 0) set_led(i, 0, 0, 0);
            }
            lp_show();
            continue;
        }

        double level = (double)cur.brightness / cur.max;
        for (int i = 0; i < 128; i++) {
            if (is_box[i]) {
                int r = box_r[i], c = box_c[i];
                uint8_t col[3];
                on_pad(cur.frame[r][c], level, col);
                if (sel[r][c]) {
                    if (!blink_on) col[0] = col[1] = col[2] = 0;
                    else if (maxc(col) == 0) col[0] = col[1] = col[2] = 60;
                }
                set_led(i, col[0], col[1], col[2]);
            } else if (color_of[i] >= 0) {
                const uint8_t *c = COLORS[color_of[i]];
                set_led(i, c[0], c[1], c[2]);
            } else if (func_of[i] >= 0) {
                int f = func_of[i];
                if (f == F_ON) set_led(i, 0, 255, 0);
                else if (f == F_OFF) set_led(i, 255, 0, 0);
                else if (f == F_BRI_UP) set_led(i, 255, 255, 255);
                else if (f == F_BRI_DOWN) set_led(i, 40, 40, 40);
                else if (f == F_TEMPO_UP) set_led(i, 0, 255, 255);
                else if (f == F_TEMPO_DOWN) set_led(i, 0, 50, 50);
                else if (f == F_ALL) set_led(i, n_sel == ROWS * COLS && !blink_on ? 0 : 255, n_sel == ROWS * COLS && !blink_on ? 0 : 200, 0);
                else if (f == F_GAME) set_led(i, 255, 0, 255);
                else {
                    const uint8_t *c = EFFECT_COLOR[f - 4];
                    int running = cur.mode >= 0 && cur.mode < N_ESP_EFFECTS &&
                                  !strcmp(ESP_EFFECTS[cur.mode], EFFECT_NAME[f - 4]);
                    if (running) set_led(i, blink_on ? c[0] : 0, blink_on ? c[1] : 0, blink_on ? c[2] : 0);
                    else set_led(i, c[0] * 35 / 100, c[1] * 35 / 100, c[2] * 35 / 100);
                }
            }
        }
        lp_show();
    }
}

/* ------------------------------------------------------------------ */
/*  /init: Grundsystem aufsetzen                                       */
/* ------------------------------------------------------------------ */
static void load_modules(void) {
    FILE *f = fopen("/modules.txt", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        int fd = open(line, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            say("Modul fehlt: %s", line);
            continue;
        }
        if (syscall(SYS_finit_module, fd, "", 0) < 0 && errno != EEXIST)
            say("Modul %s: %s", line, strerror(errno));
        close(fd);
    }
    fclose(f);
}

/* normal.txt auf der Boot-Partition? Dann normales Raspberry Pi OS starten.
 * Das System kann auf der SD-Karte (mmcblk0) oder einem USB-Stick (sda) liegen. */
static char root_dev[32];
static int have_update; /* neuere Programmversion vom Stick/SD geladen */

#ifdef __aarch64__
#define MY_BIN "led-mini-aarch64"
#else
#define MY_BIN "led-mini-armhf"
#endif

/* Liegt auf der Boot-Partition in led-mini/ eine andere Programmversion als die
 * eingebaute? Dann in den Arbeitsspeicher kopieren und die benutzen.
 * So reicht für Updates: neue Datei in den Ordner led-mini kopieren. */
static void load_update(void) {
    FILE *in = fopen("/boot/led-mini/" MY_BIN, "rb");
    if (!in) return;
    FILE *self = fopen("/init", "rb");
    FILE *out = fopen("/led-mini-neu", "wb");
    int differs = 0;
    long n = 0;
    char a[65536], b[65536];
    size_t k;
    while (out && (k = fread(a, 1, sizeof a, in)) > 0) {
        size_t k2 = self ? fread(b, 1, k, self) : 0;
        if (k2 != k || memcmp(a, b, k)) differs = 1;
        fwrite(a, 1, k, out);
        n += k;
    }
    if (self && fread(b, 1, 1, self) == 1) differs = 1;
    fclose(in);
    if (self) fclose(self);
    if (out) fclose(out);
    if (out && differs && n > 100000) {
        chmod("/led-mini-neu", 0755);
        have_update = 1;
        say("Neue Programmversion vom Stick geladen (%ld Bytes)", n);
    } else {
        unlink("/led-mini-neu");
    }
}

static int want_normal(void) {
    static const char *boot[] = {"/dev/mmcblk0p1", "/dev/sda1"};
    static const char *root[] = {"/dev/mmcblk0p2", "/dev/sda2"};
    mkdir("/boot", 0755);
    int which = -1;
    for (int k = 0; k < 500 && which < 0; k++) { /* bis 5 s warten (USB-Stick braucht länger) */
        for (int j = 0; j < 2; j++)
            if (access(boot[j], F_OK) == 0) { which = j; break; }
        if (which < 0) msleep(10);
    }
    if (which < 0 || mount(boot[which], "/boot", "vfat", MS_RDONLY, NULL) < 0) {
        say("Boot-Partition nicht lesbar");
        return 0;
    }
    snprintf(root_dev, sizeof root_dev, "%s", root[which]);
    load_update();
    int normal = 0;
    DIR *d = opendir("/boot");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (!strcasecmp(e->d_name, "normal.txt") || !strcasecmp(e->d_name, "normal.txt.txt") ||
                !strcasecmp(e->d_name, "normal")) normal = 1;
        closedir(d);
    }
    umount("/boot");
    return normal;
}

static void switch_to_normal(void) {
    say("normal.txt gefunden: starte normales Raspberry Pi OS von %s", root_dev);
    mkdir("/newroot", 0755);
    if (mount(root_dev, "/newroot", "ext4", MS_RDONLY, NULL) < 0) {
        say("Hauptsystem nicht mountbar: %s", strerror(errno));
        return;
    }
    mount("/dev", "/newroot/dev", NULL, MS_MOVE, NULL);
    umount2("/proc", MNT_DETACH);
    umount2("/sys", MNT_DETACH);
    if (chdir("/newroot") < 0) return;
    mount(".", "/", NULL, MS_MOVE, NULL);
    if (chroot(".") < 0) return;
    if (chdir("/") < 0) return;
    execl("/sbin/init", "init", (char *)NULL);
    say("/sbin/init startet nicht: %s", strerror(errno));
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (getpid() != 1) { /* zum Testen im normalen System: nur das Programm */
        app_main();
        return 0;
    }
    mkdir("/dev", 0755);
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    int con = open("/dev/console", O_RDWR);
    if (con >= 0) {
        dup2(con, 0); dup2(con, 1); dup2(con, 2);
        if (con > 2) close(con);
    }
    say("LED-Decke Mini-System");
    load_modules();

    pid_t app = fork();
    if (app == 0) {
        app_main();
        _exit(1);
    }
    int normal = want_normal();
    if (have_update && !normal) { /* auf die neue Version umsteigen */
        kill(app, SIGKILL);
        waitpid(app, NULL, 0);
        app = -1;
    }
    if (normal) {
        kill(app, SIGKILL);
        waitpid(app, NULL, 0);
        switch_to_normal();
        /* hat nicht geklappt: Mini-System weiterlaufen lassen */
        app = -1;
    }
    for (;;) { /* Programm bei Absturz/Neustart-Wunsch sofort neu starten */
        int st;
        pid_t p = app > 0 ? waitpid(-1, &st, 0) : -1;
        if (app <= 0 || p == app) {
            if (app > 0) msleep(300);
            app = fork();
            if (app == 0) {
                if (have_update) execl("/led-mini-neu", "led-mini", (char *)NULL);
                app_main();
                _exit(1);
            }
        }
    }
}
