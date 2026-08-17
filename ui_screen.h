/*
 * ui_screen.h - what the RS-T108 panel shows in the field.
 *
 * The point of the screen is to make the tester usable while walking or
 * driving, without a phone: signal, modulation, latency and loss at a glance.
 *
 * Entirely optional. If no panel answers at boot the node behaves exactly as
 * before, so the same firmware image still serves both boards.
 */
#pragma once

#include <Arduino.h>

void uiInit(void);   /* detects the panel; safe to call when none is fitted */
void uiTick(void);   /* redraws about once a second */
bool uiPresent(void);
