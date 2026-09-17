/*
 * No-IP watchdog decision logic. See hr_netwatch.h for the why.
 */
#include "hr_netwatch.h"

#include <string.h>

#define DEFAULT_GRACE_MS         (5u * 1000u)
#define DEFAULT_DHCP_RESTART_MS  (15u * 1000u)
#define DEFAULT_RECONNECT_MS     (45u * 1000u)
#define DEFAULT_RECONNECT_MAX_MS (6u * 60u * 1000u)
#define BACKOFF_SHIFT_CAP        10u /* 2^10 x base is already past any cap */
#define DEFAULT_PROBE_MISS_LIMIT 3u
#define DEAD_BACKOFF_SHIFT_CAP   4u  /* misses needed grow up to 16x */

void hr_netwatch_init(hr_netwatch_t *w, const hr_netwatch_cfg_t *cfg)
{
    memset(w, 0, sizeof(*w));
    if (cfg != NULL) {
        w->cfg = *cfg;
    }
    if (w->cfg.grace_ms == 0) {
        w->cfg.grace_ms = DEFAULT_GRACE_MS;
    }
    if (w->cfg.dhcp_restart_ms == 0) {
        w->cfg.dhcp_restart_ms = DEFAULT_DHCP_RESTART_MS;
    }
    if (w->cfg.reconnect_ms == 0) {
        w->cfg.reconnect_ms = DEFAULT_RECONNECT_MS;
    }
    if (w->cfg.reconnect_max_ms < w->cfg.reconnect_ms) {
        w->cfg.reconnect_max_ms = w->cfg.reconnect_ms > DEFAULT_RECONNECT_MAX_MS
                                      ? w->cfg.reconnect_ms
                                      : DEFAULT_RECONNECT_MAX_MS;
    }
    if (w->cfg.probe_miss_limit == 0) {
        w->cfg.probe_miss_limit = DEFAULT_PROBE_MISS_LIMIT;
    }
}

static void clear_dead(hr_netwatch_t *w)
{
    w->probe_misses = 0;
    w->dead = false;
}

static void begin_episode(hr_netwatch_t *w, uint32_t now_ms)
{
    w->in_episode = true;
    w->reported = false;
    w->noip_since_ms = now_ms;
    w->stage = 0;
}

static void end_episode(hr_netwatch_t *w)
{
    w->in_episode = false;
    w->reported = false;
    w->stage = 0;
}

void hr_netwatch_on_assoc(hr_netwatch_t *w, uint32_t now_ms)
{
    w->associated = true;
    w->have_ip = false;
    /* The DHCP clock starts at association: a join that never yields an
     * address is the same problem as a lease that was lost. The back-off
     * count is kept - a rejoin is what got us here. */
    begin_episode(w, now_ms);
}

void hr_netwatch_on_disassoc(hr_netwatch_t *w, uint32_t now_ms)
{
    (void)now_ms;
    w->associated = false;
    w->have_ip = false;
    end_episode(w);
    /* Not associated is a different state from "associated and silent";
     * the dead-link back-off count survives until a probe is answered. */
    clear_dead(w);
}

uint32_t hr_netwatch_reconnect_delay_ms(const hr_netwatch_t *w)
{
    uint32_t n = w->backoff_n < BACKOFF_SHIFT_CAP ? w->backoff_n
                                                  : BACKOFF_SHIFT_CAP;
    uint64_t d = (uint64_t)w->cfg.reconnect_ms << n;
    if (d > w->cfg.reconnect_max_ms) {
        d = w->cfg.reconnect_max_ms;
    }
    return (uint32_t)d;
}

hr_netwatch_action_t hr_netwatch_tick(hr_netwatch_t *w, bool have_ip,
                                      uint32_t now_ms)
{
    if (!w->associated) {
        return HR_NETWATCH_NONE;
    }
    if (have_ip) {
        w->have_ip = true;
        w->backoff_n = 0; /* the network answered; start clean next time */
        end_episode(w);
        return HR_NETWATCH_NONE;
    }
    w->have_ip = false;
    /* The address went; the no-IP remedies take over from the probe. */
    clear_dead(w);
    if (!w->in_episode) {
        begin_episode(w, now_ms);
    }
    uint32_t elapsed = now_ms - w->noip_since_ms;
    if (!w->reported && elapsed >= w->cfg.grace_ms) {
        w->reported = true;
        w->episodes++;
    }
    if (w->stage == 0) {
        if (elapsed >= w->cfg.dhcp_restart_ms) {
            w->stage = 1;
            w->dhcp_restarts++;
            w->last_action_ms = now_ms;
            return HR_NETWATCH_DHCP_RESTART;
        }
        return HR_NETWATCH_NONE;
    }
    uint32_t delay = hr_netwatch_reconnect_delay_ms(w);
    if (w->stage == 1) {
        if (elapsed >= delay) {
            w->stage = 2;
            w->reconnects++;
            w->backoff_n++;
            w->last_action_ms = now_ms;
            return HR_NETWATCH_RECONNECT;
        }
        return HR_NETWATCH_NONE;
    }
    /*
     * Stage 2: a reconnect was issued. Normally the driver reports the
     * disassociation and the caller resets us through on_disassoc(); if it
     * does not (the request was refused, or the rejoin happened without our
     * seeing the events) ask again after the same, growing, delay.
     */
    if (now_ms - w->last_action_ms >= delay) {
        w->reconnects++;
        w->backoff_n++;
        w->last_action_ms = now_ms;
        return HR_NETWATCH_RECONNECT;
    }
    return HR_NETWATCH_NONE;
}

bool hr_netwatch_no_ip(const hr_netwatch_t *w, uint32_t now_ms)
{
    return w->associated && !w->have_ip && w->in_episode &&
           (now_ms - w->noip_since_ms) >= w->cfg.grace_ms;
}

uint32_t hr_netwatch_noip_for_ms(const hr_netwatch_t *w, uint32_t now_ms)
{
    if (!w->associated || w->have_ip || !w->in_episode) {
        return 0;
    }
    return now_ms - w->noip_since_ms;
}

uint32_t hr_netwatch_probe_miss_limit(const hr_netwatch_t *w)
{
    uint32_t n = w->dead_backoff_n < DEAD_BACKOFF_SHIFT_CAP
                     ? w->dead_backoff_n
                     : DEAD_BACKOFF_SHIFT_CAP;
    return w->cfg.probe_miss_limit << n;
}

hr_netwatch_action_t hr_netwatch_on_probe(hr_netwatch_t *w, bool answered,
                                          uint32_t now_ms)
{
    if (!w->associated || !w->have_ip) {
        /* Nothing to probe through; a stale result is not evidence. */
        return HR_NETWATCH_NONE;
    }
    if (answered) {
        /* The network is there. Whatever the rejoins cost, start clean. */
        clear_dead(w);
        w->dead_backoff_n = 0;
        return HR_NETWATCH_NONE;
    }
    w->probe_misses++;
    if (w->probe_misses < hr_netwatch_probe_miss_limit(w)) {
        return HR_NETWATCH_NONE;
    }
    /*
     * Enough silence. The address is fine and the association is fine as
     * far as the driver knows, so nothing short of leaving and rejoining
     * makes the router look at this station afresh. Ask once per limit
     * reached; the misses needed double while the answers stay away, so a
     * router that is really gone is not rejoined every minute for hours.
     */
    if (!w->dead) {
        w->dead = true;
        w->dead_since_ms = now_ms;
        w->dead_episodes++;
    }
    w->probe_misses = 0;
    w->dead_reconnects++;
    w->dead_backoff_n++;
    w->last_action_ms = now_ms;
    return HR_NETWATCH_RECONNECT;
}

bool hr_netwatch_dead(const hr_netwatch_t *w)
{
    return w->associated && w->have_ip && w->dead;
}

uint32_t hr_netwatch_dead_for_ms(const hr_netwatch_t *w, uint32_t now_ms)
{
    return hr_netwatch_dead(w) ? now_ms - w->dead_since_ms : 0;
}
