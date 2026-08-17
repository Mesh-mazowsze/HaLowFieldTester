/*
 * serial_cli.h - line-based console on the USB serial port.
 *
 * A fallback for configuring and inspecting a node when you cannot reach the
 * web panel (and the way the two-node bring-up is scripted during testing).
 * Type "help" for the command list.
 */
#pragma once

#include <Arduino.h>

void cliInit(void);
void cliTick(void);
