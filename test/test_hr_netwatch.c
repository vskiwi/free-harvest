/*
 * The no-IP watchdog: a station that is associated but has no address.
 *
 * The property that matters most is the one the bug report is about: an
 * association that keeps its RSSI but loses its DHCP lease must be reported
 * as NOT connected, and must be acted on - DHCP restarted, then the network
 * rejoined - within bounded time, with the rejoins backing off rather than
 * hammering. And an ordinary join, where the address arrives in a second or
 * two, must trip nothing.
 */
#include "hr_netwatch.h"
#include "test_util.h"

/* Short timings so the tests read in seconds: 2 s grace, DHCP restart at
 * 5 s, rejoin at 10 s, rejoin back-off capped at 40 s. */
static const hr_netwatch_cfg_t CFG = {
    .grace_ms = 2000,
    .dhcp_restart_ms = 5000,
    .reconnect_ms = 10000,
    .reconnect_max_ms = 40000,
};

#define S(x) ((uint32_t)((x) * 1000u))

/* Advance the clock one second at a time, ticking with a fixed have_ip, and
 * return the first action seen (or NONE). Stops at `until`. */
static hr_netwatch_action_t run_until(hr_netwatch_t *w, bool have_ip,
                                      uint32_t *t, uint32_t until)
{
    while (*t < until) {
        *t += 1000;
        hr_netwatch_action_t a = hr_netwatch_tick(w, have_ip, *t);
        if (a != HR_NETWATCH_NONE) {
            return a;
        }
    }
    return HR_NETWATCH_NONE;
}

static void test_defaults(void)
{
    TEST_CASE("zero config fills in the defaults, cap never below the base");
    hr_netwatch_t w;
    hr_netwatch_init(&w, NULL);
    CHECK_INT(w.cfg.grace_ms, 5000);
    CHECK_INT(w.cfg.dhcp_restart_ms, 15000);
    CHECK_INT(w.cfg.reconnect_ms, 45000);
    CHECK_INT(w.cfg.reconnect_max_ms, 360000);

    hr_netwatch_cfg_t big = {.reconnect_ms = 900000, .reconnect_max_ms = 1000};
    hr_netwatch_init(&w, &big);
    CHECK_INT(w.cfg.reconnect_max_ms, 900000);
}

static void test_normal_join_is_quiet(void)
{
    TEST_CASE("a join whose address arrives within the grace trips nothing");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = S(100);
    CHECK_INT(hr_netwatch_tick(&w, false, t), HR_NETWATCH_NONE); /* idle */
    CHECK(!hr_netwatch_no_ip(&w, t));

    hr_netwatch_on_assoc(&w, t);
    t += 1500; /* DHCP took 1.5 s */
    CHECK(!hr_netwatch_no_ip(&w, t));
    CHECK_INT(hr_netwatch_tick(&w, true, t), HR_NETWATCH_NONE);
    CHECK(!hr_netwatch_no_ip(&w, t));
    CHECK_INT(hr_netwatch_noip_for_ms(&w, t), 0);
    CHECK_INT(w.episodes, 0);

    /* Hours of renewals later, still fine. */
    CHECK_INT(run_until(&w, true, &t, S(4000)), HR_NETWATCH_NONE);
    CHECK_INT(w.episodes, 0);
    CHECK_INT(w.dhcp_restarts, 0);
    CHECK_INT(w.reconnects, 0);
}

static void test_lost_lease_is_reported_then_repaired(void)
{
    TEST_CASE("associated + ip==0 -> NO_IP after grace, DHCP restart, rejoin");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = S(10);
    hr_netwatch_on_assoc(&w, t);
    t += 1000;
    hr_netwatch_tick(&w, true, t); /* got an address */

    /* The lease expires: lwIP drops the address, the association stays. */
    uint32_t lost = t + S(3600);
    t = lost;
    CHECK_INT(hr_netwatch_tick(&w, false, t), HR_NETWATCH_NONE);
    CHECK(!hr_netwatch_no_ip(&w, t)); /* inside the grace: not yet */
    CHECK(w.associated);

    t = lost + S(2);
    hr_netwatch_tick(&w, false, t);
    CHECK(hr_netwatch_no_ip(&w, t)); /* grace over: report it */
    CHECK_INT(hr_netwatch_noip_for_ms(&w, t), S(2));
    CHECK_INT(w.episodes, 1);

    /* First remedy at 5 s: restart the DHCP client, once. */
    t = lost + S(2);
    CHECK_INT(run_until(&w, false, &t, lost + S(20)),
              HR_NETWATCH_DHCP_RESTART);
    CHECK_INT(t, lost + S(5));
    CHECK_INT(w.dhcp_restarts, 1);

    /* Second remedy at 10 s: rejoin. Nothing else in between. */
    CHECK_INT(run_until(&w, false, &t, lost + S(20)), HR_NETWATCH_RECONNECT);
    CHECK_INT(t, lost + S(10));
    CHECK_INT(w.reconnects, 1);
    CHECK(hr_netwatch_no_ip(&w, t)); /* still not connected */

    /* The driver reports the disassociation; we are no longer "no IP",
     * we are simply not associated. */
    hr_netwatch_on_disassoc(&w, t);
    CHECK(!hr_netwatch_no_ip(&w, t));
    CHECK_INT(hr_netwatch_noip_for_ms(&w, t), 0);
    CHECK_INT(hr_netwatch_tick(&w, false, t), HR_NETWATCH_NONE);

    /* Rejoined, DHCP answers this time: episode over, back-off reset. */
    t += 1000;
    hr_netwatch_on_assoc(&w, t);
    t += 1000;
    CHECK_INT(hr_netwatch_tick(&w, true, t), HR_NETWATCH_NONE);
    CHECK(!hr_netwatch_no_ip(&w, t));
    CHECK_INT(w.backoff_n, 0);
    CHECK_INT(hr_netwatch_reconnect_delay_ms(&w), S(10));
    CHECK_INT(w.episodes, 1);
}

static void test_persistent_no_dhcp_backs_off(void)
{
    TEST_CASE("a network that never answers is rejoined with doubling waits");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = 0;
    uint32_t expected_wait[] = {S(10), S(20), S(40), S(40), S(40)};
    for (unsigned i = 0; i < 5; i++) {
        uint32_t joined = t;
        hr_netwatch_on_assoc(&w, t);
        hr_netwatch_action_t a = run_until(&w, false, &t, joined + S(300));
        CHECK_INT(a, HR_NETWATCH_DHCP_RESTART);
        CHECK_INT(t, joined + S(5)); /* DHCP restart timing never grows */
        a = run_until(&w, false, &t, joined + S(300));
        CHECK_INT(a, HR_NETWATCH_RECONNECT);
        CHECK_INT(t - joined, expected_wait[i]); /* the rejoin wait does */
        CHECK_INT(w.reconnects, i + 1);
        hr_netwatch_on_disassoc(&w, t);
        t += 1000;
    }
    CHECK_INT(w.dhcp_restarts, 5);
    CHECK_INT(w.episodes, 5);
    /* Every episode was reported as NO_IP once the grace had passed. */
}

static void test_join_that_never_gets_an_address(void)
{
    TEST_CASE("a first join with no DHCP answer is handled the same way");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = S(5);
    hr_netwatch_on_assoc(&w, t);
    hr_netwatch_action_t a = run_until(&w, false, &t, S(60));
    CHECK_INT(a, HR_NETWATCH_DHCP_RESTART);
    CHECK_INT(t, S(10));
    CHECK(hr_netwatch_no_ip(&w, t));
    a = run_until(&w, false, &t, S(60));
    CHECK_INT(a, HR_NETWATCH_RECONNECT);
    CHECK_INT(t, S(15));
}

static void test_reconnect_repeats_if_disconnect_never_comes(void)
{
    TEST_CASE("if the driver never reports the leave, ask again after the wait");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = 0;
    hr_netwatch_on_assoc(&w, t);
    run_until(&w, false, &t, S(100)); /* DHCP restart */
    run_until(&w, false, &t, S(100)); /* rejoin #1 at 10 s */
    CHECK_INT(t, S(10));
    /* No on_disassoc(). The next request comes after the doubled wait. */
    hr_netwatch_action_t a = run_until(&w, false, &t, S(100));
    CHECK_INT(a, HR_NETWATCH_RECONNECT);
    CHECK_INT(t, S(30));
    CHECK_INT(w.reconnects, 2);
    a = run_until(&w, false, &t, S(200));
    CHECK_INT(a, HR_NETWATCH_RECONNECT);
    CHECK_INT(t, S(70)); /* 40 s: the cap */
}

static void test_not_associated_is_never_no_ip(void)
{
    TEST_CASE("without an association there is nothing to watch");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = 0;
    CHECK_INT(run_until(&w, false, &t, S(600)), HR_NETWATCH_NONE);
    CHECK(!hr_netwatch_no_ip(&w, t));
    CHECK_INT(w.episodes, 0);
    /* An address report while not associated (stale event) is ignored. */
    CHECK_INT(hr_netwatch_tick(&w, true, t), HR_NETWATCH_NONE);
    CHECK(!w.have_ip);
}

static void test_clock_wrap(void)
{
    TEST_CASE("a millisecond clock wrapping through zero does not confuse it");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = 0xFFFFFFFFu - S(3);
    hr_netwatch_on_assoc(&w, t);
    /* Step by hand: run_until()'s "t < until" cannot cross the wrap. */
    hr_netwatch_action_t a = HR_NETWATCH_NONE;
    int steps = 0;
    while (a == HR_NETWATCH_NONE && steps < 30) {
        t += 1000;
        steps++;
        a = hr_netwatch_tick(&w, false, t);
    }
    CHECK_INT(a, HR_NETWATCH_DHCP_RESTART);
    CHECK_INT(steps, 5);
    CHECK_INT(t, S(2) - 1); /* 5 s after the assoc, across the wrap */
    CHECK(hr_netwatch_no_ip(&w, t));
}

/* Associate and get an address at t; the state a probe runs from. */
static void join_with_ip(hr_netwatch_t *w, uint32_t *t)
{
    hr_netwatch_on_assoc(w, *t);
    *t += 1000;
    CHECK_INT(hr_netwatch_tick(w, true, *t), HR_NETWATCH_NONE);
}

static void test_probe_answered_is_quiet(void)
{
    TEST_CASE("a gateway that answers, with the odd miss, trips nothing");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    CHECK_INT(w.cfg.probe_miss_limit, 3);
    uint32_t t = S(100);
    join_with_ip(&w, &t);
    for (int i = 0; i < 200; i++) {
        t += S(20);
        /* Every fifth probe lost: a weak link, not a dead one. */
        bool ok = (i % 5) != 0;
        CHECK_INT(hr_netwatch_on_probe(&w, ok, t), HR_NETWATCH_NONE);
        CHECK(!hr_netwatch_dead(&w));
    }
    CHECK_INT(w.dead_episodes, 0);
    CHECK_INT(w.dead_reconnects, 0);
    CHECK_INT(hr_netwatch_dead_for_ms(&w, t), 0);
    /* Two misses then an answer: the count starts over. */
    t += S(20);
    CHECK_INT(hr_netwatch_on_probe(&w, false, t), HR_NETWATCH_NONE);
    t += S(20);
    CHECK_INT(hr_netwatch_on_probe(&w, false, t), HR_NETWATCH_NONE);
    CHECK_INT(w.probe_misses, 2);
    t += S(20);
    CHECK_INT(hr_netwatch_on_probe(&w, true, t), HR_NETWATCH_NONE);
    CHECK_INT(w.probe_misses, 0);
}

static void test_dead_link_is_rejoined_after_the_limit(void)
{
    TEST_CASE("address held, gateway silent 3x in a row -> DEAD, rejoin");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = S(10);
    join_with_ip(&w, &t);

    /* The station's frames stop getting across; beacons still arrive, the
     * driver stays associated, the lease is valid. */
    uint32_t silent = t + S(600);
    t = silent;
    CHECK_INT(hr_netwatch_on_probe(&w, false, t), HR_NETWATCH_NONE);
    t += S(20);
    CHECK_INT(hr_netwatch_on_probe(&w, false, t), HR_NETWATCH_NONE);
    CHECK(!hr_netwatch_dead(&w)); /* two misses: not yet */
    CHECK(!hr_netwatch_no_ip(&w, t)); /* and it is not a no-IP case */
    t += S(20);
    CHECK_INT(hr_netwatch_on_probe(&w, false, t), HR_NETWATCH_RECONNECT);
    CHECK(hr_netwatch_dead(&w));
    CHECK_INT(hr_netwatch_dead_for_ms(&w, t), 0); /* just declared */
    CHECK_INT(w.dead_episodes, 1);
    CHECK_INT(w.dead_reconnects, 1);
    CHECK_INT(t - silent, S(40)); /* third probe at 40 s: bounded */
    t += S(5);
    CHECK_INT(hr_netwatch_dead_for_ms(&w, t), S(5));

    /* The driver leaves; dead is a property of an association. */
    hr_netwatch_on_disassoc(&w, t);
    CHECK(!hr_netwatch_dead(&w));
    CHECK_INT(hr_netwatch_dead_for_ms(&w, t), 0);

    /* Rejoined, address back, gateway answers: clean slate. */
    t += S(2);
    join_with_ip(&w, &t);
    t += S(20);
    CHECK_INT(hr_netwatch_on_probe(&w, true, t), HR_NETWATCH_NONE);
    CHECK(!hr_netwatch_dead(&w));
    CHECK_INT(w.dead_backoff_n, 0);
    CHECK_INT(hr_netwatch_probe_miss_limit(&w), 3);
    CHECK_INT(w.dead_episodes, 1);
    /* The no-IP counters were never involved. */
    CHECK_INT(w.episodes, 0);
    CHECK_INT(w.reconnects, 0);
}

static void test_dead_link_rejoins_back_off(void)
{
    TEST_CASE("rejoins that bring an address but no answer need 3,6,12,24,48,48 misses");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = 0;
    uint32_t expected_misses[] = {3, 6, 12, 24, 48, 48};
    for (unsigned i = 0; i < 6; i++) {
        join_with_ip(&w, &t);
        CHECK_INT(hr_netwatch_probe_miss_limit(&w), expected_misses[i]);
        unsigned misses = 0;
        hr_netwatch_action_t a = HR_NETWATCH_NONE;
        while (a == HR_NETWATCH_NONE && misses < 100) {
            t += S(20);
            misses++;
            a = hr_netwatch_on_probe(&w, false, t);
        }
        CHECK_INT(a, HR_NETWATCH_RECONNECT);
        CHECK_INT(misses, expected_misses[i]);
        CHECK_INT(w.dead_reconnects, i + 1);
        hr_netwatch_on_disassoc(&w, t);
        t += S(2);
    }
    CHECK_INT(w.dead_episodes, 6);
}

static void test_dead_link_repeats_if_disconnect_never_comes(void)
{
    TEST_CASE("if the leave is not reported, ask again after more misses");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = 0;
    join_with_ip(&w, &t);
    for (int i = 0; i < 3; i++) {
        t += S(20);
        hr_netwatch_on_probe(&w, false, t);
    }
    CHECK(hr_netwatch_dead(&w));
    uint32_t declared = t;
    /* No on_disassoc(). Still dead, still one episode; the next request
     * comes after six more misses. */
    unsigned misses = 0;
    hr_netwatch_action_t a = HR_NETWATCH_NONE;
    while (a == HR_NETWATCH_NONE && misses < 100) {
        t += S(20);
        misses++;
        a = hr_netwatch_on_probe(&w, false, t);
        CHECK(hr_netwatch_dead(&w));
    }
    CHECK_INT(a, HR_NETWATCH_RECONNECT);
    CHECK_INT(misses, 6);
    CHECK_INT(w.dead_episodes, 1);
    CHECK_INT(w.dead_reconnects, 2);
    CHECK_INT(hr_netwatch_dead_for_ms(&w, t), t - declared);
}

static void test_probe_without_address_is_ignored(void)
{
    TEST_CASE("misses while there is no address, or no association, count for nothing");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = 0;
    for (int i = 0; i < 10; i++) {
        t += S(20);
        CHECK_INT(hr_netwatch_on_probe(&w, false, t), HR_NETWATCH_NONE);
    }
    CHECK_INT(w.probe_misses, 0);
    hr_netwatch_on_assoc(&w, t); /* associated, DHCP still running */
    for (int i = 0; i < 10; i++) {
        t += 1000;
        CHECK_INT(hr_netwatch_on_probe(&w, false, t), HR_NETWATCH_NONE);
    }
    CHECK_INT(w.probe_misses, 0);
    CHECK(!hr_netwatch_dead(&w));
    /* Address arrives, then two misses, then the lease is lost: the no-IP
     * path takes over and the miss count is dropped with it. */
    hr_netwatch_tick(&w, true, t);
    t += S(20);
    hr_netwatch_on_probe(&w, false, t);
    t += S(20);
    hr_netwatch_on_probe(&w, false, t);
    CHECK_INT(w.probe_misses, 2);
    t += S(1);
    hr_netwatch_tick(&w, false, t);
    CHECK_INT(w.probe_misses, 0);
    t += S(20);
    CHECK_INT(hr_netwatch_on_probe(&w, false, t), HR_NETWATCH_NONE);
    CHECK(!hr_netwatch_dead(&w));
}

int main(void)
{
    test_defaults();
    test_normal_join_is_quiet();
    test_lost_lease_is_reported_then_repaired();
    test_persistent_no_dhcp_backs_off();
    test_join_that_never_gets_an_address();
    test_reconnect_repeats_if_disconnect_never_comes();
    test_not_associated_is_never_no_ip();
    test_clock_wrap();
    test_probe_answered_is_quiet();
    test_dead_link_is_rejoined_after_the_limit();
    test_dead_link_rejoins_back_off();
    test_dead_link_repeats_if_disconnect_never_comes();
    test_probe_without_address_is_ignored();
    return TEST_REPORT();
}
