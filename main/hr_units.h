/*
 * The owner's temperature-unit preference for the adapter's own display
 * (boards with a screen): Fahrenheit, Celsius, or AUTO - follow the dryer's
 * panel. The web page keeps a per-browser unit of its own (upstream 1.2.1)
 * and mirrors an explicit choice into this setting over /api/units so the
 * screen follows; /api/state and MQTT stay in F.
 *
 * The dryer sends degrees F in every frame and never says which unit its
 * panel shows - but it KEEPS that choice in a data-flash record, HRTempFC.txt
 * ("0,Celsius, "), which the file client reads with one FILEREAD (hr_files.h,
 * hr_temp.h: hr_tempfc_parse). hr_unitsync does that read after every link-up,
 * when a batch starts and on request, and hands the answer here
 * (hr_units_set_dryer_unit); it is kept in RAM and NVS so a reboot starts
 * from the last known panel unit. AUTO then resolves to it
 * through hr_temp_resolve(); an explicit F/C always wins. Until the first
 * sync ever, AUTO falls back to Fahrenheit, the dryer's factory unit.
 *
 * Stored values stay in F; only presentation converts (hr_temp.h). Never
 * sends anything to the dryer: the setting is the adapter's alone (the
 * sync is a read, and lives in hr_unitsync).
 */
#ifndef HR_UNITS_H
#define HR_UNITS_H

#include "hr_temp.h"

#include <stdbool.h>
#include <stdint.h>

/* Read the stored preference and the last known dryer unit. Call once at boot. */
void hr_units_init(void);

/* What the owner asked for (f / c / auto). */
hr_temp_pref_t hr_units_pref(void);

/* The unit to present temperatures in, right now (AUTO resolved). */
hr_temp_unit_t hr_units_temp(void);

/* Shorthand for the display: true when presenting in Celsius. */
bool hr_units_metric(void);

/*
 * Store and apply. Returns false if NVS refused the write; the in-memory
 * value is still updated so the UI follows the request immediately.
 */
bool hr_units_set_pref(hr_temp_pref_t pref);

/* True once after every change of the RESOLVED unit (the main loop may act
 * on it; unused today). */
bool hr_units_take_changed(void);

/* ---- the dryer's panel unit, as last read from HRTempFC.txt ---- */

/*
 * Record what the dryer said (HR_TEMP_F / HR_TEMP_C as ints, or
 * HR_TEMP_DRYER_UNKNOWN to forget). `epoch_s` is the wall clock at the
 * sync, 0 when unknown. Persists to NVS when the value changed; call from
 * a task that may block (not the USB RX task).
 */
void hr_units_set_dryer_unit(int unit, uint32_t epoch_s);

/* HR_TEMP_F, HR_TEMP_C or HR_TEMP_DRYER_UNKNOWN. */
int hr_units_dryer_unit(void);

/* Seconds since the last successful sync THIS BOOT; -1 if none yet (a value
 * restored from NVS counts as "not synced this boot"). */
long hr_units_dryer_unit_age_s(void);

/* Wall clock of the last sync (this boot or restored), 0 if never/unknown. */
uint32_t hr_units_dryer_unit_epoch(void);

/* "nvs" (restored, not confirmed this boot), "live" (read this boot), "none". */
const char *hr_units_dryer_unit_source(void);

#endif /* HR_UNITS_H */
