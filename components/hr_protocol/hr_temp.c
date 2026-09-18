#include "hr_temp.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

hr_temp_unit_t hr_temp_resolve(hr_temp_pref_t pref, int dryer_unit,
                               hr_temp_unit_t fallback)
{
    switch (pref) {
    case HR_TEMP_PREF_F:
        return HR_TEMP_F;
    case HR_TEMP_PREF_C:
        return HR_TEMP_C;
    case HR_TEMP_PREF_AUTO:
    default:
        break;
    }
    if (dryer_unit == (int)HR_TEMP_F || dryer_unit == (int)HR_TEMP_C) {
        return (hr_temp_unit_t)dryer_unit;
    }
    return fallback;
}

/* (f - 32) * 50 / 9, rounded half away from zero. */
long hr_temp_f_to_c_x10(long f)
{
    long num = (f - 32) * 50;
    if (num >= 0) {
        return (num + 4) / 9;
    }
    return -((-num + 4) / 9);
}

long hr_temp_f_to_c(long f)
{
    long num = (f - 32) * 5;
    if (num >= 0) {
        return (num + 4) / 9;
    }
    return -((-num + 4) / 9);
}

double hr_temp_from_f(long f, hr_temp_unit_t unit)
{
    if (unit == HR_TEMP_C) {
        return (double)hr_temp_f_to_c_x10(f) / 10.0;
    }
    return (double)f;
}

const char *hr_temp_unit_letter(hr_temp_unit_t unit)
{
    return unit == HR_TEMP_C ? "C" : "F";
}

const char *hr_temp_pref_str(hr_temp_pref_t pref)
{
    switch (pref) {
    case HR_TEMP_PREF_F:
        return "f";
    case HR_TEMP_PREF_C:
        return "c";
    case HR_TEMP_PREF_AUTO:
    default:
        return "auto";
    }
}

static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

bool hr_temp_pref_parse(const char *s, hr_temp_pref_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    if (ieq(s, "f") || ieq(s, "fahrenheit") || ieq(s, "imperial")) {
        *out = HR_TEMP_PREF_F;
        return true;
    }
    if (ieq(s, "c") || ieq(s, "celsius") || ieq(s, "metric")) {
        *out = HR_TEMP_PREF_C;
        return true;
    }
    if (ieq(s, "auto")) {
        *out = HR_TEMP_PREF_AUTO;
        return true;
    }
    return false;
}

size_t hr_temp_fmt_num(long f, hr_temp_unit_t unit, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    int n;
    if (unit == HR_TEMP_C) {
        /* Sign handled by hand so "-0.6" keeps its minus (whole part 0). */
        long x10 = hr_temp_f_to_c_x10(f);
        long mag = x10 < 0 ? -x10 : x10;
        n = snprintf(out, cap, "%s%ld.%ld", x10 < 0 ? "-" : "", mag / 10,
                     mag % 10);
    } else {
        n = snprintf(out, cap, "%ld", f);
    }
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

/* Case-insensitive whole-word search: `word` bounded by non-letters. */
static bool has_word_ci(const char *s, size_t n, const char *word)
{
    const size_t wl = strlen(word);
    for (size_t i = 0; i + wl <= n; i++) {
        if (i > 0 && isalpha((unsigned char)s[i - 1])) {
            continue;
        }
        if (i + wl < n && isalpha((unsigned char)s[i + wl])) {
            continue;
        }
        size_t k = 0;
        while (k < wl &&
               tolower((unsigned char)s[i + k]) == (unsigned char)word[k]) {
            k++;
        }
        if (k == wl) {
            return true;
        }
    }
    return false;
}

int hr_tempfc_parse(const char *data, size_t n)
{
    if (data == NULL) {
        return HR_TEMP_DRYER_UNKNOWN;
    }
    /* trim whitespace, NULs and the dryer's line ends at both ends */
    while (n > 0 && ((unsigned char)data[0] <= 0x20)) {
        data++;
        n--;
    }
    while (n > 0 && ((unsigned char)data[n - 1] <= 0x20)) {
        n--;
    }
    if (n == 0 || n > 64) {
        return HR_TEMP_DRYER_UNKNOWN;
    }
    /* an empty data-flash record echoes the dryer's transmit buffer */
    if (n >= 11 && memcmp(data, "FDFILEBLOCK", 11) == 0) {
        return HR_TEMP_DRYER_UNKNOWN;
    }
    const bool c = has_word_ci(data, n, "celsius");
    const bool f = has_word_ci(data, n, "fahrenheit");
    if (c && !f) {
        return HR_TEMP_C;
    }
    if (f && !c) {
        return HR_TEMP_F;
    }
    if (c && f) {
        return HR_TEMP_DRYER_UNKNOWN; /* both words: not the record we know */
    }
    /* No word: fall back to the flag, "<0|1>," - 0 was Celsius live. */
    if (n >= 2 && data[1] == ',' && (data[0] == '0' || data[0] == '1')) {
        return data[0] == '0' ? HR_TEMP_C : HR_TEMP_F;
    }
    return HR_TEMP_DRYER_UNKNOWN;
}
