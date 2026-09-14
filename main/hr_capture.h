/*
 * Persistent capture log.
 *
 * The in-RAM history ring only holds ~16 minutes of frames, which is far too
 * short to record a full freeze-drying cycle (24h+). This module appends every
 * inbound frame to a file in a dedicated SPIFFS partition so a whole cycle can
 * be downloaded afterwards - and survives reboots and power cuts.
 *
 * Format v2: one line per event, four tab-separated columns:
 *
 *     <millis>\t<epoch|->\t<dir>\t<payload>
 *
 *   millis   milliseconds since boot (esp_timer)
 *   epoch    Unix seconds when the browser has set the clock, else "-"
 *   dir      >  frame from the dryer            <  frame we sent
 *            !  adapter event (usb/link/boot)   ?  bytes the parser rejected
 *            ~  run summary ("repeat <body> xN first..last")
 *   payload  the frame without its CR; events as short text; rejected bytes
 *            printable as-is and otherwise \xNN
 *
 * The file begins (after every mount) with "# hr-capture v2 ..." naming the
 * firmware and the columns. Format v1 was "<millis>\t<body>" and recorded
 * only the inbound side; both directions plus USB events are what make a
 * capture usable for decoding the protocol.
 */
#ifndef HR_CAPTURE_H
#define HR_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hr_trend.h"

/* Mount the capture partition. Safe to call once at startup. */
/*
 * Start the capture log. Returns immediately: the SPIFFS mount happens on a
 * background task after a short delay, because doing it synchronously on the
 * boot path breaks USB (see hr_capture.c for the full explanation).
 */
/*
 * Why the chip last restarted, as a short word: "poweron", "panic",
 * "brownout", "sw", ... Lives here because the capture log records it, and
 * both this and /api/state need the same answer from one place.
 */
const char *hr_reset_reason_str(void);

void hr_capture_init(void);

/* How long to wait before mounting, so USB enumeration finishes untouched. */
#define HR_CAPTURE_MOUNT_DELAY_MS 8000

/* Perform the mount synchronously. Normally only called by the mount task. */
void hr_capture_mount_now(void);

/* True when the log is mounted and writable. */
bool hr_capture_ready(void);

/*
 * Append one frame line. No-op if not ready.
 *
 * NON-BLOCKING: the line is copied onto a queue and written by a worker task.
 * This is called from the USB RX callback, and doing file I/O there is what
 * overflowed the TinyUSB task stack and panicked the chip on every frame in
 * v0.3-v0.3.3. Nothing on this path may touch flash directly, ever.
 *
 * If the queue is full the line is DROPPED rather than blocking the USB task;
 * hr_capture_dropped() reports how often, so the loss is visible instead of
 * silent.
 */
void hr_capture_append(uint32_t t_ms, const char *body);

/* Direction markers for hr_capture_append_dir(). */
#define HR_CAP_DIR_RX    '>'
#define HR_CAP_DIR_TX    '<'
#define HR_CAP_DIR_EVENT '!'
#define HR_CAP_DIR_BAD   '?'

/* As hr_capture_append(), with an explicit direction column. Same rules:
 * non-blocking, safe from the USB task, dropped and counted when full. */
void hr_capture_append_dir(uint32_t t_ms, char dir, const char *body);

/* Record an adapter event ("usb mount #2", "link down", ...) with the
 * current uptime. printf-style; the result is capped to one line. */
void hr_capture_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Record bytes the frame parser rejected: printable ASCII as-is, anything
 * else as \xNN, capped to `n` <= 64 bytes of input. */
void hr_capture_rejected(uint32_t t_ms, const char *bytes, size_t n,
                         const char *why);

/* Frame lines discarded because the write queue was full. */
unsigned long hr_capture_dropped(void);

/* Bytes currently stored, and the partition's total capacity. */
size_t hr_capture_size(void);
size_t hr_capture_capacity(void);

/* Per-segment view, for diagnosing a download that does not match the size. */
unsigned hr_capture_seg_count(void);
unsigned hr_capture_seg_active(void);
size_t   hr_capture_seg_bytes(unsigned i);

/* Erase the log. Returns false on failure. */
bool hr_capture_clear(void);

/*
 * Open the log for reading. Returns NULL if unavailable. Caller must call
 * hr_capture_close(). Kept opaque so the HTTP layer can stream it.
 */
/* Flush and unmount before a deliberate reboot, so SPIFFS is not left
 * inconsistent. After this the capture APIs are inert. */
void hr_capture_shutdown(void);

/* Reformat the partition. Destroys the capture log, the trend and the
 * logbook - the recovery path when SPIFFS refuses writes with EIO while
 * still reporting free space. */
bool hr_capture_format(void);

void *hr_capture_open(void);
int hr_capture_read(void *handle, char *buf, size_t cap);
void hr_capture_close(void *handle);

/* ------------------------------------------------------------------ */
/* Power-loss recovery for the graph series                            */
/* ------------------------------------------------------------------ */
/*
 * The adapter is powered from the dryer, so a brownout - or any reflash -
 * loses the in-RAM graph. The dryer keeps counting regardless: its batch
 * elapsed value advances whether or not we are alive.
 *
 * So each persisted point carries the batch-elapsed value it was recorded at.
 * On the next boot, comparing the dryer's current elapsed against the last
 * stored one gives the outage duration EXACTLY, with no clock of our own and
 * no guessing. Divided by the bucket width it becomes the number of missing
 * buckets, which are restored as gaps.
 *
 * A LOWER elapsed value means the dryer started a new batch while we were
 * down, so the stored series belongs to a finished run and is discarded.
 */

/*
 * Write the whole series to flash, replacing whatever was there.
 *
 * Call from a normal task - it does file I/O synchronously. NOT from the USB
 * RX callback. Returns false if the write failed.
 */
bool hr_capture_trend_save(const hr_trend_t *tr, uint32_t batch_elapsed_s);

/*
 * Load a persisted series into `tr`. Sets *last_elapsed to the batch-elapsed
 * value of the final stored point. Returns the number of points restored, or 0
 * if there is nothing to resume.
 */
size_t hr_capture_trend_load(hr_trend_t *tr, uint32_t *last_elapsed);

/* Bytes currently in the persisted series file - diagnostics. */
size_t hr_capture_trend_bytes(void);
unsigned long hr_capture_trend_writes(void);
unsigned long hr_capture_trend_fails(void);

/*
 * How many times the log has rotated, and how many repeated frames were
 * collapsed into run summaries. Both are reported so the compression is
 * visible rather than something the user has to infer from a short log.
 */
unsigned long hr_capture_rotations(void);
unsigned long hr_capture_suppressed(void);

/* Discard the stored series (new batch, or a resume that does not apply). */
void hr_capture_trend_reset(void);

#endif /* HR_CAPTURE_H */
