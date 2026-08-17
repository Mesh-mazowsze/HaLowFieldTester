/*
 * web_assets.h - the panel's HTML/CSS/JS, stored in PROGMEM.
 *
 * Kept in flash rather than in a SPIFFS/LittleFS image so that flashing the
 * firmware is a single step - no separate filesystem upload is needed.
 */
#pragma once

#include <Arduino.h>

extern const char INDEX_HTML[] PROGMEM;
extern const char APP_CSS[]    PROGMEM;
extern const char APP_JS[]     PROGMEM;
