/*
 * MorseLink — LoRa radio driver (T-Beam V1.1 / V1.2), SX1276 or SX1262.
 *
 * radio_init() finds out which chip the board has; the rest of the firmware
 * never needs to know. The SX1276 is driven in radio.c, the SX1262 in
 * radio_sx1262.c. The description below is the SX1276's; the SX1262 does the
 * same jobs with DIO1 (GPIO33) as its interrupt and a BUSY pin (GPIO32).
 *
 * A deliberately small driver: the radio sits in continuous receive, and
 * leaves it only to transmit one packet at a time. Everything about what to
 * send and when lives in the link engine (link.c); net.c moves bytes
 * between the two.
 *
 * Settings (config.h, spec §6): 869.525 MHz, 125 kHz bandwidth, SF9 by
 * default (SF9-12 as a setting, radio_set_sf), coding rate 4/5, explicit
 * header, CRC on, private sync word; up to 22 dBm on the SX1262, 20 dBm on
 * the SX1276's PA_BOOST output.
 *
 * Wiring (board.h): SPI on SCK 5 / MISO 19 / MOSI 27 / CS 18, reset on 23,
 * DIO0 on 26. DIO0 rises when a packet has been sent ("TxDone") or received
 * ("RxDone"); its interrupt only wakes the caller's task, which then calls
 * radio_service() to find out which. DIO1/DIO2 (GPIO33/32) are unused for
 * now; Phase 2 channel-activity detection would use DIO1.
 *
 * Power: the radio runs from a PMIC rail (AXP192 LDO2 on V1.1, AXP2101
 * ALDO2 on V1.2) that pmic_init() switches on, so call pmic_init() first.
 * Without it the chip reads back garbage (e.g. version 0xAA).
 *
 * Not thread-safe: call everything from one task (net.c's).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    uint32_t freq_hz;     /* carrier frequency */
    uint32_t bw_hz;       /* 7800 .. 500000; 125000 here */
    uint8_t sf;           /* spreading factor 6..12 */
    uint8_t cr;           /* coding rate 4/(4+cr), 1..4 */
    uint8_t sync_word;    /* private network ID byte */
    int8_t tx_dbm;        /* RADIO_TX_DBM_MIN .. RADIO_TX_DBM_MAX */
    uint16_t rx_preamble; /* preamble length the receiver expects (the longest any sender uses) */
} radio_cfg_t;

/* What radio_service() found. */
typedef enum {
    RADIO_EV_NONE = 0, /* nothing pending (a spurious wake-up) */
    RADIO_EV_TX_DONE,  /* our packet has left; the radio is listening again */
    RADIO_EV_RX,       /* a packet arrived: it is in the caller's buffer */
    RADIO_EV_RX_BAD,   /* a packet arrived damaged (CRC error) and was dropped */
} radio_ev_t;

typedef struct {
    int len;        /* bytes received */
    int16_t rssi;   /* signal strength of the packet, dBm */
    int8_t snr_x4;  /* signal-to-noise ratio, quarter dB (negative: below the noise) */
} radio_rx_t;

/* Counters for the console. */
typedef struct {
    uint32_t tx, rx, rx_bad, irqs, resets;
} radio_stats_t;

/*
 * Reset the chip, find out whether it is an SX1276 or an SX1262, configure
 * it and start receiving. `task` is woken with xTaskNotify(task, `bit`,
 * eSetBits) whenever the chip's interrupt pin rises. Returns ESP_ERR_NOT_FOUND if no radio answers (e.g. in the
 * emulator); everything else in this file is then a harmless no-op.
 */
esp_err_t radio_init(const radio_cfg_t *cfg, TaskHandle_t task, uint32_t bit);

bool radio_present(void);
uint8_t radio_chip_version(void); /* 0x12 for an SX1276 */
const char *radio_chip_name(void); /* "SX1276", "SX1262" or "none" */
const char *radio_problem(void);   /* why there is no radio; "" if there is one */

/* Change the transmit power (dBm, clamped to this chip's range:
 * RADIO_TX_DBM_MIN .. radio_tx_dbm_max()). */
esp_err_t radio_set_tx_power(int dbm);
int radio_tx_power(void);

/* The most this chip can transmit: 22 dBm (SX1262), 20 dBm (SX1276);
 * RADIO_TX_DBM_MAX before radio_init() or without a radio. */
int radio_tx_dbm_max(void);

/* Change the spreading factor (RADIO_SF_MIN .. RADIO_SF_MAX), with the
 * low-data-rate optimisation that goes with it. Takes effect at once:
 * receiving pauses for a moment. ESP_ERR_INVALID_STATE while transmitting
 * (try again after TX_DONE) or without a radio. */
esp_err_t radio_set_sf(int sf);
int radio_sf(void);

/* Load a packet and start sending it with `preamble` symbols. Returns at
 * once; DIO0 signals the end (radio_service() then says RADIO_EV_TX_DONE). */
esp_err_t radio_transmit(const uint8_t *buf, int len, uint16_t preamble);

/* True while transmitting (between radio_transmit and its TX_DONE). */
bool radio_transmitting(void);

/* Read and clear the chip's interrupt flags and act on them. A received
 * packet is copied into buf (up to maxlen bytes) and described in *rx. */
radio_ev_t radio_service(uint8_t *buf, int maxlen, radio_rx_t *rx);

/* Is DIO0 high right now? (An event is waiting: covers a missed edge.) */
bool radio_irq_pending(void);

/* Is someone else's packet on the air right now (preamble or header
 * detected)? Used to hold back our own transmission rather than collide. */
bool radio_channel_busy(void);

/* Background noise on the channel right now, dBm (a range-test figure). */
int radio_noise_dbm(void);

/* Give up on a transmission that never finished (no TX_DONE): reconfigure
 * and listen again. */
void radio_recover(void);

/* Health check, every few seconds while idle: if the chip has left
 * continuous receive (a brown-out resets it to FSK standby), set it up
 * again. Returns true if it had to. */
bool radio_check(void);

void radio_get_stats(radio_stats_t *st);

/* Bench power (TEST_PLAN §14.1): stop receiving (standby: the radio's
 * smallest awake current), or send a plain carrier at `dbm` (the transmit
 * current), until radio_bench_listen() goes back to receiving at the usual
 * power. Not while a packet is going out (ESP_ERR_INVALID_STATE). */
esp_err_t radio_bench_standby(void);
esp_err_t radio_bench_carrier(int dbm);
esp_err_t radio_bench_listen(void);
