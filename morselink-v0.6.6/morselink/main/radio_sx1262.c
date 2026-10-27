/*
 * MorseLink — SX1262 back end of the radio driver (see radio_sx1262.h).
 *
 * Command numbers, register addresses and bit meanings are from the Semtech
 * SX1261/2 datasheet (DS.SX1261-2, rev. 2.1): chapter 13 "Commands
 * Interface" and chapter 15 "Known Limitations" (the errata workarounds
 * marked "errata 15.x" below).
 *
 * How the SX1262 is driven, in short:
 *   - Every SPI exchange is one command: an opcode byte, then parameters.
 *     "Get" commands clock out a status byte and then the answer.
 *   - The chip raises BUSY while it works on a command; the next command
 *     may only start once BUSY is low again (wait_busy()).
 *   - Received and outgoing packets live in a 256-byte data buffer, reached
 *     with WriteBuffer / ReadBuffer. Each SPI transaction is kept to 64
 *     bytes (no DMA), so packets move in a few pieces.
 *   - Interrupt flags are set by the chip and routed to DIO1, which wakes
 *     the network task. The task then reads the flags (GetIrqStatus).
 *
 * The behaviour matches the SX1276 back end exactly, so the rest of the
 * firmware can't tell the two apart: continuous receive, one packet sent at
 * a time, the same LoRa settings, and the same sync word as seen on the air.
 */
#include "radio_sx1262.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "proto.h"

static const char *TAG = "radio";

/* ---- Commands (datasheet §13) ------------------------------------------- */
#define CMD_SET_STANDBY 0x80
#define CMD_SET_PACKET_TYPE 0x8A
#define CMD_SET_RF_FREQUENCY 0x86
#define CMD_SET_PA_CONFIG 0x95
#define CMD_SET_TX_PARAMS 0x8E
#define CMD_SET_MODULATION_PARAMS 0x8B
#define CMD_SET_PACKET_PARAMS 0x8C
#define CMD_SET_BUFFER_BASE 0x8F
#define CMD_WRITE_BUFFER 0x0E
#define CMD_READ_BUFFER 0x1E
#define CMD_WRITE_REGISTER 0x0D
#define CMD_READ_REGISTER 0x1D
#define CMD_SET_DIO_IRQ_PARAMS 0x08
#define CMD_GET_IRQ_STATUS 0x12
#define CMD_CLEAR_IRQ_STATUS 0x02
#define CMD_SET_RX 0x82
#define CMD_SET_TX 0x83
#define CMD_GET_RX_BUFFER_STATUS 0x13
#define CMD_GET_PACKET_STATUS 0x14
#define CMD_GET_RSSI_INST 0x15
#define CMD_GET_STATUS 0xC0
#define CMD_GET_DEVICE_ERRORS 0x17
#define CMD_CLEAR_DEVICE_ERRORS 0x07
#define CMD_SET_DIO3_AS_TCXO 0x97
#define CMD_SET_DIO2_AS_RF_SWITCH 0x9D
#define CMD_CALIBRATE 0x89
#define CMD_CALIBRATE_IMAGE 0x98
#define CMD_SET_REGULATOR_MODE 0x96
#define CMD_SET_TX_CONTINUOUS_WAVE 0xD1 /* bench power: a plain carrier until standby */

/* ---- Registers ------------------------------------------------------------ */
#define REG_VERSION_STRING 0x0320 /* "SX1261 V2D 2D02"-style text */
#define REG_SYNC_WORD_MSB 0x0740
#define REG_SYNC_WORD_LSB 0x0741
#define REG_IQ_POLARITY 0x0736    /* errata 15.4 */
#define REG_TX_MODULATION 0x0889  /* errata 15.1 */
#define REG_RX_GAIN 0x08AC        /* 0x96 = boosted gain (better sensitivity, ~0.7 mA more) */
#define REG_TX_CLAMP 0x08D8       /* errata 15.2 */
#define REG_OCP 0x08E7            /* PA current limit, 2.5 mA steps */

/* ---- Interrupt flags (16 bits) ------------------------------------------- */
#define IRQ_TX_DONE 0x0001
#define IRQ_RX_DONE 0x0002
#define IRQ_PREAMBLE 0x0004     /* a preamble is being received */
#define IRQ_HEADER_VALID 0x0010 /* a valid LoRa header arrived */
#define IRQ_HEADER_ERR 0x0020
#define IRQ_CRC_ERR 0x0040
#define IRQ_TIMEOUT 0x0200
#define IRQ_ALL 0x03FF

/* Flags the chip records, and the subset that raises DIO1. Preamble and
 * header-valid are only recorded: radio_channel_busy() reads them. */
#define IRQ_ENABLED (IRQ_TX_DONE | IRQ_RX_DONE | IRQ_PREAMBLE | IRQ_HEADER_VALID | IRQ_HEADER_ERR | IRQ_CRC_ERR | IRQ_TIMEOUT)
#define IRQ_ON_DIO1 (IRQ_TX_DONE | IRQ_RX_DONE | IRQ_HEADER_ERR | IRQ_CRC_ERR | IRQ_TIMEOUT)

/* GetStatus: bits 6-4 are the chip mode. */
#define MODE_STBY_RC 2
#define MODE_RX 5

/* GetDeviceErrors bits (datasheet table 13-85). They stay set until cleared. */
#define CALIB_ERRS 0x001F     /* bits 0-4: a calibration (RC, PLL, ADC, image) failed */
#define XOSC_START_ERR 0x0020 /* the crystal / TCXO didn't start */
#define PLL_LOCK_ERR 0x0040   /* the synthesiser couldn't lock (no clock, usually) */
#define TCXO_1V8 0x02         /* SetDIO3AsTCXOCtrl voltage code */
#define TCXO_WAIT 320         /* how long the chip waits for the TCXO, 15.625 us steps: 5 ms */

#define FXTAL_HZ 32000000ull
#define SPI_HZ (8 * 1000 * 1000)
#define SPI_CHUNK 64          /* longest transaction without DMA */
#define BUSY_TIMEOUT_US 100000

/* ---- state ---------------------------------------------------------------- */
static spi_device_handle_t s_spi;
static radio_cfg_t s_cfg;
static bool s_txing;
static bool s_bench_cw;      /* bench power: the carrier is on */
static int s_bench_keep_dbm; /* ... and the power to go back to afterwards */
static bool s_tcxo = true;      /* the clock is a TCXO powered from DIO3 (false: a plain crystal) */
static TaskHandle_t s_task;     /* woken by the DIO1 interrupt */
static uint32_t s_bit;
static radio_stats_t *s_stats;  /* radio.c's counters */
static int64_t s_busy_since;    /* when radio_channel_busy() first saw a preamble (0: not now) */
static uint32_t s_busy_max_us;  /* longer than any packet: a header flag older than this is stale */
static uint32_t s_pre_max_us;   /* longer than preamble + header: a preamble flag with no header after this is stale */

/* ========================================================================= */
/* SPI and commands                                                           */
/* ========================================================================= */

/* wait_busy() -- wait until the chip is ready for the next command (BUSY
 * low). Returns false after 100 ms (the chip is stuck or absent). */
static bool wait_busy(void)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(PIN_LORA_BUSY)) {
        if (esp_timer_get_time() - start > BUSY_TIMEOUT_US) {
            return false;
        }
        esp_rom_delay_us(10);
    }
    return true;
}

/* xfer() -- one full-duplex SPI transaction of n (<= 64) bytes, after
 * waiting for BUSY. rx may be NULL. Returns false if BUSY never cleared. */
static bool xfer(const uint8_t *tx, uint8_t *rx, int n)
{
    bool ok = wait_busy();
    spi_transaction_t t = {
        .length = (size_t)n * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_polling_transmit(s_spi, &t);
    return ok;
}

/* cmd() -- send opcode `op` with n parameter bytes. */
static void cmd(uint8_t op, const uint8_t *p, int n)
{
    uint8_t b[16] = {op};
    memcpy(b + 1, p, (size_t)n);
    xfer(b, NULL, 1 + n);
}

/* cmd_get() -- a "Get" command: send op, clock out n answer bytes into
 * out (skipping the status byte that comes first). Returns the status. */
static uint8_t cmd_get(uint8_t op, uint8_t *out, int n)
{
    uint8_t tx[8] = {op}, rx[8] = {0};
    xfer(tx, rx, 2 + n);
    memcpy(out, rx + 2, (size_t)n);
    return rx[1];
}

/* write_reg() / read_reg() -- one register byte at a 16-bit address. */
static void write_reg(uint16_t addr, uint8_t val)
{
    uint8_t p[3] = {(uint8_t)(addr >> 8), (uint8_t)addr, val};
    cmd(CMD_WRITE_REGISTER, p, 3);
}

/* read_reg() -- ReadRegister: the answer comes after the opcode, the two
 * address bytes and one status byte. */
static uint8_t read_reg(uint16_t addr)
{
    uint8_t tx[5] = {CMD_READ_REGISTER, (uint8_t)(addr >> 8), (uint8_t)addr, 0, 0}, rx[5] = {0};
    xfer(tx, rx, 5);
    return rx[4]; /* after opcode, address and one status byte */
}

/* write_buf() -- copy n bytes into the data buffer at offset 0, in pieces
 * of up to 62 bytes (2 command bytes + data per transaction). */
static void write_buf(const uint8_t *data, int n)
{
    uint8_t b[SPI_CHUNK];
    int off = 0;
    while (off < n) {
        int k = n - off > SPI_CHUNK - 2 ? SPI_CHUNK - 2 : n - off;
        b[0] = CMD_WRITE_BUFFER;
        b[1] = (uint8_t)off;
        memcpy(b + 2, data + off, (size_t)k);
        xfer(b, NULL, 2 + k);
        off += k;
    }
}

/* read_buf() -- copy n bytes out of the data buffer from `start`, in
 * pieces of up to 61 bytes (opcode, offset, status, then data). */
static void read_buf(uint8_t start, uint8_t *data, int n)
{
    uint8_t tx[SPI_CHUNK] = {0}, rx[SPI_CHUNK];
    int off = 0;
    while (off < n) {
        int k = n - off > SPI_CHUNK - 3 ? SPI_CHUNK - 3 : n - off;
        tx[0] = CMD_READ_BUFFER;
        tx[1] = (uint8_t)(start + off); /* the buffer wraps at 256, like the uint8_t */
        xfer(tx, rx, 3 + k);
        memcpy(data + off, rx + 3, (size_t)k);
        off += k;
    }
}

/* irq_get() / irq_clear() -- the 16 interrupt flags. */
static uint16_t irq_get(void)
{
    uint8_t r[2];
    cmd_get(CMD_GET_IRQ_STATUS, r, 2);
    return (uint16_t)((r[0] << 8) | r[1]);
}

/* irq_clear() -- ClearIrqStatus for the flags in `mask`. */
static void irq_clear(uint16_t mask)
{
    uint8_t p[2] = {(uint8_t)(mask >> 8), (uint8_t)mask};
    cmd(CMD_CLEAR_IRQ_STATUS, p, 2);
}

/* chip_mode() -- 2 = standby (RC), 3 = standby (crystal), 5 = RX, 6 = TX. */
static int chip_mode(void)
{
    uint8_t tx[2] = {CMD_GET_STATUS, 0}, rx[2] = {0};
    xfer(tx, rx, 2);
    return (rx[1] >> 4) & 0x07;
}

/* standby() -- SetStandby on the internal RC clock: the state every
 * configuration command expects. */
static void standby(void)
{
    uint8_t p = 0x00; /* STDBY_RC: the internal RC clock, always available */
    cmd(CMD_SET_STANDBY, &p, 1);
}

/* reset() -- pulse NRESET (the pin is set up open-drain by radio.c; datasheet
 * §8.1: low for > 100 us), then wait for the chip to finish starting. */
static void reset(void)
{
    gpio_set_level(PIN_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    wait_busy();
}

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

/* bw_code() -- SetModulationParams bandwidth code for bw_hz (nearest not
 * above). The SX1262's codes are not in frequency order. */
static uint8_t bw_code(uint32_t bw_hz)
{
    static const struct {
        uint32_t hz;
        uint8_t code;
    } t[] = {{7800, 0x00},  {10400, 0x08}, {15600, 0x01},  {20800, 0x09},  {31250, 0x02},
             {41700, 0x0A}, {62500, 0x03}, {125000, 0x04}, {250000, 0x05}, {500000, 0x06}};
    uint8_t code = 0x00;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (bw_hz >= t[i].hz) {
            code = t[i].code;
        }
    }
    return code;
}

/* set_packet_params() -- preamble length, explicit header, maximum or
 * actual payload length, CRC on, standard IQ. Then errata 15.4: with
 * standard IQ, bit 2 of register 0x0736 must be 1, or reception suffers. */
static void set_packet_params(uint16_t preamble, uint8_t len)
{
    uint8_t p[6] = {(uint8_t)(preamble >> 8), (uint8_t)preamble, 0x00 /* explicit header */, len,
                    0x01 /* CRC on */, 0x00 /* standard IQ */};
    cmd(CMD_SET_PACKET_PARAMS, p, 6);
    write_reg(REG_IQ_POLARITY, (uint8_t)(read_reg(REG_IQ_POLARITY) | 0x04));
}

/*
 * apply_tx_power() -- the SX1262's high-power PA. The settings 0x04 / 0x07
 * are the datasheet's table 13-21 values for its full +22 dBm range; the
 * power is then given directly in dBm. Current limit 140 mA (0x38). Ramp
 * time 200 us.
 */
static void apply_tx_power(int dbm)
{
    if (dbm < RADIO_TX_DBM_MIN) {
        dbm = RADIO_TX_DBM_MIN;
    }
    if (dbm > RADIO_TX_DBM_MAX) {
        dbm = RADIO_TX_DBM_MAX;
    }
    uint8_t pa[4] = {0x04 /* duty cycle */, 0x07 /* hpMax */, 0x00 /* SX1262 */, 0x01};
    cmd(CMD_SET_PA_CONFIG, pa, 4);
    write_reg(REG_OCP, 0x38);
    uint8_t tp[2] = {(uint8_t)(int8_t)dbm, 0x04 /* 200 us ramp */};
    cmd(CMD_SET_TX_PARAMS, tp, 2);
    s_cfg.tx_dbm = (int8_t)dbm;
}

/* device_errors() -- read the chip's error flags, then clear them (they
 * are sticky). Returns the flags as they were. */
static uint16_t device_errors(void)
{
    uint8_t e[2];
    cmd_get(CMD_GET_DEVICE_ERRORS, e, 2);
    uint8_t zero[2] = {0, 0};
    cmd(CMD_CLEAR_DEVICE_ERRORS, zero, 2);
    return (uint16_t)((e[0] << 8) | e[1]);
}

/*
 * start_clock() -- tell the chip how its reference clock is powered (s_tcxo),
 * then calibrate everything. With a TCXO (the T-Beam module's usual fitting),
 * DIO3 must power it at 1.8 V before calibration, or calibration fails.
 *
 * The error flags are cleared first. That matters: on a board with a TCXO
 * the chip always sets "crystal didn't start" right after power-up or reset,
 * because it tries the clock before anyone has switched the TCXO's supply
 * on. v0.3.2 read that stale flag, wrongly concluded "no TCXO", turned the
 * TCXO supply off and so left the radio without a clock.
 *
 * Returns the error flags raised by this calibration (0 = all good).
 */
static uint16_t start_clock(void)
{
    standby();
    (void)device_errors(); /* drop the stale power-up flag (see above) */
    if (s_tcxo) {
        uint8_t p[4] = {TCXO_1V8, 0, (uint8_t)(TCXO_WAIT >> 8), (uint8_t)TCXO_WAIT};
        cmd(CMD_SET_DIO3_AS_TCXO, p, 4);
    }
    uint8_t all = 0x7F; /* every block: RC oscillators, PLL, ADC, image */
    cmd(CMD_CALIBRATE, &all, 1);
    wait_busy();
    return device_errors();
}

/*
 * set_busy_limits() -- the limits radio_channel_busy() uses, for the
 * current SF: a valid header can't stay unfinished for longer than the
 * longest packet we use; a preamble can't go without a header for longer
 * than the preamble, the 4.25 sync symbols and the 8 header symbols (13
 * with rounding up).
 */
static void set_busy_limits(void)
{
    lora_params_t lp = {s_cfg.sf, s_cfg.bw_hz, s_cfg.cr};
    s_busy_max_us = lora_airtime_us(PROTO_HDR_LEN + PROTO_MAX_PAYLOAD, &lp, RADIO_PREAMBLE_LONG) + 100000;
    uint32_t sym_us = (uint32_t)(((uint64_t)1000000 << s_cfg.sf) / s_cfg.bw_hz); /* 4096 us at SF9 / 125 kHz */
    s_pre_max_us = sym_us * (uint32_t)(s_cfg.rx_preamble + 13) + 20000;
}

/*
 * apply_modulation() -- SF, bandwidth, coding rate (same 1..4 numbering as
 * ours) and the low-data-rate optimisation, by the same rule as the SX1276
 * and proto.c (in standby).
 */
static void apply_modulation(void)
{
    lora_params_t lp = {s_cfg.sf, s_cfg.bw_hz, s_cfg.cr};
    uint8_t mod[4] = {s_cfg.sf, bw_code(s_cfg.bw_hz), s_cfg.cr, lora_needs_ldro(&lp) ? 1 : 0};
    cmd(CMD_SET_MODULATION_PARAMS, mod, 4);
}

/*
 * configure() -- everything from a freshly reset chip to standby with our
 * settings, using the clock type in s_tcxo. Order as the datasheet's §14
 * "Circuit Configuration" steps. Returns the error flags the chip raised on
 * the way (0 = all good).
 */
static uint16_t configure(void)
{
    uint16_t errs = start_clock();
    standby();
    uint8_t dcdc = 0x01; /* DC-DC regulator: about half the receive current of the LDO */
    cmd(CMD_SET_REGULATOR_MODE, &dcdc, 1);
    uint8_t one = 0x01;
    cmd(CMD_SET_DIO2_AS_RF_SWITCH, &one, 1); /* DIO2 flips the antenna switch to TX while sending */
    uint8_t lora = 0x01;
    cmd(CMD_SET_PACKET_TYPE, &lora, 1);

    /* Carrier: RfFreq = f * 2^25 / 32 MHz (0.95 Hz steps). */
    uint32_t frf = (uint32_t)(((uint64_t)s_cfg.freq_hz << 25) / FXTAL_HZ);
    uint8_t f[4] = {(uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf};
    cmd(CMD_SET_RF_FREQUENCY, f, 4);
    /* Image calibration for the 863-870 MHz band (datasheet table 9-2). */
    uint8_t img[2] = {0xD7, 0xDB};
    cmd(CMD_CALIBRATE_IMAGE, img, 2);
    wait_busy();
    errs |= device_errors();

    uint8_t base[2] = {0, 0}; /* TX and RX both use the buffer from offset 0 */
    cmd(CMD_SET_BUFFER_BASE, base, 2);

    apply_modulation();
    /* Errata 15.1: below 500 kHz, bit 2 of 0x0889 must be set (at 500 kHz
     * cleared), or the transmitted signal is degraded. */
    uint8_t txm = read_reg(REG_TX_MODULATION);
    write_reg(REG_TX_MODULATION, s_cfg.bw_hz >= 500000 ? (uint8_t)(txm & ~0x04) : (uint8_t)(txm | 0x04));

    set_packet_params(s_cfg.rx_preamble, 0xFF);

    /* Sync word: the SX1276's one byte 0xXY is sent by the SX126x family as
     * the two bytes 0xX4, 0xY4. Writing it this way keeps an SX1262 board
     * and an SX1276 board able to hear each other. */
    write_reg(REG_SYNC_WORD_MSB, (uint8_t)((s_cfg.sync_word & 0xF0) | 0x04));
    write_reg(REG_SYNC_WORD_LSB, (uint8_t)(((s_cfg.sync_word & 0x0F) << 4) | 0x04));

    /* Errata 15.2: the PA's over-voltage clamp is too strict for some
     * antenna mismatches; Semtech's fix is to set bits 4-1 of 0x08D8. */
    write_reg(REG_TX_CLAMP, (uint8_t)(read_reg(REG_TX_CLAMP) | 0x1E));
    apply_tx_power(s_cfg.tx_dbm);
    write_reg(REG_RX_GAIN, 0x96); /* boosted receive gain */

    uint8_t irq[8] = {(uint8_t)(IRQ_ENABLED >> 8), (uint8_t)IRQ_ENABLED, (uint8_t)(IRQ_ON_DIO1 >> 8),
                      (uint8_t)IRQ_ON_DIO1, 0, 0, 0, 0}; /* nothing on DIO2 / DIO3: they have other jobs */
    cmd(CMD_SET_DIO_IRQ_PARAMS, irq, 8);
    irq_clear(IRQ_ALL);
    return errs;
}

/* start_rx() -- continuous receive (timeout 0xFFFFFF): the chip stays in RX
 * after each packet; DIO1 rises for each one. */
static void start_rx(void)
{
    irq_clear(IRQ_ALL);
    uint8_t t[3] = {0xFF, 0xFF, 0xFF};
    cmd(CMD_SET_RX, t, 3);
    s_busy_since = 0;
}

/* dio1_isr() -- DIO1 rose: a packet was sent or received (or a damaged one
 * arrived). Wakes the network task, which does the SPI work. */
static void IRAM_ATTR dio1_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    s_stats->irqs++;
    if (s_task) {
        xTaskNotifyFromISR(s_task, s_bit, eSetBits, &woken);
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

/* ========================================================================= */
/* Back-end API (called from radio.c)                                         */
/* ========================================================================= */

bool sx1262_probe(void)
{
    spi_device_interface_config_t dev = {
        .mode = 0, /* clock idle low, sample on the rising edge */
        .clock_speed_hz = SPI_HZ,
        .spics_io_num = PIN_LORA_CS,
        .queue_size = 1,
    };
    if (spi_bus_add_device(SPI2_HOST, &dev, &s_spi) != ESP_OK) {
        return false;
    }
    gpio_config_t busy = {
        .pin_bit_mask = 1ULL << PIN_LORA_BUSY,
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&busy);
    reset();
    /* Two independent signs of an SX126x: its version text starts "SX126",
     * and the LoRa sync word register holds its reset value 0x1424. */
    char ver[6] = {0};
    for (int i = 0; i < 5; i++) {
        ver[i] = (char)read_reg((uint16_t)(REG_VERSION_STRING + i));
    }
    bool found = strncmp(ver, "SX126", 5) == 0 ||
                 (read_reg(REG_SYNC_WORD_MSB) == 0x14 && read_reg(REG_SYNC_WORD_LSB) == 0x24);
    if (!found) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    return found;
}

esp_err_t sx1262_init(const radio_cfg_t *cfg, TaskHandle_t task, uint32_t bit, radio_stats_t *stats, char *problem,
                      size_t problem_len)
{
    s_cfg = *cfg;
    s_task = task;
    s_bit = bit;
    s_stats = stats;
    set_busy_limits(); /* for radio_channel_busy() */

    /* Which clock does this module have? LilyGO's SX1262 modules use a
     * TCXO powered from DIO3, so try that first; if the clock doesn't start
     * (or the chip won't go into receive), reset and try a plain crystal.
     * A failed calibration of a single block is reported but tolerated; no
     * clock, or no receive mode, is not. */
    uint16_t errs = 0;
    int mode = 0;
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        s_tcxo = (attempt == 0);
        if (attempt > 0) {
            reset();
        }
        errs = configure();
        start_rx();
        errs |= device_errors(); /* entering receive needs the synthesiser to lock: no clock shows up here */
        mode = chip_mode();
        ok = mode == MODE_RX && !(errs & (XOSC_START_ERR | PLL_LOCK_ERR));
        if (!ok) {
            ESP_LOGW(TAG, "SX1262 with %s: device errors 0x%04x, mode %d (5 = receiving)",
                     s_tcxo ? "TCXO on DIO3" : "crystal", errs, mode);
        }
    }
    if (!ok) {
        snprintf(problem, problem_len, "SX1262 found but would not start (device errors 0x%04x, mode %d)", errs,
                 mode);
        ESP_LOGE(TAG, "%s", problem);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (errs & CALIB_ERRS) {
        ESP_LOGW(TAG, "SX1262 calibration reported 0x%04x; carrying on", errs);
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_LORA_SX1262_DIO1,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io);
    /* ISR service: installed by app_main(). A packet that arrived before
     * this point still gets handled: net.c also polls the pin's level. */
    gpio_isr_handler_add(PIN_LORA_SX1262_DIO1, dio1_isr, NULL);

    ESP_LOGI(TAG, "SX1262 ready: %.3f MHz, SF%d, %lu kHz, CR 4/%d, sync 0x%02X, %d dBm, %s", s_cfg.freq_hz / 1e6,
             s_cfg.sf, (unsigned long)(s_cfg.bw_hz / 1000), 4 + s_cfg.cr, s_cfg.sync_word, s_cfg.tx_dbm,
             s_tcxo ? "TCXO" : "crystal");
    return ESP_OK;
}

/* sx1262_set_tx_power() -- PA settings are written in standby, so receiving
 * pauses for a moment. */
esp_err_t sx1262_set_tx_power(int dbm)
{
    if (s_txing) {
        return ESP_ERR_INVALID_STATE;
    }
    standby();
    apply_tx_power(dbm);
    start_rx();
    return ESP_OK;
}

/* sx1262_tx_power() -- the transmit power in use, dBm. */
int sx1262_tx_power(void)
{
    return s_cfg.tx_dbm;
}

/* sx1262_bench_standby() -- bench power step 1: out of receive, into
 * standby (the radio's smallest awake current). */
esp_err_t sx1262_bench_standby(void)
{
    if (s_txing) {
        return ESP_ERR_INVALID_STATE;
    }
    standby();
    return ESP_OK;
}

/* sx1262_bench_carrier() -- bench power step 4: SetTxContinuousWave at
 * `dbm` (DIO2 switches the antenna to transmit by itself). */
esp_err_t sx1262_bench_carrier(int dbm)
{
    if (s_txing) {
        return ESP_ERR_INVALID_STATE;
    }
    standby();
    if (!s_bench_cw) {
        s_bench_keep_dbm = s_cfg.tx_dbm;
    }
    apply_tx_power(dbm);
    uint8_t none = 0;
    cmd(CMD_SET_TX_CONTINUOUS_WAVE, &none, 0);
    s_bench_cw = true;
    return ESP_OK;
}

/* sx1262_bench_listen() -- carrier off (the usual power back), then receive. */
esp_err_t sx1262_bench_listen(void)
{
    standby();
    if (s_bench_cw) {
        apply_tx_power(s_bench_keep_dbm);
        s_bench_cw = false;
    }
    start_rx();
    return ESP_OK;
}

/* sx1262_set_sf() -- a new spreading factor: modulation (and the busy
 * limits that depend on the symbol time) in standby, then receive again.
 * Refused while a packet is going out. */
esp_err_t sx1262_set_sf(int sf)
{
    if (s_txing) {
        return ESP_ERR_INVALID_STATE;
    }
    standby();
    s_cfg.sf = (uint8_t)sf;
    apply_modulation();
    set_busy_limits();
    start_rx();
    ESP_LOGI(TAG, "SX1262: SF%d", sf);
    return ESP_OK;
}

/* sx1262_transmit() -- standby, packet into the buffer, this packet's
 * length and preamble, then TX with no timeout. When done the chip falls
 * back to standby by itself and raises TxDone. */
esp_err_t sx1262_transmit(const uint8_t *buf, int len, uint16_t preamble)
{
    if (len <= 0 || len > 255) {
        return ESP_ERR_INVALID_ARG;
    }
    standby();
    write_buf(buf, len);
    set_packet_params(preamble, (uint8_t)len);
    irq_clear(IRQ_ALL);
    s_txing = true;
    uint8_t t[3] = {0, 0, 0};
    cmd(CMD_SET_TX, t, 3);
    return ESP_OK;
}

/* sx1262_transmitting() -- between sx1262_transmit() and its TX_DONE. */
bool sx1262_transmitting(void)
{
    return s_txing;
}

/*
 * sx1262_service() -- read and clear the flags, then act on them.
 * Signal figures (GetPacketStatus): RSSI = -RssiPkt / 2 dBm; SnrPkt is
 * already in quarter dB, the unit radio_rx_t uses.
 */
radio_ev_t sx1262_service(uint8_t *buf, int maxlen, radio_rx_t *rx)
{
    uint16_t irq = irq_get();
    irq_clear(irq);
    if (s_txing && (irq & (IRQ_TX_DONE | IRQ_TIMEOUT))) {
        s_txing = false;
        s_stats->tx++;
        set_packet_params(s_cfg.rx_preamble, 0xFF); /* back to the receive setting */
        start_rx();
        return RADIO_EV_TX_DONE;
    }
    if (irq & (IRQ_CRC_ERR | IRQ_HEADER_ERR)) {
        s_stats->rx_bad++;
        return RADIO_EV_RX_BAD;
    }
    if (!(irq & IRQ_RX_DONE)) {
        return RADIO_EV_NONE;
    }
    uint8_t st[2];
    cmd_get(CMD_GET_RX_BUFFER_STATUS, st, 2); /* length, start offset */
    int n = st[0] > maxlen ? maxlen : st[0];
    read_buf(st[1], buf, n);
    uint8_t ps[3];
    cmd_get(CMD_GET_PACKET_STATUS, ps, 3);
    rx->len = n;
    rx->rssi = (int16_t)(-(int)ps[0] / 2);
    rx->snr_x4 = (int8_t)ps[1];
    s_stats->rx++;
    return RADIO_EV_RX;
}

/* sx1262_irq_pending() -- DIO1 still high: an event is waiting (covers an
 * edge the interrupt missed). */
bool sx1262_irq_pending(void)
{
    return gpio_get_level(PIN_LORA_SX1262_DIO1) == 1;
}

/*
 * sx1262_channel_busy() -- the chip has seen a preamble or a valid header
 * that hasn't finished as a packet yet. Those flags stay set until cleared,
 * and noise sometimes looks like a preamble with nothing following, so:
 *   - a preamble with no valid header after about 100 ms (s_pre_max_us)
 *     was a false alarm;
 *   - a valid header that never finished within the longest packet time
 *     (s_busy_max_us) is stale too.
 * Either is cleared and the channel counts as free. The time is measured
 * from when this function first saw the flag.
 */
bool sx1262_channel_busy(void)
{
    if (s_txing) {
        return false;
    }
    uint16_t irq = irq_get();
    if (!(irq & (IRQ_PREAMBLE | IRQ_HEADER_VALID))) {
        s_busy_since = 0;
        return false;
    }
    int64_t now = esp_timer_get_time();
    uint32_t limit = (irq & IRQ_HEADER_VALID) ? s_busy_max_us : s_pre_max_us;
    if (s_busy_since == 0) {
        s_busy_since = now;
    } else if (now - s_busy_since > limit) {
        irq_clear(IRQ_PREAMBLE | IRQ_HEADER_VALID);
        s_busy_since = 0;
        return false;
    }
    return true;
}

/* sx1262_noise_dbm() -- GetRssiInst: the channel's signal level right
 * now (the noise floor when nobody transmits), -RssiInst/2 dBm. */
int sx1262_noise_dbm(void)
{
    uint8_t r;
    cmd_get(CMD_GET_RSSI_INST, &r, 1);
    return -(int)r / 2;
}

/* sx1262_recover() -- a transmission never finished: reset, set up again,
 * listen. */
void sx1262_recover(void)
{
    ESP_LOGW(TAG, "transmission never finished: resetting the radio");
    s_txing = false;
    s_stats->resets++;
    reset();
    (void)configure();
    start_rx();
}

/* sx1262_check() -- should be in RX; a brown-out or glitch leaves it in
 * standby (reset state). Then set it up again. */
bool sx1262_check(void)
{
    if (s_txing) {
        return false;
    }
    int mode = chip_mode();
    if (mode == MODE_RX) {
        return false;
    }
    ESP_LOGW(TAG, "radio found in mode %d (expected RX): setting it up again", mode);
    s_stats->resets++;
    if (mode != MODE_STBY_RC) {
        standby();
    }
    (void)configure();
    start_rx();
    return true;
}
