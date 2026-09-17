#include "hr_units.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include <stdatomic.h>

static const char *TAG = "hr_units";

#define UNITS_NVS_NS   "hrunits"
#define UNITS_NVS_KEY  "temp"     /* u8: 0 = auto, 1 = F, 2 = C (hr_temp_pref_t) */
#define DRYER_NVS_KEY  "dryer"    /* u8: 0 = F, 1 = C (hr_temp_unit_t); absent = unknown */
#define DRYER_NVS_TIME "dryer_t"  /* u32: epoch seconds of that sync, 0 unknown */

static atomic_int  s_pref = HR_TEMP_PREF_F;
static atomic_bool s_changed;

/* The dryer's panel unit: HR_TEMP_F / HR_TEMP_C / HR_TEMP_DRYER_UNKNOWN. */
static atomic_int  s_dryer = HR_TEMP_DRYER_UNKNOWN;
static atomic_bool s_dryer_live;             /* read this boot, not restored */
static _Atomic uint32_t s_dryer_epoch;       /* wall clock of the sync, or 0 */
static _Atomic long s_dryer_synced_ms;       /* uptime at the sync; -1 none */

static hr_temp_pref_t pref_from_u8(uint8_t v)
{
    switch (v) {
    case HR_TEMP_PREF_C:
        return HR_TEMP_PREF_C;
    case HR_TEMP_PREF_AUTO:
        return HR_TEMP_PREF_AUTO;
    case HR_TEMP_PREF_F:
    default:
        return HR_TEMP_PREF_F;
    }
}

static long uptime_ms(void)
{
    return (long)(esp_timer_get_time() / 1000);
}

static const char *dryer_str(int u)
{
    return u == HR_TEMP_C ? "C" : u == HR_TEMP_F ? "F" : "unknown";
}

void hr_units_init(void)
{
    hr_temp_pref_t pref = HR_TEMP_PREF_F;
    const char *src = "default";
    int dryer = HR_TEMP_DRYER_UNKNOWN;
    uint32_t epoch = 0;
    atomic_store(&s_dryer_synced_ms, -1);
    nvs_handle_t nh;
    if (nvs_open(UNITS_NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(nh, UNITS_NVS_KEY, &v) == ESP_OK) {
            pref = pref_from_u8(v);
            src = "nvs";
        }
        if (nvs_get_u8(nh, DRYER_NVS_KEY, &v) == ESP_OK &&
            (v == HR_TEMP_F || v == HR_TEMP_C)) {
            dryer = (int)v;
            nvs_get_u32(nh, DRYER_NVS_TIME, &epoch);
        }
        nvs_close(nh);
    }
    atomic_store(&s_pref, (int)pref);
    atomic_store(&s_dryer, dryer);
    atomic_store(&s_dryer_epoch, epoch);
    atomic_store(&s_dryer_live, false);
    atomic_store(&s_changed, true);
    ESP_LOGI(TAG, "temperature unit: %s -> degrees %s (%s); dryer panel %s%s",
             hr_temp_pref_str(pref), hr_temp_unit_letter(hr_units_temp()),
             src, dryer_str(dryer),
             dryer == HR_TEMP_DRYER_UNKNOWN ? "" : " (nvs, unconfirmed)");
}

hr_temp_pref_t hr_units_pref(void)
{
    return (hr_temp_pref_t)atomic_load(&s_pref);
}

hr_temp_unit_t hr_units_temp(void)
{
    /* AUTO follows the dryer's panel once HRTempFC.txt has been read (this
     * boot or an earlier one); before any sync ever, F - the factory unit. */
    return hr_temp_resolve(hr_units_pref(), atomic_load(&s_dryer), HR_TEMP_F);
}

bool hr_units_metric(void)
{
    return hr_units_temp() == HR_TEMP_C;
}

bool hr_units_set_pref(hr_temp_pref_t pref)
{
    hr_temp_unit_t before = hr_units_temp();
    hr_temp_pref_t old = (hr_temp_pref_t)atomic_exchange(&s_pref, (int)pref);
    if (old != pref || hr_units_temp() != before) {
        atomic_store(&s_changed, true);
    }
    nvs_handle_t nh;
    if (nvs_open(UNITS_NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed; setting not persisted");
        return false;
    }
    bool ok = nvs_set_u8(nh, UNITS_NVS_KEY, (uint8_t)pref) == ESP_OK &&
              nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    ESP_LOGI(TAG, "temperature unit set: %s -> degrees %s%s",
             hr_temp_pref_str(pref), hr_temp_unit_letter(hr_units_temp()),
             ok ? "" : " (NOT persisted)");
    return ok;
}

bool hr_units_take_changed(void)
{
    return atomic_exchange(&s_changed, false);
}

void hr_units_set_dryer_unit(int unit, uint32_t epoch_s)
{
    if (unit != HR_TEMP_F && unit != HR_TEMP_C) {
        unit = HR_TEMP_DRYER_UNKNOWN;
    }
    hr_temp_unit_t before = hr_units_temp();
    int old = atomic_exchange(&s_dryer, unit);
    if (unit != HR_TEMP_DRYER_UNKNOWN) {
        atomic_store(&s_dryer_live, true);
        atomic_store(&s_dryer_synced_ms, uptime_ms());
        if (epoch_s != 0) {
            atomic_store(&s_dryer_epoch, epoch_s);
        }
    } else {
        atomic_store(&s_dryer_live, false);
        atomic_store(&s_dryer_synced_ms, -1);
        atomic_store(&s_dryer_epoch, 0);
    }
    if (hr_units_temp() != before) {
        atomic_store(&s_changed, true);
    }
    ESP_LOGI(TAG, "dryer panel unit: %s%s -> screen shows degrees %s (%s)",
             dryer_str(unit), old == unit ? "" : " (changed)",
             hr_temp_unit_letter(hr_units_temp()),
             hr_temp_pref_str(hr_units_pref()));

    /* Persist only a change - one NVS write per real flip, not per sync. */
    nvs_handle_t nh;
    uint8_t stored = 0xff;
    if (nvs_open(UNITS_NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        return;
    }
    nvs_get_u8(nh, DRYER_NVS_KEY, &stored);
    bool need = (unit == HR_TEMP_DRYER_UNKNOWN) ? (stored != 0xff)
                                                 : (stored != (uint8_t)unit);
    if (need) {
        bool ok;
        if (unit == HR_TEMP_DRYER_UNKNOWN) {
            ok = nvs_erase_key(nh, DRYER_NVS_KEY) == ESP_OK;
            nvs_erase_key(nh, DRYER_NVS_TIME);
        } else {
            ok = nvs_set_u8(nh, DRYER_NVS_KEY, (uint8_t)unit) == ESP_OK;
        }
        ok = ok && nvs_commit(nh) == ESP_OK;
        if (!ok) {
            ESP_LOGW(TAG, "dryer panel unit NOT persisted");
        }
    }
    if (unit != HR_TEMP_DRYER_UNKNOWN && epoch_s != 0) {
        uint32_t was = 0;
        nvs_get_u32(nh, DRYER_NVS_TIME, &was);
        /* the time stamp is cheap to keep fresh but not worth a write per
         * sync: once a day is plenty for "last seen" */
        if (was == 0 || epoch_s - was > 86400u || need) {
            nvs_set_u32(nh, DRYER_NVS_TIME, epoch_s);
            nvs_commit(nh);
        }
    }
    nvs_close(nh);
}

int hr_units_dryer_unit(void)
{
    return atomic_load(&s_dryer);
}

long hr_units_dryer_unit_age_s(void)
{
    long at = atomic_load(&s_dryer_synced_ms);
    if (at < 0) {
        return -1;
    }
    return (uptime_ms() - at) / 1000;
}

uint32_t hr_units_dryer_unit_epoch(void)
{
    return atomic_load(&s_dryer_epoch);
}

const char *hr_units_dryer_unit_source(void)
{
    if (atomic_load(&s_dryer) == HR_TEMP_DRYER_UNKNOWN) {
        return "none";
    }
    return atomic_load(&s_dryer_live) ? "live" : "nvs";
}
