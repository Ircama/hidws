/*
 * hidws v1.1.0 — WebSocket ↔ HID bridge
 *
 * Uses libwebsockets for WebSocket server, hidapi (libusb) for HID access.
 *
 * Run:    ./hidws [port]       (default: 9001)
 *
 * Wire protocol (JSON over WebSocket):
 *   Client → Server:  {"cmd":"list"}
 *                      {"cmd":"open","vendorId":<int>,"productId":<int>}
 *                      {"cmd":"send_report","reportId":<int>,"data":[...]}
 *                      {"cmd":"send_feature_report","reportId":<int>,"data":[...]}
 *                      {"cmd":"close"}
 *   Server → Client:  {"type":"device_list","devices":[...]}
 *                      {"type":"opened","vendorId":...,"productId":...,"productName":"..."}
 *                      {"type":"input_report","reportId":<int>,"data":[...]}
 *                      {"type":"ok"}
 *                      {"type":"error","message":"..."}
 *                      {"type":"closed"}
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdarg.h>

#include <hidapi/hidapi.h>
#include <libwebsockets.h>

/* ──────────────────────────── Version ──────────────────────────────── */
#define HIDWS_VERSION "1.1.0"

/* ──────────────────────────── Configuration ──────────────────────────── */
#ifndef PORT_DEFAULT
#define PORT_DEFAULT 9001
#endif
#ifndef MAX_MSG_SIZE
#define MAX_MSG_SIZE 4096
#endif
#ifndef HID_READ_BUF
#define HID_READ_BUF 1024
#endif

/* ──────────────────── Per-connection user data ──────────────────────── */
struct per_session_data {
    bool opened;
};

/* ──────────────────── HID device state ──────────────────────────────── */
static hid_device *g_hid_handle = NULL;
static int g_hid_vendor_id = 0;
static int g_hid_product_id = 0;
static char *g_hid_product_string = NULL;
static bool g_hid_open = false;
static pthread_mutex_t g_hid_write_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t g_hid_thread = (pthread_t)0;
static volatile bool g_hid_thread_running = false;

/* ──────────────────── Global state ──────────────────────────────────── */
static struct lws_context *g_context = NULL;
static volatile bool g_running = true;

static pthread_mutex_t g_bcast_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_bcast_msg = NULL;

/* ──────────────────── JSON helpers ──────────────────────────────────── */

static char *json_escape(const char *s) {
    size_t len = strlen(s);
    size_t cap = len * 2 + 3;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  out[j++] = '\\'; out[j++] = '"';  break;
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
        case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
        case '\t': out[j++] = '\\'; out[j++] = 't';  break;
        default:
            if (c < 0x20) {
                j += snprintf(out + j, cap - j, "\\u%04x", c);
            } else {
                out[j++] = c;
            }
            break;
        }
        if (j >= cap - 8) { cap *= 2; out = realloc(out, cap); }
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}

static char *json_bytes(const uint8_t *data, int len) {
    char *out = malloc((size_t)len * 6 + 4);
    if (!out) return NULL;
    out[0] = '[';
    int pos = 1;
    for (int i = 0; i < len; i++)
        pos += snprintf(out + pos, 6, "%s%u", i ? "," : "", (unsigned)data[i]);
    out[pos++] = ']';
    out[pos] = '\0';
    return out;
}

/* ──────────────────── Send / broadcast ─────────────────────────────── */

static void send_json_text(struct lws *wsi, const char *msg) {
    size_t len = strlen(msg);
    unsigned char *buf = (unsigned char *)malloc(LWS_PRE + len);
    if (!buf) return;
    memcpy(buf + LWS_PRE, msg, len);
    lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
    free(buf);
}

static void broadcast_text(const char *json) {
    if (!g_context) return;
    pthread_mutex_lock(&g_bcast_lock);
    free(g_bcast_msg);
    g_bcast_msg = strdup(json);
    pthread_mutex_unlock(&g_bcast_lock);
    lws_cancel_service(g_context);
}

static void broadcast_text_fmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = NULL;
    int n = vasprintf(&msg, fmt, ap);
    va_end(ap);
    if (n >= 0 && msg) { broadcast_text(msg); free(msg); }
}

/* Forward declarations */
static void *hid_read_worker(void *arg);

/* ──────────────────── HID command handlers ──────────────────────────── */

static void cmd_list(struct lws *wsi) {
    struct hid_device_info *devs = hid_enumerate(0, 0);
    int count = 0;
    for (struct hid_device_info *cur = devs; cur; cur = cur->next) count++;

    size_t cap = 256 + (size_t)count * 256;
    char *json = malloc(cap);
    if (!json) { hid_free_enumeration(devs); return; }

    size_t pos = snprintf(json, cap, "{\"type\":\"device_list\",\"devices\":[");
    int idx = 0;
    for (struct hid_device_info *cur = devs; cur; cur = cur->next) {
        if (idx > 0) json[pos++] = ',';
        /* Convert wchar_t product string to char */
        char prod_buf[256] = "";
        if (cur->product_string)
            wcstombs(prod_buf, cur->product_string, sizeof(prod_buf) - 1);
        else
            snprintf(prod_buf, sizeof(prod_buf), "%s", "Unknown");
        char *ename = json_escape(prod_buf);
        if (!ename) continue;
        int n = snprintf(json + pos, cap - pos,
            "{\"vendorId\":%d,\"productId\":%d,\"productName\":%s}",
            cur->vendor_id, cur->product_id, ename);
        free(ename);
        if (n > 0) pos += n;
        idx++;
    }
    snprintf(json + pos, cap - pos, "]}");
    send_json_text(wsi, json);
    free(json);
    hid_free_enumeration(devs);
}

static void cmd_open(struct lws *wsi, int vendor_id, int product_id) {
    if (g_hid_open) {
        send_json_text(wsi, "{\"type\":\"error\",\"message\":\"Already open. Close first.\"}");
        return;
    }

    g_hid_handle = hid_open(vendor_id, product_id, NULL);
    if (!g_hid_handle) {
        send_json_text(wsi, "{\"type\":\"error\",\"message\":\"Device not found or permission denied\"}");
        return;
    }

    g_hid_vendor_id = vendor_id;
    g_hid_product_id = product_id;

    wchar_t wstr[256];
    free(g_hid_product_string);
    g_hid_product_string = NULL;
    if (hid_get_product_string(g_hid_handle, wstr, sizeof(wstr)/2) == 0) {
        size_t needed = wcslen(wstr) * 4 + 1;
        g_hid_product_string = malloc(needed);
        if (g_hid_product_string)
            wcstombs(g_hid_product_string, wstr, needed - 1);
    } else {
        g_hid_product_string = strdup("Unknown");
    }
    g_hid_open = true;

    if (!g_hid_thread_running) {
        g_hid_thread_running = true;
        if (pthread_create(&g_hid_thread, NULL, hid_read_worker, NULL) != 0) {
            fprintf(stderr, "[hid] Failed to create reader thread\n");
            g_hid_thread_running = false;
        }
    }

    char *pname = json_escape(g_hid_product_string ? g_hid_product_string : "Unknown");
    char resp[512];
    snprintf(resp, sizeof(resp),
        "{\"type\":\"opened\",\"vendorId\":%d,\"productId\":%d,\"productName\":%s}",
        vendor_id, product_id, pname ? pname : "\"Unknown\"");
    free(pname);
    send_json_text(wsi, resp);
}

static void cmd_send_report(int report_id, const uint8_t *data, int datalen) {
    if (!g_hid_open || !g_hid_handle) return;
    uint8_t *buf = malloc((size_t)datalen + 1);
    if (!buf) return;
    buf[0] = (uint8_t)report_id;
    if (datalen > 0) memcpy(buf + 1, data, (size_t)datalen);
    pthread_mutex_lock(&g_hid_write_lock);
    int n = hid_write(g_hid_handle, buf, (size_t)datalen + 1);
    pthread_mutex_unlock(&g_hid_write_lock);
    free(buf);
    broadcast_text_fmt("{\"type\":\"%s\"}",
        n < 0 ? "error\",\"message\":\"hid_write failed" : "ok");
}

static void cmd_send_feature_report(int report_id, const uint8_t *data, int datalen) {
    if (!g_hid_open || !g_hid_handle) return;
    uint8_t *buf = malloc((size_t)datalen + 1);
    if (!buf) return;
    buf[0] = (uint8_t)report_id;
    if (datalen > 0) memcpy(buf + 1, data, (size_t)datalen);
    pthread_mutex_lock(&g_hid_write_lock);
    int n = hid_send_feature_report(g_hid_handle, buf, (size_t)datalen + 1);
    pthread_mutex_unlock(&g_hid_write_lock);
    free(buf);
    broadcast_text_fmt("{\"type\":\"%s\"}",
        n < 0 ? "error\",\"message\":\"hid_send_feature_report failed" : "ok");
}

static void close_hid(void) {
    if (!g_hid_open) return;
    g_hid_open = false;
    g_hid_thread_running = false;
    if (g_hid_thread != (pthread_t)0) {
        pthread_join(g_hid_thread, NULL);
        g_hid_thread = (pthread_t)0;
    }
    if (g_hid_handle) { hid_close(g_hid_handle); g_hid_handle = NULL; }
    free(g_hid_product_string);
    g_hid_product_string = NULL;
}

static void cmd_close(void) {
    close_hid();
    broadcast_text("{\"type\":\"closed\"}");
}

/* ──────────────────── JSON command parsing ──────────────────────────── */

static void process_command(struct lws *wsi, const char *json) {
    /* Extract "cmd" string value */
    char *cmd = NULL;
    {
        const char *p = strstr(json, "\"cmd\"");
        if (p) {
            p = strchr(p + 5, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    const char *end = strchr(p, '"');
                    if (end) {
                        size_t clen = (size_t)(end - p);
                        cmd = malloc(clen + 1);
                        if (cmd) { memcpy(cmd, p, clen); cmd[clen] = '\0'; }
                    }
                }
            }
        }
    }
    if (!cmd) return;

    /* Inline helpers for extracting numbers and arrays */
    int get_int(const char *key) {
        char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
        const char *p = strstr(json, pat);
        if (!p) return 0;
        p = strchr(p + strlen(pat), ':');
        if (!p) return 0;
        p++; while (*p == ' ' || *p == '\t') p++;
        return (int)strtol(p, NULL, 10);
    }

    int get_arr(const char *key, uint8_t *buf, int cap) {
        char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
        const char *p = strstr(json, pat);
        if (!p) return -1;
        p = strchr(p + strlen(pat), ':');
        if (!p) return -1;
        p++; while (*p == ' ' || *p == '\t') p++;
        if (*p != '[') return -1;
        p++;
        int cnt = 0;
        while (*p && *p != ']' && cnt < cap) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (*p == ']') break;
            buf[cnt++] = (uint8_t)strtol(p, (char**)&p, 10);
        }
        return cnt;
    }

    if (strcmp(cmd, "list") == 0) {
        fprintf(stderr, "[hid] Command: list\n");
        cmd_list(wsi);
    } else if (strcmp(cmd, "open") == 0) {
        int vid = get_int("vendorId"), pid = get_int("productId");
        fprintf(stderr, "[hid] Command: open VID=0x%04X PID=0x%04X\n", vid, pid);
        cmd_open(wsi, vid, pid);
    } else if (strcmp(cmd, "send_report") == 0) {
        int rid = get_int("reportId");
        fprintf(stderr, "[hid] Command: send_report reportId=%d\n", rid);
        uint8_t data[2048];
        int n = get_arr("data", data, sizeof(data));
        if (n >= 0) cmd_send_report(rid, data, n);
    } else if (strcmp(cmd, "send_feature_report") == 0) {
        int rid = get_int("reportId");
        fprintf(stderr, "[hid] Command: send_feature_report reportId=%d\n", rid);
        uint8_t data[2048];
        int n = get_arr("data", data, sizeof(data));
        if (n >= 0) cmd_send_feature_report(rid, data, n);
    } else if (strcmp(cmd, "close") == 0) {
        fprintf(stderr, "[hid] Command: close\n");
        cmd_close();
    } else {
        fprintf(stderr, "[hid] Command: UNKNOWN '%s'\n", cmd);
        char err[256];
        snprintf(err, sizeof(err),
            "{\"type\":\"error\",\"message\":\"Unknown command: %s\"}", cmd);
        send_json_text(wsi, err);
    }
    free(cmd);
}

/* ──────────────────── HID reader worker thread ──────────────────────── */

static void *hid_read_worker(void *arg) {
    (void)arg;
    uint8_t buf[HID_READ_BUF];
    fprintf(stderr, "[hid] Reader thread started\n");
    while (g_hid_thread_running) {
        if (!g_hid_open || !g_hid_handle) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
            nanosleep(&ts, NULL);
            continue;
        }
        int n = hid_read_timeout(g_hid_handle, buf, sizeof(buf), 100);
        if (n < 0) { fprintf(stderr, "[hid] Read error\n"); break; }
        if (n == 0) continue;
        int report_id = (n > 0) ? buf[0] : 0;
        char *bstr = json_bytes(buf, n);
        if (bstr) {
            broadcast_text_fmt(
                "{\"type\":\"input_report\",\"reportId\":%d,\"data\":%s}",
                report_id, bstr);
            free(bstr);
        }
    }
    fprintf(stderr, "[hid] Reader thread stopped\n");
    return NULL;
}

/* ──────────────────── libwebsockets protocol callback ────────────────── */

static int hidws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *in, size_t len)
{
    struct per_session_data *psd = (struct per_session_data *)user;

    switch (reason) {

    case LWS_CALLBACK_ESTABLISHED:
        psd->opened = true;
        fprintf(stderr, "[ws] Client connected\n");
        break;

    case LWS_CALLBACK_RECEIVE:
        if (in && len > 0) {
            /* Log the received command (truncated to 200 chars) */
            size_t loglen = len < 200 ? len : 200;
            fprintf(stderr, "[ws] CMD received: %.*s%s\n",
                (int)loglen, (char *)in, loglen < len ? "..." : "");
            char *buf = malloc(len + 1);
            if (buf) {
                memcpy(buf, in, len);
                buf[len] = '\0';
                process_command(wsi, buf);
                free(buf);
            }
        }
        break;

    case LWS_CALLBACK_CLOSED:
        psd->opened = false;
        fprintf(stderr, "[ws] Client disconnected\n");
        /* Release the HID device so it is not left open after the client
         * closes the web app; otherwise only a kill would free it. */
        close_hid();
        break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
        pthread_mutex_lock(&g_bcast_lock);
        if (g_bcast_msg) {
            size_t msglen = strlen(g_bcast_msg);
            fprintf(stderr, "[ws] Sending: %.*s\n",
                (int)(msglen < 200 ? msglen : 200), g_bcast_msg);
            unsigned char *out = malloc(LWS_PRE + msglen);
            if (out) {
                memcpy(out + LWS_PRE, g_bcast_msg, msglen);
                lws_write(wsi, out + LWS_PRE, msglen, LWS_WRITE_TEXT);
                free(out);
            }
        }
        pthread_mutex_unlock(&g_bcast_lock);
        break;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        lws_callback_on_writable_all_protocol(lws_get_context(wsi),
            lws_get_protocol(wsi));
        break;

    default:
        break;
    }
    return 0;
}

/* ──────────────────── Protocol list ──────────────────────────────────── */

static struct lws_protocols protocols[] = {
    {
        .name                  = "hidws",
        .callback              = hidws_callback,
        .per_session_data_size = sizeof(struct per_session_data),
        .rx_buffer_size        = MAX_MSG_SIZE,
    },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

/* ──────────────────── Signal handler ────────────────────────────────── */

static void sigint_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\n[server] Shutting down...\n");
    g_running = false;
}

/* ──────────────────── Entry point ───────────────────────────────────── */

int main(int argc, char **argv) {
    int port = PORT_DEFAULT;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Usage: %s [port]\n", argv[0]);
            return 1;
        }
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    if (hid_init() != 0) {
        fprintf(stderr, "[hid] Failed to initialize hidapi\n");
        return 1;
    }

    /* Create libwebsockets context */
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = port;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    g_context = lws_create_context(&info);
    if (!g_context) {
        fprintf(stderr, "[server] Failed to create libwebsockets context\n");
        hid_exit();
        return 1;
    }

    fprintf(stderr, "[server] hidws v%s listening on 0.0.0.0:%d\n",
            HIDWS_VERSION, port);

    while (g_running && g_context)
        lws_service(g_context, 500);

    if (g_hid_open) cmd_close();
    lws_context_destroy(g_context);
    hid_exit();
    fprintf(stderr, "[server] Bye.\n");
    return 0;
}
