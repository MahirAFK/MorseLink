/*
 * MorseLink — serial console (115200 baud, the same port as the logs).
 * Type "help" for the command list.
 */
#pragma once

#include "esp_err.h"

esp_err_t console_start(void);
