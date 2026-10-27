/*
 * MorseLink — SX1262 back end of the radio driver (private to radio.c).
 *
 * Some T-Beams (common on V1.2) carry an SX1262 instead of an SX1276. The
 * two chips speak the same LoRa on the air but are driven completely
 * differently: the SX1276 has registers, the SX1262 takes commands and has a
 * BUSY pin that must be low before each one. radio.c finds out which chip is
 * fitted and forwards every radio_*() call here when it is an SX1262; the
 * functions below mirror radio.h one for one, so see there for what each
 * does. Nothing outside radio.c should include this file.
 *
 * Wiring on the SX1262 T-Beam (board.h): same SPI pins, CS 18 and reset 23
 * as the SX1276 version; DIO1 on GPIO33 (interrupt: TxDone / RxDone), BUSY on
 * GPIO32. DIO2 switches the antenna between transmit and receive; DIO3
 * powers the module's temperature-compensated crystal (TCXO) at 1.8 V.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "radio.h"

/* sx1262_probe() -- reset the chip and check an SX1262 answers on SPI2_HOST
 * (the bus is already initialised by radio.c). Adds its own SPI device.
 * Returns true if found; on false the device is removed again. */
bool sx1262_probe(void);

/* sx1262_init() -- configure (cfg), hook DIO1 to wake `task` with `bit`,
 * start receiving. `stats` is radio.c's counter block, updated in place.
 * On failure, a one-line reason goes into problem (problem_len bytes). */
esp_err_t sx1262_init(const radio_cfg_t *cfg, TaskHandle_t task, uint32_t bit, radio_stats_t *stats, char *problem,
                      size_t problem_len);

esp_err_t sx1262_set_tx_power(int dbm);
int sx1262_tx_power(void);
esp_err_t sx1262_set_sf(int sf);
esp_err_t sx1262_bench_standby(void);
esp_err_t sx1262_bench_carrier(int dbm);
esp_err_t sx1262_bench_listen(void);
esp_err_t sx1262_transmit(const uint8_t *buf, int len, uint16_t preamble);
bool sx1262_transmitting(void);
radio_ev_t sx1262_service(uint8_t *buf, int maxlen, radio_rx_t *rx);
bool sx1262_irq_pending(void);
bool sx1262_channel_busy(void);
int sx1262_noise_dbm(void);
void sx1262_recover(void);
bool sx1262_check(void);
