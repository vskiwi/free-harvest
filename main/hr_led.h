/*
 * APA102 RGB LED on the T-Dongle-S3, driven from the hr_ui task.
 *
 * Output only. Brightness is capped at HR_UI_LED_MAX_PCT (30 %) in hardware
 * terms here as well as in the UI table: the LED sits in a closed plastic
 * stick next to the Wi-Fi antenna, and at full white it draws ~60 mA.
 */
#ifndef HR_LED_H
#define HR_LED_H

#include <stdint.h>

void hr_led_init(void);

/* Colour at full scale plus a 0..100 brightness (clamped to the cap). */
void hr_led_set(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness_pct);

#endif /* HR_LED_H */
