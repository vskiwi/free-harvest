/*
 * hr_dryerfiles - see hr_dryerfiles.h.
 */
#include "hr_dryerfiles.h"

#include "hr_capture.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "hr_files";

#define NVS_NS   "hrfiles"
#define NVS_KEY  "on"

/* Cached copies live beside the capture log. SPIFFS is flat and caps names
 * at 31 characters after the mount point, so the prefix is two bytes and
 * the dryer's 24-character CSV names fit with room to spare. */
#define MOUNT      "/capture"
#define CACHE_PFX  "d."
#define PART_PATH  MOUNT "/d.part"
#define PATH_MAX_  (sizeof(MOUNT) + 1 + 31 + 1)
/* Free space we insist on keeping for the capture log itself. */
#define KEEP_FREE  (64 * 1024)

static hr_session_t *s_session;
static SemaphoreHandle_t s_lock;
static hr_files_t s_fs;
static bool s_enabled;

/* The stream's side buffer for whole FDFILEBLOCK frames (hr_big_cb). */
static char s_big[HR_FILES_BIGBUF];

/*
 * One block, handed from the USB RX task (the sink) to the main loop (the
 * writer). The state machine answers WAIT for it and asks for the next
 * block only after hr_files_resume(), so one slot is always enough.
 */
static struct {
    bool full;
    long block;
    long nbytes;
    char data[HR_FDBLOCK_SIZE];
} s_mail;

/* Last list from the dryer. */
static hr_df_entry_t s_list[HR_DF_LIST_MAX];
static unsigned s_nlist;
static bool s_list_valid;
static unsigned long s_list_done_ms;

/* The read in progress: where its bytes go, and whether they got there. */
static char s_target[HR_FILES_NAME_MAX];
static bool s_writing;          /* d.part is open for this transfer */
static bool s_write_failed;
static bool s_finish_pending;   /* DONE arrived; main loop must rename */
static bool s_link_up;
static bool s_running;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static unsigned long now_ms(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------ */
/* Names and paths                                                     */
/* ------------------------------------------------------------------ */
/*
 * A name we are willing to send and to store. The wire form is one
 * space-delimited argument, the reply header is comma-delimited, SPIFFS
 * wants it short, and a path separator has no business in a file name.
 */
static bool name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    size_t n = strlen(name);
    if (n >= HR_FILES_NAME_MAX || n + sizeof(CACHE_PFX) - 1 > 31) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c <= 0x20 || c >= 0x7f || c == ',' || c == '"' || c == '/' ||
            c == '\\' || c == '\'' || c == '*' || c == '?') {
            return false;
        }
    }
    return true;
}

static void cache_path(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, MOUNT "/" CACHE_PFX "%.29s", name);
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Walk the cache: count entries, optionally copy names, optionally find a
 * victim that is not `keep`. All under fs_enter by the caller. */
static unsigned cache_scan(char names[][HR_FILES_NAME_MAX], unsigned cap,
                           const char *keep, char *victim, size_t victim_cap)
{
    unsigned n = 0;
    DIR *d = opendir(MOUNT);
    if (d == NULL) {
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, CACHE_PFX, sizeof(CACHE_PFX) - 1) != 0) {
            continue;
        }
        const char *nm = e->d_name + sizeof(CACHE_PFX) - 1;
        if (strcmp(nm, "part") == 0) {
            continue;
        }
        if (names != NULL && n < cap) {
            snprintf(names[n], HR_FILES_NAME_MAX, "%.47s", nm);
        }
        if (victim != NULL && victim[0] == '\0' &&
            (keep == NULL || strcmp(nm, keep) != 0)) {
            snprintf(victim, victim_cap, "%.47s", nm);
        }
        n++;
    }
    closedir(d);
    return n;
}

/* ------------------------------------------------------------------ */
/* hr_files callbacks                                                  */
/* ------------------------------------------------------------------ */
static bool cb_send(const char *verb, const char *a1, const char *a2, void *u)
{
    (void)u;
    hr_builder_t b;
    hr_build_begin(&b, verb);
    hr_build_str(&b, a1);
    if (a2 != NULL) {
        hr_build_str(&b, a2);
    }
    /* Plaintext on every firmware: 6.0.644170 encodes only what it SENDS
     * (docs/20 §7), so FDFILES / FILEREAD go out exactly like FDNAME. */
    return hr_session_send(s_session, &b);
}

/* USB RX task, under s_lock (the caller took it). Copy, never write flash. */
static int cb_sink(const hr_fdblock_t *b, void *u)
{
    (void)u;
    if (s_mail.full) {
        return HR_FILES_SINK_FAIL; /* cannot happen: the machine waits */
    }
    s_mail.block = b->block;
    s_mail.nbytes = b->nbytes;
    memcpy(s_mail.data, b->data, (size_t)b->nbytes);
    hr_fdblock_unbel(s_mail.data, (size_t)b->nbytes);
    s_mail.full = true;

    /* A one-line note per block in the capture, since the data itself never
     * goes through the frame path. */
    char note[96];
    snprintf(note, sizeof(note), "FDFILEBLOCK,%s,%ld,%ld,%ld,sum=%02X ok",
             b->name, b->block, b->nbytes, b->size, b->sum);
    hr_capture_append_dir((uint32_t)now_ms(), HR_CAP_DIR_RX, note);
    return HR_FILES_SINK_WAIT;
}

static void cb_list(const hr_fdlist_entry_t *e, void *u)
{
    (void)u;
    if (e->end) {
        s_list_valid = true;
        s_list_done_ms = now_ms();
        return;
    }
    if (s_nlist < HR_DF_LIST_MAX) {
        snprintf(s_list[s_nlist].name, sizeof(s_list[s_nlist].name), "%s",
                 e->name);
        s_list[s_nlist].size = e->size;
        s_list[s_nlist].cached = false; /* filled in by the snapshot */
        s_nlist++;
    }
}

static void cb_done(hr_files_state_t st, hr_files_err_t err, void *u)
{
    (void)u;
    if (s_target[0] != '\0') {
        /* a read ended; the main loop closes the file out */
        s_finish_pending = true;
    }
    ESP_LOGI(TAG, "transfer %s%s%s (%lu requests, %lu blocks, %lu bad, "
                  "%lu timeouts)",
             hr_files_state_str(st), err ? ": " : "", hr_files_err_str(err),
             s_fs.requests, s_fs.blocks_ok, s_fs.blocks_bad, s_fs.timeouts);
    hr_capture_event("files %s %s %s", hr_files_state_str(st),
                     hr_files_err_str(err), s_target);
}

/* ------------------------------------------------------------------ */
/* Session hook: whole blocks from the stream's side buffer            */
/* ------------------------------------------------------------------ */
static void on_block(const char *frame, size_t len, void *user)
{
    (void)user;
    LOCK();
    hr_files_on_block(&s_fs, frame, len, now_ms());
    UNLOCK();
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */
void hr_dryerfiles_init(hr_session_t *s)
{
    s_session = s;
    s_lock = xSemaphoreCreateMutex();
    hr_files_init(&s_fs, cb_send, NULL);
    hr_files_set_sink(&s_fs, cb_sink, NULL);
    hr_files_set_list_cb(&s_fs, cb_list, NULL);
    hr_files_set_done_cb(&s_fs, cb_done, NULL);

#ifdef CONFIG_HR_BATCH_HISTORY_DEFAULT_ON
    bool on = true;
#else
    bool on = false;
#endif
    const char *src = "kconfig";
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(nh, NVS_KEY, &v) == ESP_OK) {
            on = (v != 0);
            src = "nvs";
        }
        nvs_close(nh);
    }
    s_enabled = on;

    /* The side buffer is lent regardless of the switch: with it the stream
     * collects blocks whole; without it, it swallows them. Either way no
     * block data can ever be mistaken for frames. Requests are what the
     * switch gates. */
    hr_session_set_block_sink(s, s_big, sizeof(s_big), on_block, NULL);
    ESP_LOGI(TAG, "batch history (dryer files): %s (%s); side buffer %u B",
             on ? "ON" : "off", src, (unsigned)sizeof(s_big));
}

bool hr_dryerfiles_enabled(void)
{
    return s_enabled;
}

bool hr_dryerfiles_set_enabled(bool on)
{
    LOCK();
    s_enabled = on;
    if (!on && hr_files_busy(&s_fs)) {
        hr_files_cancel(&s_fs, HR_FILES_ERR_CANCEL, now_ms());
    }
    UNLOCK();
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed; setting not persisted");
        return false;
    }
    bool ok = nvs_set_u8(nh, NVS_KEY, on ? 1 : 0) == ESP_OK &&
              nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    ESP_LOGW(TAG, "batch history set %s%s", on ? "ON" : "off",
             ok ? "" : " (NOT persisted)");
    return ok;
}

void hr_dryerfiles_on_frame(const hr_frame_t *f)
{
    if (f == NULL || strcmp(f->verb, "FDFILELIST") != 0 || s_lock == NULL) {
        return;
    }
    LOCK();
    hr_files_on_frame(&s_fs, f, now_ms());
    UNLOCK();
}

/* Main loop. Flash work happens here, outside the lock. */
void hr_dryerfiles_tick(unsigned long t, bool link_up, bool dryer_running)
{
    if (s_lock == NULL) {
        return;
    }
    /* 1. a block waiting to be written */
    bool have_block = false;
    long nbytes = 0;
    static char data[HR_FDBLOCK_SIZE];
    LOCK();
    s_link_up = link_up;
    s_running = dryer_running;
    if (s_mail.full) {
        have_block = true;
        nbytes = s_mail.nbytes;
        memcpy(data, s_mail.data, (size_t)nbytes);
    }
    UNLOCK();

    if (have_block) {
        bool ok = false;
        if (hr_capture_fs_enter()) {
            FILE *f = fopen(PART_PATH, s_writing ? "ab" : "wb");
            if (f != NULL) {
                ok = fwrite(data, 1, (size_t)nbytes, f) == (size_t)nbytes;
                fclose(f);
            } else {
                ESP_LOGE(TAG, "open %s failed: %s", PART_PATH, strerror(errno));
            }
            hr_capture_fs_leave();
        }
        s_writing = true;
        LOCK();
        s_mail.full = false;
        if (ok) {
            hr_files_resume(&s_fs, t);
        } else {
            s_write_failed = true;
            hr_files_cancel(&s_fs, HR_FILES_ERR_SINK, t);
        }
        UNLOCK();
    }

    /* 2. drive the state machine (requests, timeouts, link rule) */
    LOCK();
    hr_files_tick(&s_fs, t, link_up);
    bool finish = s_finish_pending;
    bool complete = (s_fs.state == HR_FILES_DONE);
    char target[HR_FILES_NAME_MAX];
    snprintf(target, sizeof(target), "%s", s_target);
    if (finish) {
        s_finish_pending = false;
        s_target[0] = '\0';
    }
    UNLOCK();

    /* 3. close out a finished read: keep it under its name, or drop it */
    if (finish) {
        if (hr_capture_fs_enter()) {
            if (complete && s_writing && !s_write_failed) {
                char dst[PATH_MAX_];
                cache_path(target, dst, sizeof(dst));
                remove(dst);
                if (rename(PART_PATH, dst) != 0) {
                    ESP_LOGE(TAG, "rename to %s failed: %s", dst,
                             strerror(errno));
                    remove(PART_PATH);
                } else {
                    ESP_LOGI(TAG, "cached %s (%ld bytes)", target,
                             s_fs.received);
                }
            } else {
                remove(PART_PATH);
            }
            hr_capture_fs_leave();
        }
        s_writing = false;
        s_write_failed = false;
    }
}

static hr_df_result_t precheck(bool force)
{
    if (!s_enabled) {
        return HR_DF_DISABLED;
    }
    if (hr_files_busy(&s_fs) || s_mail.full || s_finish_pending) {
        return HR_DF_BUSY;
    }
    if (!s_link_up) {
        return HR_DF_LINK;
    }
    if (s_running && !force) {
        return HR_DF_RUNNING;
    }
    return HR_DF_OK;
}

hr_df_result_t hr_dryerfiles_refresh(const char *pattern, bool force)
{
    if (s_lock == NULL) {
        return HR_DF_DISABLED;
    }
    if (pattern == NULL || pattern[0] == '\0') {
        pattern = ".csv";
    }
    LOCK();
    hr_df_result_t r = precheck(force);
    if (r == HR_DF_OK) {
        s_nlist = 0;
        s_list_valid = false;
        s_target[0] = '\0';
        if (!hr_files_start_list(&s_fs, pattern, now_ms())) {
            r = HR_DF_BADNAME;
        }
    }
    UNLOCK();
    if (r == HR_DF_OK) {
        ESP_LOGW(TAG, "asking the dryer for its file list (%s)%s", pattern,
                 force ? " [forced]" : "");
        hr_capture_event("files list %s%s", pattern, force ? " forced" : "");
    }
    return r;
}

hr_df_result_t hr_dryerfiles_fetch(const char *name, bool force)
{
    if (s_lock == NULL) {
        return HR_DF_DISABLED;
    }
    if (!name_ok(name)) {
        return HR_DF_BADNAME;
    }
    /* size from the list, if we have it */
    long expected = -1;
    LOCK();
    hr_df_result_t r = precheck(force);
    for (unsigned i = 0; r == HR_DF_OK && i < s_nlist; i++) {
        if (strcmp(s_list[i].name, name) == 0) {
            expected = s_list[i].size;
        }
    }
    UNLOCK();
    if (r != HR_DF_OK) {
        return r;
    }
    if (expected > HR_FILES_MAX_SIZE) {
        return HR_DF_TOOBIG;
    }

    /* Room on the partition, and a slot in the cache. Flash work: outside
     * the lock, before the transfer is started. */
    if (!hr_capture_ready() || !hr_capture_fs_enter()) {
        return HR_DF_STORAGE;
    }
    size_t total = 0, used = 0;
    if (esp_spiffs_info("capture", &total, &used) != ESP_OK) {
        hr_capture_fs_leave();
        return HR_DF_STORAGE;
    }
    size_t need = (expected > 0 ? (size_t)expected : HR_FILES_MAX_SIZE) +
                  KEEP_FREE;
    char victim[HR_FILES_NAME_MAX] = "";
    unsigned have = cache_scan(NULL, 0, name, victim, sizeof(victim));
    char own[PATH_MAX_];
    cache_path(name, own, sizeof(own));
    bool replacing = file_exists(own);
    while ((have >= HR_DF_CACHE_MAX && !replacing) ||
           (total > used && total - used < need)) {
        if (victim[0] == '\0') {
            break;
        }
        char vp[PATH_MAX_];
        cache_path(victim, vp, sizeof(vp));
        ESP_LOGW(TAG, "evicting cached %s", victim);
        remove(vp);
        have--;
        victim[0] = '\0';
        cache_scan(NULL, 0, name, victim, sizeof(victim));
        if (esp_spiffs_info("capture", &total, &used) != ESP_OK) {
            break;
        }
    }
    remove(PART_PATH);
    bool room = (total > used && total - used >= need);
    hr_capture_fs_leave();
    if (!room) {
        ESP_LOGW(TAG, "not enough room on the capture partition for %s "
                      "(%u free, %u needed)", name, (unsigned)(total - used),
                 (unsigned)need);
        return HR_DF_STORAGE;
    }

    LOCK();
    r = precheck(force);
    if (r == HR_DF_OK) {
        snprintf(s_target, sizeof(s_target), "%.47s", name);
        s_writing = false;
        s_write_failed = false;
        s_mail.full = false;
        if (!hr_files_start_read(&s_fs, name, expected, now_ms())) {
            r = (s_fs.err == HR_FILES_ERR_TOO_BIG) ? HR_DF_TOOBIG
                                                    : HR_DF_BADNAME;
            s_target[0] = '\0';
        }
    }
    UNLOCK();
    if (r == HR_DF_OK) {
        ESP_LOGW(TAG, "reading %s from the dryer (%ld bytes expected)%s", name,
                 expected, force ? " [forced]" : "");
        hr_capture_event("files read %s %ld%s", name, expected,
                         force ? " forced" : "");
    }
    return r;
}

void hr_dryerfiles_cancel(void)
{
    if (s_lock == NULL) {
        return;
    }
    LOCK();
    hr_files_cancel(&s_fs, HR_FILES_ERR_CANCEL, now_ms());
    UNLOCK();
}

void hr_dryerfiles_snapshot(hr_df_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    /* the cache directory first - flash, outside the lock */
    unsigned ncached = 0;
    char cached[HR_DF_CACHE_MAX][HR_FILES_NAME_MAX];
    if (hr_capture_ready() && hr_capture_fs_enter()) {
        ncached = cache_scan(cached, HR_DF_CACHE_MAX, NULL, NULL, 0);
        hr_capture_fs_leave();
    }
    if (ncached > HR_DF_CACHE_MAX) {
        ncached = HR_DF_CACHE_MAX;
    }

    unsigned long t = now_ms();
    LOCK();
    out->enabled = s_enabled;
    out->link_up = s_link_up;
    out->dryer_running = s_running;
    out->state = s_fs.state;
    out->err = s_fs.err;
    snprintf(out->name, sizeof(out->name), "%s", s_fs.name);
    out->block = s_fs.block;
    out->received = s_fs.received;
    out->size = s_fs.size;
    out->pct = hr_files_progress_pct(&s_fs);
    out->age_ms = hr_files_busy(&s_fs) ? 0 : t - s_fs.finished_ms;
    out->list_valid = s_list_valid;
    out->list_age_ms = s_list_valid ? t - s_list_done_ms : (unsigned long)-1;
    out->nlist = s_nlist;
    memcpy(out->list, s_list, sizeof(hr_df_entry_t) * s_nlist);
    out->requests = s_fs.requests;
    out->timeouts = s_fs.timeouts;
    out->blocks_ok = s_fs.blocks_ok;
    out->blocks_bad = s_fs.blocks_bad;
    out->transfers = s_fs.transfers;
    out->blocks_in = s_session ? s_session->blocks_in : 0;
    out->big_dropped = s_session ? s_session->stream.big_dropped : 0;
    UNLOCK();

    out->ncached = ncached;
    for (unsigned i = 0; i < ncached; i++) {
        snprintf(out->cached[i], sizeof(out->cached[i]), "%.47s", cached[i]);
        for (unsigned k = 0; k < out->nlist; k++) {
            if (strcmp(out->list[k].name, cached[i]) == 0) {
                out->list[k].cached = true;
            }
        }
    }
}

bool hr_dryerfiles_cached_path(const char *name, char *path, size_t cap)
{
    if (!name_ok(name) || path == NULL) {
        return false;
    }
    cache_path(name, path, cap);
    if (!hr_capture_ready() || !hr_capture_fs_enter()) {
        return false;
    }
    bool ok = file_exists(path);
    hr_capture_fs_leave();
    return ok;
}

/* ------------------------------------------------------------------ */
/* CSV -> JSON series                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    hr_df_write_fn write;
    void *user;
    char buf[512];
    size_t len;
    bool ok;
} jout_t;

static void jflush(jout_t *o)
{
    if (o->ok && o->len > 0) {
        o->ok = o->write(o->buf, o->len, o->user);
    }
    o->len = 0;
}

static void jput(jout_t *o, const char *s)
{
    size_t n = strlen(s);
    if (o->len + n >= sizeof(o->buf)) {
        jflush(o);
    }
    if (n >= sizeof(o->buf)) {
        return; /* never: every piece here is short */
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static void jint(jout_t *o, long v, bool first)
{
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%s%ld", first ? "" : ",", v);
    jput(o, tmp);
}

static void jnull_or_int(jout_t *o, long v, long none, bool first)
{
    if (v == none) {
        jput(o, first ? "null" : ",null");
    } else {
        jint(o, v, first);
    }
}

/*
 * One pass per series would mean rereading the file up to eight times; one
 * pass building eight arrays means holding them. With no PSRAM the middle
 * road is a row-oriented "rows":[[t,shelf,room,top,bot,mtorr,htr,"proc"],..]
 * - the browser transposes it in a line. `cols` says which is which.
 */
bool hr_dryerfiles_stream_json(const char *name, hr_df_write_fn write,
                               void *user)
{
    char path[PATH_MAX_];
    if (!hr_dryerfiles_cached_path(name, path, sizeof(path)) || write == NULL) {
        return false;
    }
    if (!hr_capture_fs_enter()) {
        return false;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        hr_capture_fs_leave();
        return false;
    }

    jout_t o = {.write = write, .user = user, .len = 0, .ok = true};
    static char line[HR_MAX_FRAME];
    hr_csv_cols_t cols;
    bool have_hdr = false;
    long first_min = -1;
    long rows = 0;
    long prev_min = -1;

    jput(&o, "{\"name\":\"");
    jput(&o, name);
    jput(&o, "\",\"cols\":[\"t_min\",\"shelf_f\",\"room_f\",\"top_f\",\"bot_f\","
             "\"mtorr\",\"htr_on\",\"process\"],\"rows\":[");

    while (o.ok && fgets(line, sizeof(line), f) != NULL) {
        if (!have_hdr) {
            if (hr_csv_header_parse(line, &cols)) {
                have_hdr = true;
            }
            continue;
        }
        hr_csv_row_t r;
        if (!hr_csv_row_parse(line, &cols, &r)) {
            continue;
        }
        long min = hr_csv_row_minutes(&r);
        if (min >= 0 && first_min < 0) {
            first_min = min;
        }
        long t = (min >= 0 && first_min >= 0) ? min - first_min : prev_min + 1;
        prev_min = t;

        jput(&o, rows ? ",[" : "[");
        jint(&o, t, true);
        jnull_or_int(&o, r.mid_f, HR_CSV_NO_TEMP, false);
        jnull_or_int(&o, r.room_f, HR_CSV_NO_TEMP, false);
        jnull_or_int(&o, r.top_f, HR_CSV_NO_TEMP, false);
        jnull_or_int(&o, r.bot_f, HR_CSV_NO_TEMP, false);
        jnull_or_int(&o, r.mtorr, -1, false);
        jnull_or_int(&o, r.htron, -1, false);
        /* process text, escaped minimally: it is the dryer's own ASCII */
        jput(&o, ",\"");
        char esc[48];
        size_t e = 0;
        for (const char *p = r.process; *p && e + 2 < sizeof(esc); p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') {
                esc[e++] = '\\';
                esc[e++] = (char)c;
            } else if (c >= 0x20 && c < 0x7f) {
                esc[e++] = (char)c;
            }
        }
        esc[e] = '\0';
        jput(&o, esc);
        jput(&o, "\"]");
        rows++;
    }
    fclose(f);
    hr_capture_fs_leave();

    char tail[64];
    snprintf(tail, sizeof(tail), "],\"n\":%ld,\"header\":%s}", rows,
             have_hdr ? "true" : "false");
    jput(&o, tail);
    jflush(&o);
    return o.ok && have_hdr;
}

const char *hr_df_result_str(hr_df_result_t r)
{
    switch (r) {
    case HR_DF_OK:       return "ok";
    case HR_DF_DISABLED: return "batch history is switched off in settings";
    case HR_DF_BUSY:     return "a transfer is already running";
    case HR_DF_LINK:     return "no link to the dryer";
    case HR_DF_RUNNING:  return "the dryer is running a batch (force=1 to override)";
    case HR_DF_BADNAME:  return "not a file name the dryer would have";
    case HR_DF_STORAGE:  return "no room on the capture partition";
    case HR_DF_TOOBIG:   return "file too large to fetch";
    default:             return "?";
    }
}
