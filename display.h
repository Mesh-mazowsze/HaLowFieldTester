/*
 * display.h - Heltec T108 (RS-T108) TFT on the RadioCore header.
 *
 * Panel: NV3001B, 128x220 portrait, 4-wire SPI (write only, no MISO), 4 MHz,
 * rotation 0. TFT_EN is active LOW (panel power), TFT_BL active HIGH.
 *
 * Pin mapping is the ESP32-S3 RadioCore one. The widely published table is for
 * the RadioCore *C6* and must not be used here - two of its pins (its GPIO 1
 * and 3) are the MM6108 BUSY line and the HaLow LDO enable on this board, so
 * driving them would cut power to the radio.
 *
 * Source for the register block and the ID read: the dependency-free RCC6 test
 * sketch and the Arduino_NV3001B class in the Quency-D/Arduino_GFX fork.
 * No external library is pulled in - this is a self-contained driver.
 */
#pragma once

#include <Arduino.h>

/* ESP32-S3 RadioCore mapping. Override at build time if a variant differs. */
#ifndef TFT_PIN_SCL
#define TFT_PIN_SCL 17
#endif
#ifndef TFT_PIN_SDA
#define TFT_PIN_SDA 38
#endif
#ifndef TFT_PIN_CS
#define TFT_PIN_CS  39
#endif
#ifndef TFT_PIN_DC
#define TFT_PIN_DC  16
#endif
#ifndef TFT_PIN_RST
#define TFT_PIN_RST 4
#endif
#ifndef TFT_PIN_EN
#define TFT_PIN_EN  6
#endif
#ifndef TFT_PIN_BL
#define TFT_PIN_BL  5
#endif

#define TFT_W 128
#define TFT_H 220

/* RGB565 */
#define TFT_BLACK   0x0000
#define TFT_WHITE   0xFFFF
#define TFT_RED     0xF800
#define TFT_GREEN   0x07E0
#define TFT_BLUE    0x001F
#define TFT_YELLOW  0xFFE0
#define TFT_CYAN    0x07FF
#define TFT_GREY    0x8410

/*
 * Bit-bangs the RDDID / RDID1..3 registers before any SPI setup.
 * Returns the 24-bit id; 0x300101 identifies an NV3001B.
 * This is how the pin mapping is verified without looking at the panel.
 */
uint32_t dispProbeId(void);

bool dispInit(void);          /* power, reset, SPI, full init sequence */
bool dispPresent(void);       /* true once dispInit() has succeeded */

void dispBacklight(bool on);
void dispFill(uint16_t colour);
void dispFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t colour);

/* 5x7 font, `scale` pixels per font pixel. */
void dispText(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);
