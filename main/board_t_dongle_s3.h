/*
 * LilyGo T-Dongle-S3 pin map.
 *
 * Every GPIO the board wires to a peripheral, in one place. Taken from the
 * vendor schematic (T-Dongle-S3-QWIIC V1.5) and cross-checked against the
 * vendor examples and the ESPHome / Tasmota / TFT_eSPI configurations for
 * this board - all sources agree on every number below. Same pins on the
 * 2022, 2023 "QWIIC" and 2026 "Plus" revisions.
 *
 * Not listed, deliberately: GPIO19/20 are the USB D-/D+ pins of the internal
 * PHY that TinyUSB owns - never configure them as GPIO. GPIO12/14/16/17/18/21
 * go to the microSD slot, which this firmware does not use.
 */
#ifndef BOARD_T_DONGLE_S3_H
#define BOARD_T_DONGLE_S3_H

/* --- ST7735S 0.96" 80x160 LCD, 4-wire SPI, write-only (no MISO). ------ */
#define HR_PIN_LCD_MOSI   3   /* also a strapping pin (JTAG source), harmless
                                 unless the JTAG_SEL_ENABLE eFuse is burnt */
#define HR_PIN_LCD_SCLK   5
#define HR_PIN_LCD_CS     4
#define HR_PIN_LCD_DC     2
#define HR_PIN_LCD_RST    1
/*
 * Backlight is ACTIVE LOW: GPIO38 drives the gate of a P-MOSFET whose source
 * sits on 3.3 V, so level 0 = backlight on, level 1 = off, and a LEDC duty
 * of 0 is full brightness. The pin floats at reset, so the first thing the
 * firmware does with it is drive it HIGH to stop the power-on flash.
 */
#define HR_PIN_LCD_BL     38

#define HR_LCD_WIDTH      160 /* landscape, USB plug on the left */
#define HR_LCD_HEIGHT     80
/*
 * Vendor examples clock the panel at 40 MHz. The ST7735S datasheet's minimum
 * write cycle is 66 ns (~15 MHz); 40 MHz works on this glass but is an
 * overclock, and the signals go through the GPIO matrix rather than IO_MUX.
 * 26.67 MHz (80 MHz / 3) is what TFT_eSPI uses for the board and leaves
 * margin. A full frame is 25.6 KB, about 8 ms at this rate.
 */
#define HR_LCD_SPI_HZ     (80 * 1000 * 1000 / 3)

/* --- APA102 RGB LED, bit-banged two-wire, colour bytes in B,G,R order. --- */
#define HR_PIN_LED_DATA   40
#define HR_PIN_LED_CLK    39

/*
 * --- Button (labelled BOOT / KEY1), to GND, 10 k pull-up, active LOW. ----
 *
 * GPIO0 is also the boot-mode strapping pin: held low at reset it puts the
 * chip in ROM download mode, which is how the board is flashed. The firmware
 * therefore ignores the pin for the first 100 ms after boot and never treats
 * a press as anything but a UI gesture.
 */
#define HR_PIN_BUTTON     0

/* --- UART0, on the QWIIC JST-SH connector (2023+ revisions only). ------ */
#define HR_PIN_UART0_TX   43
#define HR_PIN_UART0_RX   44

#endif /* BOARD_T_DONGLE_S3_H */
