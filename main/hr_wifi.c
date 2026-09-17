#include "hr_wifi.h"

#include "hr_capture.h"
#include "hr_netwatch.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "hr_wifi";
#define NVS_NS "hrwifi"
#define MAX_SCAN 16
#define STA_RETRY_LIMIT 8 /* consecutive failures before falling back to AP */
/*
 * Security: the setup AP is only open for this long. If no successful home-
 * network connection happens in the window, the AP is shut down and a reboot
 * is required to re-open it. This stops an internet/router outage from leaving
 * the device broadcasting an open setup network indefinitely for attackers.
 */
#define AP_OPEN_WINDOW_US (5 * 60 * 1000000ULL)
/*
 * Once the fast retries are used up, keep trying the stored network at this
 * interval for as long as we are powered. The adapter is fed by the dryer's
 * USB port, so "reboot to reconnect" means a trip to the machine mid-batch.
 */
#define STA_RETRY_BACKOFF_US (30 * 1000000ULL)
/*
 * No-IP watchdog (hr_netwatch.h). How often the station's address is checked
 * while associated, and how long "associated but 0.0.0.0" is tolerated
 * before the status flips to HR_WIFI_NO_IP. The remedy timeouts (restart
 * DHCP, rejoin) come from Kconfig.
 */
#define NOIP_POLL_US (2 * 1000000ULL)
#define NOIP_GRACE_MS 5000u
/*
 * The first address after a join gets longer: DHCP from scratch on a weak
 * link has been seen to take 8 s (RSSI -97 dBm), and a renewal that is
 * merely slow is a different thing from a lease that was lost. Kept under
 * the 15 s DHCP restart so the status still flips before the remedies.
 */
#define NOIP_FIRST_GRACE_MS 12000u
/*
 * Gateway probe (hr_netwatch.h): an ARP request every CONFIG_HR_WIFI_GW_PROBE_S
 * while an address is held, checked for an answer on the next 2 s poll.
 */
#define GW_PROBE_MS ((uint32_t)CONFIG_HR_WIFI_GW_PROBE_S * 1000u)
/*
 * Link history. A join that fails is retried every 30 s for as long as the
 * network stays out of reach - 18 hours of that is 2000 identical lines, so
 * failed attempts go into the capture only this often after the first few.
 * The signal is sampled into the capture at this interval while connected,
 * so a weak spot can be read off afterwards instead of guessed at.
 */
#define JOIN_FAIL_RECORD_EVERY 10u
#define RSSI_SAMPLE_MS (5u * 60u * 1000u)

static hr_wifi_status_t s_status;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static int s_sta_retries;
static char s_ssid[33];
static esp_timer_handle_t s_ap_timeout_timer;
static esp_timer_handle_t s_sta_retry_timer;
static esp_timer_handle_t s_noip_poll_timer;
static bool s_ap_window_expired; /* true once the 5-min window has closed */
static int64_t s_ap_opened_us;   /* when the current window was armed */
static volatile bool s_restarting; /* hr_wifi_prepare_restart() was called */
static hr_netwatch_t s_netwatch;
/* The next STA_DISCONNECTED is one we asked for to get an address back. */
static volatile bool s_noip_rejoin_pending;

static wifi_ap_record_t s_scan[MAX_SCAN];
static uint16_t s_scan_count;
static volatile bool s_scanning;

/*
 * Link history since boot (hr_wifi_link_stats). The 32 KB log ring holds a
 * few minutes of a busy link; an outage found the next morning has scrolled
 * out of it. These counters, and the "wifi ..." event records hr_wifi writes
 * into the capture (which lives in flash and spans days), are what is left
 * to read then.
 */
static unsigned s_link_joins;     /* WIFI_EVENT_STA_CONNECTED */
static unsigned s_link_drops;     /* STA_DISCONNECTED while associated */
static unsigned s_join_fails;     /* STA_DISCONNECTED while not associated */
static unsigned s_bcn_timeouts;   /* WIFI_EVENT_STA_BEACON_TIMEOUT */
static int s_last_reason;         /* wifi_err_reason_t of the last drop/fail */
static uint32_t s_assoc_since_ms; /* start of the current association */
static uint32_t s_up_since_ms;    /* start of the current CONNECTED spell */
static uint32_t s_down_since_ms;  /* when CONNECTED was last lost */
static bool s_ever_up;            /* had a working link at least once */
static uint32_t s_rssi_sample_ms; /* last RSSI record written to the capture */
static uint32_t s_probe_sent_ms;  /* when the last gateway probe went out */
static bool s_probe_pending;      /* sent, answer not yet looked for */

/* -------------------------------------------------------------------- */
/* NVS credential storage                                                */
/* -------------------------------------------------------------------- */
static bool load_credentials(char *ssid, size_t ssid_cap, char *pw,
                             size_t pw_cap)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READONLY, &nh) != ESP_OK) {
        return false;
    }
    size_t sl = ssid_cap, pl = pw_cap;
    bool ok = nvs_get_str(nh, "ssid", ssid, &sl) == ESP_OK &&
              nvs_get_str(nh, "pw", pw, &pl) == ESP_OK && ssid[0] != '\0';
    nvs_close(nh);
    return ok;
}

static bool store_credentials(const char *ssid, const char *pw)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_str(nh, "ssid", ssid) == ESP_OK &&
              nvs_set_str(nh, "pw", pw) == ESP_OK && nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    return ok;
}

/* -------------------------------------------------------------------- */
/* Mode switching                                                        */
/* -------------------------------------------------------------------- */

/*
 * AP-timeout: fires once, AP_OPEN_WINDOW_US after the setup AP opens. If we
 * have not connected to the home network by then, shut the AP down (STA-only)
 * so the device stops broadcasting. Re-opening requires a reboot.
 * Runs in the esp_timer task - safe to call esp_wifi_* here.
 */
static void ap_timeout_cb(void *arg)
{
    (void)arg;
    if (s_status == HR_WIFI_CONNECTED) {
        return; /* already connected; AP was closed on GOT_IP */
    }
    ESP_LOGW(TAG, "setup-AP window (5 min) elapsed with no connection; "
                  "disabling AP. Reboot to re-open setup.");
    s_ap_window_expired = true;
    /* STA-only kills the AP but keeps trying stored creds (if any). */
    esp_wifi_set_mode(WIFI_MODE_STA);
}

static void arm_ap_timeout(void)
{
    if (s_ap_window_expired) {
        return; /* window already used this boot; do not re-open */
    }
    if (s_ap_timeout_timer == NULL) {
        const esp_timer_create_args_t a = {.callback = ap_timeout_cb,
                                           .name = "ap_timeout"};
        if (esp_timer_create(&a, &s_ap_timeout_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(s_ap_timeout_timer); /* restart the window cleanly */
    esp_timer_start_once(s_ap_timeout_timer, AP_OPEN_WINDOW_US);
    s_ap_opened_us = esp_timer_get_time();
}

static void cancel_ap_timeout(void)
{
    if (s_ap_timeout_timer != NULL) {
        esp_timer_stop(s_ap_timeout_timer);
    }
}

/* Slow retry of the stored network. Runs in the esp_timer task. */
static void sta_retry_cb(void *arg)
{
    (void)arg;
    if (s_status == HR_WIFI_CONNECTED || s_netwatch.associated ||
        s_ssid[0] == '\0') {
        return; /* associated already; DHCP is the watchdog's business */
    }
    ESP_LOGI(TAG, "retrying \"%s\" (attempt %d)", s_ssid, s_sta_retries + 1);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void arm_sta_retry(void)
{
    if (s_sta_retry_timer == NULL) {
        const esp_timer_create_args_t a = {.callback = sta_retry_cb,
                                           .name = "sta_retry"};
        if (esp_timer_create(&a, &s_sta_retry_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(s_sta_retry_timer);
    esp_timer_start_once(s_sta_retry_timer, STA_RETRY_BACKOFF_US);
}

static void cancel_sta_retry(void)
{
    if (s_sta_retry_timer != NULL) {
        esp_timer_stop(s_sta_retry_timer);
    }
}

/* -------------------------------------------------------------------- */
/* No-IP watchdog                                                        */
/* -------------------------------------------------------------------- */
static void gw_probe_step(uint32_t now);
static void request_rejoin(void);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool sta_has_ip(void)
{
    esp_netif_ip_info_t ip = {0};
    return s_sta_netif != NULL &&
           esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK &&
           ip.ip.addr != 0;
}

/* The driver's RSSI for the current association, 0 when there is none. */
static int ap_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

static const char *status_name(hr_wifi_status_t s)
{
    switch (s) {
    case HR_WIFI_AP_SETUP: return "setup-ap";
    case HR_WIFI_CONNECTING: return "connecting";
    case HR_WIFI_CONNECTED: return "connected";
    case HR_WIFI_NO_IP: return "no-ip";
    case HR_WIFI_UNREACHABLE: return "unreachable";
    default: return "booting";
    }
}

/*
 * Gateway probe, both halves running on the lwIP thread via
 * esp_netif_tcpip_exec() (the ARP table is that thread's).
 *
 * ARP rather than ICMP: a router may be told not to answer pings, but it
 * cannot refuse to answer for its own address and still route anything.
 * The catch is that lwIP's table keeps a learned entry for ARP_MAXAGE
 * (minutes) whether or not the peer is still answering, so "is the gateway
 * in the table" says nothing by itself. The probe therefore drops the
 * station netif's entries first and asks; two seconds later the gateway is
 * either back in the table (it answered, or asked for us - alive either
 * way) or it is not. The cost is one extra ARP round-trip for each peer
 * the station talks to, once per probe interval, and lwIP queues the packet
 * meanwhile rather than dropping it.
 */
typedef struct {
    struct netif *nif;
    ip4_addr_t gw;
    bool found;
} gw_probe_t;

static esp_err_t gw_probe_send_cb(void *arg)
{
    gw_probe_t *p = arg;
    etharp_cleanup_netif(p->nif);
    return etharp_request(p->nif, &p->gw) == ERR_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t gw_probe_check_cb(void *arg)
{
    gw_probe_t *p = arg;
    struct eth_addr *eth;
    const ip4_addr_t *ip;
    p->found = etharp_find_addr(p->nif, &p->gw, &eth, &ip) >= 0;
    return ESP_OK;
}

/* The station netif and its gateway; false when there is nothing to probe. */
static bool gw_probe_target(gw_probe_t *p)
{
    esp_netif_ip_info_t ip = {0};
    if (s_sta_netif == NULL ||
        esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK ||
        ip.ip.addr == 0 || ip.gw.addr == 0) {
        return false;
    }
    p->nif = esp_netif_get_netif_impl(s_sta_netif);
    p->gw.addr = ip.gw.addr;
    p->found = false;
    return p->nif != NULL;
}

/* Send a probe; true if one went out. */
static bool gw_probe_send(void)
{
    gw_probe_t p;
    if (!gw_probe_target(&p)) {
        return false;
    }
    return esp_netif_tcpip_exec(gw_probe_send_cb, &p) == ESP_OK;
}

/* Was the probe answered? */
static bool gw_probe_answered(void)
{
    gw_probe_t p;
    if (!gw_probe_target(&p)) {
        return false;
    }
    if (esp_netif_tcpip_exec(gw_probe_check_cb, &p) != ESP_OK) {
        return false;
    }
    return p.found;
}

/*
 * Every status change goes through here so the up/down clocks in
 * hr_wifi_link_stats() cannot drift from what the rest of the firmware is
 * told. CONNECTED is the only state in which anything is reachable.
 */
static void set_status(hr_wifi_status_t s)
{
    if (s == s_status) {
        return;
    }
    uint32_t now = now_ms();
    if (s == HR_WIFI_CONNECTED) {
        s_up_since_ms = now;
        s_ever_up = true;
    } else if (s_status == HR_WIFI_CONNECTED) {
        s_down_since_ms = now;
    }
    s_status = s;
}

/*
 * One pass of the watchdog: read the station's address, let hr_netwatch
 * decide, carry out what it asks. Runs from the poll timer (esp_timer task)
 * and from the IP events (event task). Neither blocks: esp_netif_dhcpc_*
 * and esp_wifi_disconnect() only post to their own tasks.
 *
 * The status is driven from here too. WIFI_EVENT_STA_DISCONNECTED never
 * fires for a lost lease - the station stays associated, beacons and RSSI
 * are fine - so "connected" has to mean "associated AND has an address",
 * and this is the only place that knows both.
 */
static void noip_check(void)
{
    if (s_restarting || !s_netwatch.associated) {
        return;
    }
    uint32_t now = now_ms();
    bool have_ip = sta_has_ip();
    hr_netwatch_action_t act = hr_netwatch_tick(&s_netwatch, have_ip, now);
    unsigned long noip_s =
        (unsigned long)(hr_netwatch_noip_for_ms(&s_netwatch, now) / 1000u);

    if (have_ip) {
        if (s_status == HR_WIFI_NO_IP) {
            /* The GOT_IP event normally does this; cover the race where the
             * poll flipped the status just as the address arrived. */
            set_status(HR_WIFI_CONNECTED);
        }
        if (s_status == HR_WIFI_CONNECTED &&
            now - s_rssi_sample_ms >= RSSI_SAMPLE_MS) {
            s_rssi_sample_ms = now;
            hr_capture_event("wifi rssi=%d up=%lus", ap_rssi(),
                             (unsigned long)((now - s_up_since_ms) / 1000u));
        }
        gw_probe_step(now);
        return;
    }
    s_probe_pending = false; /* whatever was in flight has no path now */
    if (hr_netwatch_no_ip(&s_netwatch, now) &&
        (s_status == HR_WIFI_CONNECTED || s_status == HR_WIFI_CONNECTING ||
         s_status == HR_WIFI_UNREACHABLE)) {
        ESP_LOGW(TAG, "associated with \"%s\" but no IP address for %lus "
                      "(DHCP not answering); reporting as not connected",
                 s_ssid, noip_s);
        hr_capture_event("wifi no-ip %lus rssi=%d", noip_s, ap_rssi());
        set_status(HR_WIFI_NO_IP);
    }

    esp_err_t err;
    switch (act) {
    case HR_NETWATCH_DHCP_RESTART:
        ESP_LOGW(TAG, "no IP for %lus: restarting the DHCP client", noip_s);
        hr_capture_event("wifi dhcp-restart no-ip=%lus", noip_s);
        /* "Already stopped" is fine here; the start is what matters. */
        esp_netif_dhcpc_stop(s_sta_netif);
        err = esp_netif_dhcpc_start(s_sta_netif);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_netif_dhcpc_start: %s", esp_err_to_name(err));
        }
        break;
    case HR_NETWATCH_RECONNECT:
        ESP_LOGW(TAG, "no IP for %lus: leaving and rejoining \"%s\" so the "
                      "association and DHCP start fresh (rejoin %u, next "
                      "wait %lus)",
                 noip_s, s_ssid, (unsigned)s_netwatch.reconnects,
                 (unsigned long)(hr_netwatch_reconnect_delay_ms(&s_netwatch) /
                                 1000u));
        hr_capture_event("wifi rejoin %u no-ip=%lus rssi=%d",
                         (unsigned)s_netwatch.reconnects, noip_s, ap_rssi());
        request_rejoin();
        break;
    default:
        break;
    }
}

static void noip_poll_cb(void *arg)
{
    (void)arg;
    noip_check();
}

/* Leave and rejoin so the router sees the station afresh. Shared by the
 * no-IP and the dead-link remedies; the disconnect handler recognises it. */
static void request_rejoin(void)
{
    s_noip_rejoin_pending = true;
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        s_noip_rejoin_pending = false;
        ESP_LOGW(TAG, "esp_wifi_disconnect: %s", esp_err_to_name(err));
    }
}

/*
 * One step of the gateway probe, from the 2 s poll while an address is
 * held: look for the answer to the probe sent last time, then send the next
 * one when its interval is up. Off when CONFIG_HR_WIFI_GW_PROBE_S is 0.
 */
static void gw_probe_step(uint32_t now)
{
    if (GW_PROBE_MS == 0) {
        return;
    }
    if (s_probe_pending) {
        s_probe_pending = false;
        bool ok = gw_probe_answered();
        hr_netwatch_action_t act = hr_netwatch_on_probe(&s_netwatch, ok, now);
        unsigned long dead_s =
            (unsigned long)(hr_netwatch_dead_for_ms(&s_netwatch, now) / 1000u);
        if (ok && s_status == HR_WIFI_UNREACHABLE) {
            ESP_LOGW(TAG, "gateway answering again");
            hr_capture_event("wifi gateway back rssi=%d", ap_rssi());
            set_status(HR_WIFI_CONNECTED);
        } else if (!ok && hr_netwatch_dead(&s_netwatch) &&
                   s_status == HR_WIFI_CONNECTED) {
            ESP_LOGW(TAG, "associated with \"%s\", address held, but the "
                          "gateway has not answered %lu probes in a row; "
                          "reporting as not connected",
                     s_ssid, (unsigned long)s_netwatch.cfg.probe_miss_limit);
            hr_capture_event("wifi unreachable rssi=%d up=%lus", ap_rssi(),
                             (unsigned long)((now - s_up_since_ms) / 1000u));
            set_status(HR_WIFI_UNREACHABLE);
        }
        if (act == HR_NETWATCH_RECONNECT) {
            ESP_LOGW(TAG, "gateway silent: leaving and rejoining \"%s\" "
                          "(rejoin %u, next after %lu misses)",
                     s_ssid, (unsigned)s_netwatch.dead_reconnects,
                     (unsigned long)hr_netwatch_probe_miss_limit(&s_netwatch));
            hr_capture_event("wifi rejoin-dead %u dead=%lus rssi=%d",
                             (unsigned)s_netwatch.dead_reconnects, dead_s,
                             ap_rssi());
            request_rejoin();
            return;
        }
    }
    if (now - s_probe_sent_ms >= GW_PROBE_MS && gw_probe_send()) {
        s_probe_sent_ms = now;
        s_probe_pending = true;
    }
}

static void start_noip_poll(void)
{
    hr_netwatch_cfg_t cfg = {
        .grace_ms = NOIP_GRACE_MS,
        .first_grace_ms = NOIP_FIRST_GRACE_MS,
        .dhcp_restart_ms = (uint32_t)CONFIG_HR_WIFI_NOIP_DHCP_RESTART_S * 1000u,
        .reconnect_ms = (uint32_t)CONFIG_HR_WIFI_NOIP_RECONNECT_S * 1000u,
        .reconnect_max_ms = (uint32_t)CONFIG_HR_WIFI_NOIP_RECONNECT_MAX_S * 1000u,
        .probe_miss_limit = (uint32_t)CONFIG_HR_WIFI_GW_PROBE_MISSES,
    };
    hr_netwatch_init(&s_netwatch, &cfg);
    const esp_timer_create_args_t a = {.callback = noip_poll_cb,
                                       .name = "noip_poll"};
    if (esp_timer_create(&a, &s_noip_poll_timer) != ESP_OK ||
        esp_timer_start_periodic(s_noip_poll_timer, NOIP_POLL_US) != ESP_OK) {
        ESP_LOGE(TAG, "no-IP watchdog not started");
    }
}

static void stop_noip_poll(void)
{
    if (s_noip_poll_timer != NULL) {
        esp_timer_stop(s_noip_poll_timer);
    }
}

/*
 * Configure the setup AP. Does NOT start the WiFi driver - the driver is
 * started exactly once in hr_wifi_start(). Callable any time to (re)assert
 * the AP config (e.g. hr_wifi_forget()).
 */
static void configure_ap(void)
{
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", CONFIG_HR_AP_SSID);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;

    const char *pw = CONFIG_HR_AP_PASSWORD;
    if (strlen(pw) >= 8) {
        snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", pw);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
}

static void start_ap_mode(void)
{
    if (s_ap_window_expired) {
        /* The one-time setup window is over; refuse to re-broadcast the AP.
         * Stay STA-only and keep retrying stored credentials. */
        ESP_LOGW(TAG, "setup AP not re-opened (window used); STA-only. "
                      "Reboot to run setup again.");
        set_status(HR_WIFI_CONNECTING);
        esp_wifi_set_mode(WIFI_MODE_STA);
        return;
    }
    ESP_LOGI(TAG, "setup AP \"%s\" active (open for 5 min)", CONFIG_HR_AP_SSID);
    set_status(HR_WIFI_AP_SETUP);
    configure_ap();
    arm_ap_timeout();
    /* WiFi is already started by hr_wifi_start(); nothing else to do. */
}

/*
 * Configure the station and connect. WiFi is already started in APSTA mode by
 * hr_wifi_start(), so we MUST NOT call esp_wifi_start() again here - we only
 * set the STA config and initiate the connect.
 *
 * NOTE (single-radio APSTA): once the STA associates, the softAP is forced to
 * the STA's channel (esp_wifi.h). A browser on the setup AP will briefly drop
 * when that happens, then can reconnect to the AP (now on the new channel).
 * The AP is deliberately kept up so the status page remains reachable and can
 * report the new station IP.
 */
/*
 * Returns false if the driver refused the configuration. This used to be
 * ESP_ERROR_CHECK, i.e. abort() on data that came in over HTTP: a password the
 * driver rejects (ESP_ERR_WIFI_PASSWORD) had already been written to NVS by the
 * caller, so the next boot loaded it, hit the same abort, and the adapter
 * boot-looped until reflashed with a wiped NVS.
 */
static bool start_sta_connect(const char *ssid, const char *pw)
{
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);

    wifi_config_t sta = {0};
    snprintf((char *)sta.sta.ssid, sizeof(sta.sta.ssid), "%s", ssid);
    snprintf((char *)sta.sta.password, sizeof(sta.sta.password), "%s", pw);
    /* Accept any auth mode the AP offers (open, WPA2, WPA3-transition). */
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) rejected \"%s\": %s", ssid,
                 esp_err_to_name(err));
        return false;
    }
    set_status(HR_WIFI_CONNECTING);
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
    return true;
}

/*
 * What the driver will accept: an SSID of 1..32 bytes and a passphrase that is
 * either empty (open network) or 8..63 characters (WPA/WPA2 PSK). Checked
 * before anything is stored so a bad value is answered with HTTP 400 rather
 * than persisted.
 */
static bool credentials_plausible(const char *ssid, const char *pw)
{
    size_t sl = ssid ? strlen(ssid) : 0;
    size_t pl = pw ? strlen(pw) : 0;
    if (sl == 0 || sl > 32) {
        return false;
    }
    /* 64 hex digits is a raw PSK; the driver takes it as well. */
    if (pl != 0 && (pl < 8 || pl > 64)) {
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------- */
/* Event handling                                                        */
/* -------------------------------------------------------------------- */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (s_restarting) {
        /*
         * esp_restart() is stopping the driver. The disconnect it raises is
         * not a lost link to be repaired - re-enabling the AP and calling
         * esp_wifi_connect() into a stack that is being torn down is exactly
         * the kind of work a reboot path should not be doing.
         */
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        /*
         * Associated; DHCP starts now. From here the no-IP watchdog owns
         * "did we get an address, and do we still have one". The slow retry
         * timer is for a station that is NOT associated - if one is pending
         * it must not fire esp_wifi_connect() into a live association.
         */
        wifi_event_sta_connected_t *c = (wifi_event_sta_connected_t *)data;
        uint32_t now = now_ms();
        cancel_sta_retry();
        hr_netwatch_on_assoc(&s_netwatch, now);
        s_link_joins++;
        s_assoc_since_ms = now;
        hr_capture_event("wifi joined \"%.*s\" ch=%u rssi=%d %s=%lus",
                         (int)c->ssid_len, (const char *)c->ssid,
                         (unsigned)c->channel, ap_rssi(),
                         s_ever_up ? "down" : "boot",
                         (unsigned long)((now - (s_ever_up ? s_down_since_ms
                                                           : 0u)) / 1000u));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_BEACON_TIMEOUT) {
        /*
         * The driver has missed the AP's beacons for its inactive time (6 s)
         * and is about to give up on the association (reason 200 follows).
         * Weak or interfered signal looks exactly like this; counted so a
         * marginal spot can be told from a router that went away.
         */
        s_bcn_timeouts++;
        hr_capture_event("wifi beacon-timeout rssi=%d assoc=%lus", ap_rssi(),
                         (unsigned long)((now_ms() - s_assoc_since_ms) / 1000u));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /*
         * Runs in the WiFi event task - MUST NOT block. Reconnect immediately
         * (no vTaskDelay here; that would stall the stack and the setup AP's
         * beacons). We keep the AP up throughout so the user is never locked
         * out. After many failures we stop hammering but stay in APSTA.
         */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        uint32_t now = now_ms();
        bool was_assoc = s_netwatch.associated;
        hr_netwatch_on_disassoc(&s_netwatch, now);
        s_probe_pending = false;
        s_last_reason = d->reason;
        if (was_assoc) {
            s_link_drops++;
            hr_capture_event("wifi lost reason=%u rssi=%d assoc=%lus was=%s",
                             (unsigned)d->reason, (int)d->rssi,
                             (unsigned long)((now - s_assoc_since_ms) / 1000u),
                             status_name(s_status));
        } else {
            /* A join that did not get as far as an association: the network
             * is out of reach (201), or auth/handshake failed. Retried every
             * 30 s, so thinned after the first few. */
            s_join_fails++;
            if (s_join_fails <= STA_RETRY_LIMIT ||
                s_join_fails % JOIN_FAIL_RECORD_EVERY == 0) {
                hr_capture_event("wifi join-failed reason=%u rssi=%d n=%u %s=%lus",
                                 (unsigned)d->reason, (int)d->rssi,
                                 s_join_fails, s_ever_up ? "down" : "boot",
                                 (unsigned long)((now - (s_ever_up
                                                             ? s_down_since_ms
                                                             : 0u)) / 1000u));
            }
        }
        if (s_noip_rejoin_pending) {
            /*
             * The disconnect the no-IP watchdog asked for. Not a lost link:
             * the network is still there, it just stopped handing out
             * addresses to this association. Rejoin at once, STA-only - the
             * setup AP is for a user who needs to change the network, and
             * re-opening it on every rejoin would turn a DHCP outage into a
             * recurring open-AP window (the case F33 closed).
             */
            s_noip_rejoin_pending = false;
            ESP_LOGW(TAG, "left \"%s\" (reason %d) to re-run DHCP; rejoining",
                     s_ssid, d->reason);
            set_status(HR_WIFI_CONNECTING);
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
                arm_sta_retry();
            }
        } else if (s_status == HR_WIFI_CONNECTED || s_status == HR_WIFI_NO_IP ||
                   s_status == HR_WIFI_UNREACHABLE) {
            /*
             * We had a working link and lost it (e.g. router/internet outage).
             * Reconnect, but DO NOT re-broadcast the setup AP if the one-time
             * setup window already closed - an outage must not reopen an attack
             * surface. Only restore the AP if the window is still valid.
             */
            if (!s_ap_window_expired) {
                ESP_LOGW(TAG, "link dropped (reason %d), restoring setup AP + "
                              "reconnecting", d->reason);
                esp_wifi_set_mode(WIFI_MODE_APSTA);
                configure_ap();
                arm_ap_timeout();
            } else {
                ESP_LOGW(TAG, "link dropped (reason %d), reconnecting STA-only "
                              "(setup AP window used)", d->reason);
            }
            set_status(HR_WIFI_CONNECTING);
            /*
             * The result comes back as STA_CONNECTED or another
             * STA_DISCONNECTED (which retries); a refusal here would leave
             * the station waiting for neither, so it gets the slow timer.
             */
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
                arm_sta_retry();
            }
        } else if (++s_sta_retries < STA_RETRY_LIMIT) {
            ESP_LOGW(TAG, "join attempt %d failed (reason %d), retrying",
                     s_sta_retries, d->reason);
            if (esp_wifi_connect() != ESP_OK) {
                arm_sta_retry();
            }
        } else {
            /*
             * The fast retries are spent. This used to stop here for good:
             * no further esp_wifi_connect(), and - once the setup-AP window
             * had closed and the mode had been forced to STA - no AP either.
             * A router reboot longer than eight quick attempts (well under
             * two minutes) left the adapter unreachable on every interface
             * until someone unplugged it from the dryer.
             *
             * Keep trying, slowly. If the AP window is still open the user
             * can also re-submit credentials from the setup page meanwhile.
             */
            if (s_sta_retries == STA_RETRY_LIMIT) {
                ESP_LOGW(TAG, "join failed %d times (reason %d); will keep "
                              "retrying every %us",
                         s_sta_retries, d->reason,
                         (unsigned)(STA_RETRY_BACKOFF_US / 1000000ULL));
            }
            set_status(s_ap_window_expired ? HR_WIFI_CONNECTING
                                           : HR_WIFI_AP_SETUP);
            arm_sta_retry();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        /* Also raised on every lease renewal (ip_changed=false) and when
         * DHCP recovers after a no-IP episode: idempotent by design. */
        uint32_t now = now_ms();
        unsigned long noip_s =
            (unsigned long)(hr_netwatch_noip_for_ms(&s_netwatch, now) / 1000u);
        if (s_status == HR_WIFI_NO_IP) {
            ESP_LOGW(TAG, "IP address back after %lus without one: " IPSTR,
                     noip_s, IP2STR(&e->ip_info.ip));
            hr_capture_event("wifi ip " IPSTR " back after no-ip=%lus",
                             IP2STR(&e->ip_info.ip), noip_s);
        } else if (s_status != HR_WIFI_CONNECTED || e->ip_changed) {
            ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&e->ip_info.ip));
            hr_capture_event("wifi ip " IPSTR " gw " IPSTR " dhcp=%lus rssi=%d",
                             IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw),
                             noip_s, ap_rssi());
        }
        hr_netwatch_tick(&s_netwatch, true, now);
        set_status(HR_WIFI_CONNECTED);
        s_sta_retries = 0;
        cancel_sta_retry();
        cancel_ap_timeout(); /* connected in time; no need to force-close AP */
        /*
         * Security: once we're on the home network, shut the setup AP down
         * entirely (STA-only) so nobody can join HR-Adapter-Setup and try to
         * reconfigure the device while it's in use. The AP is restored
         * automatically if the STA link later drops (see disconnect handler).
         */
        esp_err_t merr = esp_wifi_set_mode(WIFI_MODE_STA);
        if (merr == ESP_OK) {
            ESP_LOGI(TAG, "setup AP disabled (STA-only) for security");
        } else {
            ESP_LOGW(TAG, "could not disable setup AP: %s",
                     esp_err_to_name(merr));
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        /*
         * esp_netif raises this CONFIG_ESP_NETIF_IP_LOST_TIMER_INTERVAL
         * (120 s by default) after the address went to 0.0.0.0 and stayed
         * there. The 2 s poll has long since noticed; this is the belt to
         * its braces, and the one line in the log that names the cause.
         */
        ESP_LOGW(TAG, "IP address lost (DHCP lease not renewed)");
        hr_capture_event("wifi ip lost (lease not renewed)");
        noip_check();
    }
    /* Scan results are collected synchronously in hr_wifi_scan_start(); no
     * SCAN_DONE handling needed here. */
}

/* -------------------------------------------------------------------- */
/* Captive-portal DNS: answer every A query with our AP address.         */
/* -------------------------------------------------------------------- */
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* AP gateway address the portal lives on (default 192.168.4.1). */
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src,
                         &slen);
        if (n < (int)sizeof(uint16_t) * 6) {
            continue;
        }
        /* Only answer standard queries with exactly one question. */
        if ((buf[2] & 0x80) != 0) {
            continue;
        }
        /* Build a minimal response: flip QR bit, one answer A record. */
        buf[2] |= 0x80; /* response */
        buf[3] |= 0x80; /* recursion available */
        buf[6] = 0x00;
        buf[7] = 0x01; /* ANCOUNT = 1 */

        /* Append answer pointing back at the question name (0xC00C). */
        if (n + 16 > (int)sizeof(buf)) {
            continue;
        }
        uint8_t *p = buf + n;
        *p++ = 0xC0;
        *p++ = 0x0C; /* name pointer to offset 12 */
        *p++ = 0x00;
        *p++ = 0x01; /* type A */
        *p++ = 0x00;
        *p++ = 0x01; /* class IN */
        *p++ = 0x00;
        *p++ = 0x00;
        *p++ = 0x00;
        *p++ = 0x3C; /* TTL 60s */
        *p++ = 0x00;
        *p++ = 0x04; /* RDLENGTH 4 */
        memcpy(p, &ip.ip.addr, 4);
        p += 4;

        sendto(sock, buf, p - buf, 0, (struct sockaddr *)&src, slen);
    }
}

/* -------------------------------------------------------------------- */
/* Public API                                                            */
/* -------------------------------------------------------------------- */
void hr_wifi_start(void)
{
    s_status = HR_WIFI_BOOTING;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP, &on_event, NULL, NULL));
    start_noip_poll();

    /*
     * Bring the driver up ONCE, in APSTA, with the setup AP always configured.
     * The AP stays up permanently so the user is never locked out; the STA is
     * connected on demand. esp_wifi_start() must happen before any
     * esp_wifi_connect() call (else ESP_ERR_WIFI_NOT_STARTED).
     */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    configure_ap();
    ESP_ERROR_CHECK(esp_wifi_start());
    /*
     * The open setup AP is on the air from this moment, whatever happens with
     * the stored network, so its five-minute window has to start now as well.
     * It used to be armed only from start_ap_mode() (no credentials) and from
     * the reconnect path - so a boot with stored credentials and the router
     * down broadcast the open AP indefinitely, which is the exact case the
     * window exists for. A successful connection cancels it on GOT_IP.
     */
    arm_ap_timeout();

    /*
     * No power save. The adapter is mains-powered off the dryer's USB port, so
     * the default MIN_MODEM save buys nothing and costs real throughput: it
     * parks the radio between beacons (listen interval 3), which adds
     * multi-second stalls to a ~1.3MB OTA upload and makes the web UI feel
     * laggy. Worth doing purely so a remote firmware update is dependable.
     */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    char ssid[33] = {0}, pw[65] = {0};
    if (load_credentials(ssid, sizeof(ssid), pw, sizeof(pw)) &&
        credentials_plausible(ssid, pw) && start_sta_connect(ssid, pw)) {
        /* connecting; driver is started so esp_wifi_connect() is legal */
    } else {
        /* Nothing stored, or the stored values are ones the driver will not
         * take: stay reachable on the setup AP instead of aborting. */
        start_ap_mode();
    }

    /* Captive-portal DNS is harmless in STA mode too, so always run it. */
    xTaskCreate(dns_task, "hr_dns", 3072, NULL, 4, NULL);
}

hr_wifi_status_t hr_wifi_status(void)
{
    return s_status;
}

void hr_wifi_ip(char *out, size_t cap)
{
    esp_netif_ip_info_t ip = {0};
    if (s_status == HR_WIFI_CONNECTED || s_status == HR_WIFI_NO_IP ||
        s_status == HR_WIFI_UNREACHABLE) {
        /* The station's real address - 0.0.0.0 while it has none, which is
         * exactly what the status page should say then. */
        esp_netif_get_ip_info(s_sta_netif, &ip);
    } else {
        esp_netif_get_ip_info(s_ap_netif, &ip);
    }
    snprintf(out, cap, IPSTR, IP2STR(&ip.ip));
}

void hr_wifi_noip_stats(hr_wifi_noip_stats_t *out)
{
    uint32_t now = now_ms();
    out->associated = s_netwatch.associated;
    out->noip_s = hr_netwatch_noip_for_ms(&s_netwatch, now) / 1000u;
    out->episodes = (unsigned)s_netwatch.episodes;
    out->dhcp_restarts = (unsigned)s_netwatch.dhcp_restarts;
    out->reconnects = (unsigned)s_netwatch.reconnects;
    out->dead_s = hr_netwatch_dead_for_ms(&s_netwatch, now) / 1000u;
    out->probe_misses = (unsigned)s_netwatch.probe_misses;
    out->dead_episodes = (unsigned)s_netwatch.dead_episodes;
    out->dead_reconnects = (unsigned)s_netwatch.dead_reconnects;
}

void hr_wifi_link_stats(hr_wifi_link_stats_t *out)
{
    uint32_t now = now_ms();
    out->joins = s_link_joins;
    out->drops = s_link_drops;
    out->join_fails = s_join_fails;
    out->bcn_timeouts = s_bcn_timeouts;
    out->last_reason = s_last_reason;
    out->rssi_dbm = s_netwatch.associated ? ap_rssi() : 0;
    out->assoc_s = s_netwatch.associated ? (now - s_assoc_since_ms) / 1000u : 0;
    out->up_s = s_status == HR_WIFI_CONNECTED ? (now - s_up_since_ms) / 1000u : 0;
    out->down_s = (s_status != HR_WIFI_CONNECTED && s_ever_up)
                      ? (now - s_down_since_ms) / 1000u
                      : 0;
}

/*
 * RSSI as a percentage.
 *
 * The captured adapter reports 0 when unassociated and values in the 40-90
 * range when connected, which is a percentage rather than dBm. Mapping -90dBm
 * to 0 and -30dBm to 100 puts a normal home signal in that same band.
 */
int hr_wifi_rssi_pct(void)
{
    wifi_ap_record_t ap;
    if (s_status != HR_WIFI_CONNECTED ||
        esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    int pct = (ap.rssi + 90) * 100 / 60;
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    return pct;
}

int hr_wifi_rssi_dbm(void)
{
    wifi_ap_record_t ap;
    if (s_status != HR_WIFI_CONNECTED ||
        esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

long hr_wifi_ap_remaining_s(void)
{
    if (s_status != HR_WIFI_AP_SETUP || s_ap_window_expired ||
        s_ap_opened_us == 0) {
        return 0;
    }
    int64_t left_us = (int64_t)AP_OPEN_WINDOW_US -
                      (esp_timer_get_time() - s_ap_opened_us);
    return left_us > 0 ? (long)(left_us / 1000000) : 0;
}

bool hr_wifi_ap_window_expired(void)
{
    return s_ap_window_expired;
}

void hr_wifi_current_ssid(char *out, size_t cap)
{
    snprintf(out, cap, "%s", s_ssid);
}

bool hr_wifi_set_credentials(const char *ssid, const char *password)
{
    if (password == NULL) {
        password = "";
    }
    if (!credentials_plausible(ssid, password)) {
        return false;
    }
    /*
     * Apply first, store second. If the driver refuses the configuration the
     * caller gets a 400 and nothing has been written - the old credentials, if
     * any, stay in NVS and keep working across the next reboot.
     */
    s_sta_retries = 0;
    cancel_sta_retry();
    s_noip_rejoin_pending = false;
    esp_wifi_disconnect();
    if (!start_sta_connect(ssid, password)) {
        return false;
    }
    if (!store_credentials(ssid, password)) {
        ESP_LOGE(TAG, "credentials accepted by the driver but not saved");
        return false;
    }
    return true;
}

void hr_wifi_forget(void)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
        nvs_erase_all(nh);
        nvs_commit(nh);
        nvs_close(nh);
    }
    /*
     * Erasing the stored network used to be all this did: the station stayed
     * associated, the retry logic kept its SSID, and after the AP window had
     * closed start_ap_mode() refused to bring the AP back - so "Forget" did
     * nothing visible until the next reboot.
     *
     * Leave the network for real, drop the STA config so nothing reconnects
     * to it, and treat the explicit (PIN-guarded) request as permission for
     * one more setup window: the user just asked for setup mode.
     */
    cancel_sta_retry();
    s_sta_retries = 0;
    s_ssid[0] = '\0';
    s_noip_rejoin_pending = false;
    esp_wifi_disconnect();
    wifi_config_t empty = {0};
    esp_wifi_set_config(WIFI_IF_STA, &empty);
    s_ap_window_expired = false;
    esp_err_t merr = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (merr != ESP_OK) {
        ESP_LOGW(TAG, "could not re-enable the setup AP: %s",
                 esp_err_to_name(merr));
    }
    start_ap_mode();
}

void hr_wifi_prepare_restart(void)
{
    s_restarting = true;
    cancel_sta_retry();
    cancel_ap_timeout();
    stop_noip_poll();
}

void hr_wifi_scan_start(void)
{
    /*
     * Blocking scan so the HTTP handler returns fresh results in one request.
     * The earlier async design returned the PREVIOUS scan (empty on first
     * call), so the UI never showed anything. A blocking scan takes ~1-2s;
     * we guard against overlapping calls and against scanning while a STA
     * connect is in flight (which would return ESP_ERR_WIFI_STATE).
     */
    if (s_scanning) {
        return;
    }
    if (s_status == HR_WIFI_CONNECTING) {
        return; /* can't scan mid-connect; UI keeps last results */
    }
    /*
     * Rate-limit. Each scan parks the single httpd worker for 1-2 s and takes
     * the radio off-channel, which stalls MQTT keep-alives and an OTA upload
     * in progress. /api/scan is an unauthenticated GET that the setup page
     * polls by itself, so anyone on the LAN could keep the radio scanning
     * continuously. Within the window the last results are served instead.
     */
    static uint32_t last_scan_ms;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (last_scan_ms != 0 && (uint32_t)(now - last_scan_ms) < 10000u) {
        return;
    }
    last_scan_ms = now;
    s_scanning = true;

    wifi_scan_config_t cfg = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&cfg, true); /* block until done */
    if (err == ESP_OK) {
        uint16_t n = MAX_SCAN;
        esp_wifi_scan_get_ap_records(&n, s_scan);
        s_scan_count = n;
        ESP_LOGI(TAG, "scan found %u networks", n);
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }
    s_scanning = false;
}

/*
 * Build the JSON array. Every write is checked against the space left: the
 * previous version added snprintf's *desired* length to the offset even when
 * the output had been truncated, and with enough long SSIDs in range the
 * offset ran past the caller's buffer, the final "]" was written beyond it and
 * the caller sent that many bytes to the client. An entry that does not fit
 * is left out rather than truncated, so the result is always valid JSON.
 */
size_t hr_wifi_scan_result_json(char *out, size_t cap)
{
    if (out == NULL || cap < 3) {
        if (out != NULL && cap > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    size_t o = 0;
    out[o++] = '[';
    bool first = true;
    for (uint16_t i = 0; i < s_scan_count; i++) {
        char ssid_esc[64];
        /* SSIDs can contain quotes/backslashes; escape for JSON. */
        size_t e = 0;
        for (size_t k = 0; s_scan[i].ssid[k] && e < sizeof(ssid_esc) - 2; k++) {
            char c = (char)s_scan[i].ssid[k];
            if (c == '"' || c == '\\') {
                ssid_esc[e++] = '\\';
            }
            ssid_esc[e++] = c;
        }
        ssid_esc[e] = '\0';
        bool secure = s_scan[i].authmode != WIFI_AUTH_OPEN;
        /* Leave room for the closing bracket and the NUL. */
        size_t room = cap - o - 2;
        int w = snprintf(out + o, room,
                         "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                         first ? "" : ",", ssid_esc, s_scan[i].rssi,
                         secure ? "true" : "false");
        if (w < 0 || (size_t)w >= room) {
            out[o] = '\0'; /* undo the partial entry */
            break;
        }
        o += (size_t)w;
        first = false;
    }
    out[o++] = ']';
    out[o] = '\0';
    return o;
}
