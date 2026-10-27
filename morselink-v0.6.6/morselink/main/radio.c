/*
 * MorseLink — LoRa radio driver (see radio.h): finds out which chip is
 * fitted, drives an SX1276 itself, and hands every call to radio_sx1262.c
 * when the chip is an SX1262 (each API function below starts with that
 * check).
 *
 * Register numbers and bit meanings are from the Semtech SX1276/77/78/79
 * datasheet (rev. 7), chapter 6.4 "LoRa Mode Register Map". Only LoRa mode
 * is used; the chip starts in FSK mode after reset and is switched over.
 *
 * SPI: one byte of address (bit 7 set = write) followed by data. Reading or
 * writing register 0x00 (the FIFO) repeatedly moves through the FIFO, so a
 * long packet can be moved in several short transactions; that keeps the
 * SPI driver in its simple non-DMA mode (64 bytes at most per transaction).
 */
#include "radio.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "proto.h"
#include "radio_sx1262.h"

static const char *TAG = "radio";

/* ---- SX1276 registers (LoRa mode) ---------------------------------------- */
#define REG_FIFO 0x00
#define REG_OPMODE 0x01
#define REG_FRF_MSB 0x06
#define REG_FRF_MID 0x07
#define REG_FRF_LSB 0x08
#define REG_PA_CONFIG 0x09
#define REG_OCP 0x0B
#define REG_LNA 0x0C
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FIFO_TX_BASE 0x0E
#define REG_FIFO_RX_BASE 0x0F
#define REG_FIFO_RX_CURRENT 0x10
#define REG_IRQ_FLAGS_MASK 0x11
#define REG_IRQ_FLAGS 0x12
#define REG_RX_NB_BYTES 0x13
#define REG_MODEM_STAT 0x18
#define REG_PKT_SNR 0x19
#define REG_PKT_RSSI 0x1A
#define REG_RSSI 0x1B
#define REG_HOP_CHANNEL 0x1C
#define REG_MODEM_CONFIG1 0x1D
#define REG_MODEM_CONFIG2 0x1E
#define REG_PREAMBLE_MSB 0x20
#define REG_PREAMBLE_LSB 0x21
#define REG_PAYLOAD_LENGTH 0x22
#define REG_MAX_PAYLOAD 0x23
#define REG_MODEM_CONFIG3 0x26
#define REG_IF_FREQ1 0x2F
#define REG_IF_FREQ2 0x30
#define REG_DETECT_OPTIMIZE 0x31
#define REG_DETECTION_THRESHOLD 0x37
#define REG_SYNC_WORD 0x39
#define REG_DIO_MAPPING1 0x40
#define REG_VERSION 0x42
#define REG_PA_DAC 0x4D

/* RegOpMode: bit 7 = LoRa, bits 2-0 = mode. Bit 3 (low-frequency registers)
 * stays 0 because 869 MHz is in the chip's high-frequency band. */
#define MODE_LORA 0x80
#define MODE_SLEEP 0x00
#define MODE_STDBY 0x01
#define MODE_TX 0x03
#define MODE_RX_CONT 0x05

/* RegIrqFlags bits (write 1 to clear). */
#define IRQ_RX_DONE 0x40
#define IRQ_CRC_ERROR 0x20
#define IRQ_TX_DONE 0x08

/* RegDioMapping1 bits 7-6 choose what DIO0 signals. */
#define DIO0_RX_DONE 0x00
#define DIO0_TX_DONE 0x40

#define SX1276_VERSION 0x12
#define FXOSC_HZ 32000000ull   /* the module's crystal */
#define RSSI_OFFSET_HF (-157)  /* RSSI register offset in the 862-1020 MHz band */
#define SPI_HZ (8 * 1000 * 1000)
#define SPI_CHUNK 64           /* longest transaction without DMA */

/* ---- state ---------------------------------------------------------------- */
static spi_device_handle_t s_spi;
static radio_cfg_t s_cfg;
static bool s_present;
static bool s_sx1262;      /* true: the chip is an SX1262, radio_sx1262.c drives it */
static char s_problem[112]; /* why there is no radio ("" if there is one) */
static bool s_txing;
static bool s_bench_cw;       /* bench power: a plain carrier is on (TxContinuousMode) */
static int s_bench_keep_dbm;  /* ... and the power to go back to afterwards */
static uint8_t s_version;
static TaskHandle_t s_task;    /* woken by the DIO0 interrupt */
static uint32_t s_bit;
static volatile uint32_t s_irqs;
static radio_stats_t s_stats;

/* ========================================================================= */
/* SPI register access                                                        */
/* ========================================================================= */

/* wr() -- write one register. */
static void wr(uint8_t reg, uint8_t val)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .addr = reg | 0x80, /* bit 7 set: write */
        .length = 8,
        .tx_data = {val},
    };
    spi_device_polling_transmit(s_spi, &t);
}

/* rd() -- read one register (0 if the bus fails). */
static uint8_t rd(uint8_t reg)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .addr = reg & 0x7F, /* bit 7 clear: read */
        .length = 8,
        .rxlength = 8,
        .tx_data = {0},
    };
    if (spi_device_polling_transmit(s_spi, &t) != ESP_OK) {
        return 0;
    }
    return t.rx_data[0];
}

/* fifo_write() / fifo_read() -- move n bytes to / from the FIFO, in chunks
 * of SPI_CHUNK (each access continues where the previous one stopped). */
static void fifo_write(const uint8_t *data, int n)
{
    while (n > 0) {
        int k = n > SPI_CHUNK ? SPI_CHUNK : n;
        spi_transaction_t t = {
            .addr = REG_FIFO | 0x80,
            .length = (size_t)k * 8,
            .tx_buffer = data,
        };
        spi_device_polling_transmit(s_spi, &t);
        data += k;
        n -= k;
    }
}

/* fifo_read() -- the reading half: zeros are clocked out while the
 * packet's bytes come in. */
static void fifo_read(uint8_t *data, int n)
{
    static const uint8_t zeros[SPI_CHUNK]; /* clocked out while reading */
    while (n > 0) {
        int k = n > SPI_CHUNK ? SPI_CHUNK : n;
        spi_transaction_t t = {
            .addr = REG_FIFO,
            .length = (size_t)k * 8,
            .rxlength = (size_t)k * 8,
            .tx_buffer = zeros,
            .rx_buffer = data,
        };
        spi_device_polling_transmit(s_spi, &t);
        data += k;
        n -= k;
    }
}

/* set_mode() -- change operating mode, staying in LoRa. */
static void set_mode(uint8_t mode)
{
    wr(REG_OPMODE, MODE_LORA | mode);
}

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

/* bw_code() -- RegModemConfig1 bandwidth code for bw_hz (nearest not above). */
static uint8_t bw_code(uint32_t bw_hz)
{
    static const uint32_t bw[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};
    uint8_t code = 0;
    for (uint8_t i = 0; i < sizeof(bw) / sizeof(bw[0]); i++) {
        if (bw_hz >= bw[i]) {
            code = i;
        }
    }
    return code;
}

/* set_preamble() -- RegPreamble (symbols, not counting the 4.25 sync symbols). */
static void set_preamble(uint16_t n)
{
    wr(REG_PREAMBLE_MSB, (uint8_t)(n >> 8));
    wr(REG_PREAMBLE_LSB, (uint8_t)n);
}

/*
 * apply_tx_power() -- program the PA_BOOST output (the T-Beam's antenna is
 * on PA_BOOST; the RFO pin is not connected).
 *   2..17 dBm:  PaDac normal (0x84), OutputPower = dBm - 2, current limit 100 mA
 *   18..20 dBm: PaDac high-power (0x87), OutputPower = dBm - 5, limit 140 mA
 * (datasheet §5.4.2-5.4.4 and RegOcp: Imax = 45 + 5*trim below 120 mA,
 * -30 + 10*trim above).
 */
static void apply_tx_power(int dbm)
{
    if (dbm < RADIO_TX_DBM_MIN) {
        dbm = RADIO_TX_DBM_MIN;
    }
    if (dbm > RADIO_TX_DBM_MAX_SX1276) {
        dbm = RADIO_TX_DBM_MAX_SX1276; /* (22 dBm is the SX1262's: this chip stops at 20) */
    }
    if (dbm > 17) {
        wr(REG_PA_DAC, 0x87);
        wr(REG_OCP, 0x20 | 17); /* OCP on, 140 mA */
        wr(REG_PA_CONFIG, (uint8_t)(0x80 | (dbm - 5)));
    } else {
        wr(REG_PA_DAC, 0x84);
        wr(REG_OCP, 0x20 | 11); /* OCP on, 100 mA */
        wr(REG_PA_CONFIG, (uint8_t)(0x80 | (dbm - 2)));
    }
    s_cfg.tx_dbm = (int8_t)dbm;
}

/*
 * apply_modem() -- the modem settings from s_cfg (in standby or sleep):
 * bandwidth, coding rate and explicit header; spreading factor and payload
 * CRC; AGC, and the low-data-rate optimisation if a symbol lasts more than
 * 16 ms (SF11/12 at 125 kHz: it must match the sender, and proto.c uses the
 * same rule for airtime).
 */
static void apply_modem(void)
{
    wr(REG_MODEM_CONFIG1, (uint8_t)((bw_code(s_cfg.bw_hz) << 4) | ((s_cfg.cr & 0x07) << 1)));
    wr(REG_MODEM_CONFIG2, (uint8_t)((s_cfg.sf << 4) | 0x04));
    lora_params_t lp = {s_cfg.sf, s_cfg.bw_hz, s_cfg.cr};
    wr(REG_MODEM_CONFIG3, (uint8_t)(0x04 | (lora_needs_ldro(&lp) ? 0x08 : 0x00)));
}

/*
 * configure() -- put the chip into LoRa mode with our settings, ending in
 * standby. The mode bit (LoRa / FSK) can only change in sleep.
 * Returns false if the chip didn't take LoRa mode.
 */
static bool configure(void)
{
    wr(REG_OPMODE, MODE_SLEEP); /* FSK sleep */
    wr(REG_OPMODE, MODE_LORA | MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(10));
    if ((rd(REG_OPMODE) & MODE_LORA) == 0) {
        return false;
    }

    /* Carrier: Frf = f * 2^19 / Fxosc (61.035 Hz steps). 869.525 MHz -> 0xD9619A. */
    uint32_t frf = (uint32_t)(((uint64_t)s_cfg.freq_hz << 19) / FXOSC_HZ);
    wr(REG_FRF_MSB, (uint8_t)(frf >> 16));
    wr(REG_FRF_MID, (uint8_t)(frf >> 8));
    wr(REG_FRF_LSB, (uint8_t)frf);

    /* The whole 256-byte FIFO serves both directions, one packet at a time. */
    wr(REG_FIFO_TX_BASE, 0);
    wr(REG_FIFO_RX_BASE, 0);
    wr(REG_MAX_PAYLOAD, 0xFF);

    /* LNA: maximum gain, high-frequency boost on (AGC adjusts the gain). */
    wr(REG_LNA, 0x23);

    apply_modem();

    /* Detection settings for SF7..12 (datasheet table 42). Bit 7 of
     * DetectOptimize is cleared, and the IF registers set, as errata
     * note 2.3 ("Receiver spurious reception of a LoRa signal") prescribes
     * for bandwidths below 500 kHz. */
    bool wide = s_cfg.bw_hz >= 500000; /* 500 kHz keeps the automatic IF (bit 7 set) */
    wr(REG_DETECT_OPTIMIZE, (uint8_t)((rd(REG_DETECT_OPTIMIZE) & 0x78) | (wide ? 0x80 : 0x00) | 0x03));
    wr(REG_DETECTION_THRESHOLD, 0x0A);
    if (s_cfg.bw_hz >= 62500 && !wide) {
        wr(REG_IF_FREQ2, 0x00);
        wr(REG_IF_FREQ1, 0x40);
    }

    set_preamble(s_cfg.rx_preamble);
    wr(REG_SYNC_WORD, s_cfg.sync_word);
    apply_tx_power(s_cfg.tx_dbm);

    wr(REG_IRQ_FLAGS_MASK, 0x00); /* all interrupt sources enabled; DIO0's mapping picks one */
    wr(REG_IRQ_FLAGS, 0xFF);      /* clear anything pending */
    set_mode(MODE_STDBY);
    return true;
}

/* start_rx() -- listen continuously; DIO0 rises on each packet received. */
static void start_rx(void)
{
    set_mode(MODE_STDBY);
    wr(REG_DIO_MAPPING1, DIO0_RX_DONE);
    wr(REG_IRQ_FLAGS, 0xFF);
    wr(REG_FIFO_ADDR_PTR, 0);
    set_mode(MODE_RX_CONT);
}

/* hard_reset() -- pulse NRESET low, then release it and give the chip time
 * to start (datasheet §7.2: low for > 100 us, then wait 5 ms). The pin is
 * open-drain (set up in radio_init), so "1" means released: the chip's own
 * pull-up takes it high, as the datasheet wants. */
static void hard_reset(void)
{
    gpio_set_level(PIN_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ========================================================================= */
/* Interrupt                                                                  */
/* ========================================================================= */

/*
 * dio0_isr() -- DIO0 rose: a packet was sent or received. Runs in interrupt
 * context (in IRAM, as the GPIO ISR service is installed IRAM-safe), so it
 * only wakes the owning task, which does the SPI work.
 */
static void IRAM_ATTR dio0_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    s_irqs++;
    if (s_task) {
        xTaskNotifyFromISR(s_task, s_bit, eSetBits, &woken);
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

/* ========================================================================= */
/* API                                                                        */
/* ========================================================================= */

/*
 * radio_init() -- see radio.h. In order: SPI bus (no DMA, 8 MHz) and device,
 * reset pin (open-drain), hardware reset, version check, LoRa configuration,
 * DIO0 interrupt, continuous receive.
 */
esp_err_t radio_init(const radio_cfg_t *cfg, TaskHandle_t task, uint32_t bit)
{
    s_cfg = *cfg;
    s_task = task;
    s_bit = bit;

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LORA_MOSI,
        .miso_io_num = PIN_LORA_MISO,
        .sclk_io_num = PIN_LORA_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_CHUNK,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(err));
        return err;
    }
    spi_device_interface_config_t dev = {
        .address_bits = 8, /* the register address goes out first */
        .mode = 0,         /* SX1276: clock idle low, sample on the rising edge */
        .clock_speed_hz = SPI_HZ,
        .spics_io_num = PIN_LORA_CS,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device: %s", esp_err_to_name(err));
        return err;
    }

    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << PIN_LORA_RST,
        .mode = GPIO_MODE_OUTPUT_OD,
    };
    gpio_set_level(PIN_LORA_RST, 1); /* released from the start */
    gpio_config(&rst);
    hard_reset();
    s_version = rd(REG_VERSION);
    if (s_version != SX1276_VERSION) {
        /* Not an SX1276: free the chip select for the SX1262 back end,
         * which speaks commands instead of registers, and try that. */
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        if (!sx1262_probe()) {
            snprintf(s_problem, sizeof s_problem,
                     "no chip answered (not an SX1276: version register 0x%02x; not an SX1262 either)", s_version);
            ESP_LOGE(TAG, "no LoRa radio found: %s", s_problem);
            return ESP_ERR_NOT_FOUND;
        }
        s_sx1262 = true;
        err = sx1262_init(&s_cfg, task, bit, &s_stats, s_problem, sizeof s_problem);
        s_present = (err == ESP_OK);
        return err;
    }
    if (!configure()) {
        snprintf(s_problem, sizeof s_problem, "SX1276 found but it did not enter LoRa mode");
        ESP_LOGE(TAG, "%s", s_problem);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* DIO0 -> interrupt (rising edge = TxDone / RxDone). */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_LORA_DIO0,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io);
    /* The GPIO interrupt service is installed once by app_main(). */
    gpio_isr_handler_add(PIN_LORA_DIO0, dio0_isr, NULL);

    s_present = true;
    start_rx();
    ESP_LOGI(TAG, "SX1276 ready: %.3f MHz, SF%d, %lu kHz, CR 4/%d, sync 0x%02X, %d dBm", s_cfg.freq_hz / 1e6,
             s_cfg.sf, (unsigned long)(s_cfg.bw_hz / 1000), 4 + s_cfg.cr, s_cfg.sync_word, s_cfg.tx_dbm);
    return ESP_OK;
}

/* radio_present() -- did a radio answer (and start) at boot? */
bool radio_present(void)
{
    return s_present;
}

/* radio_chip_version() -- RegVersion as read at boot (0x12 = SX1276). */
uint8_t radio_chip_version(void)
{
    return s_version;
}

/* radio_problem() -- why radio_init() found no working radio ("" if it did,
 * or before radio_init() has run). */
const char *radio_problem(void)
{
    return s_problem;
}

/* radio_chip_name() -- "SX1276", "SX1262", or "none". */
const char *radio_chip_name(void)
{
    if (!s_present) {
        return "none";
    }
    return s_sx1262 ? "SX1262" : "SX1276";
}

/* radio_set_tx_power() -- takes effect from the next packet. The PA
 * registers are only written in standby, so receiving is paused briefly. */
esp_err_t radio_set_tx_power(int dbm)
{
    if (s_sx1262 && s_present) {
        return sx1262_set_tx_power(dbm);
    }
    if (!s_present) {
        s_cfg.tx_dbm = (int8_t)dbm;
        return ESP_ERR_INVALID_STATE;
    }
    if (s_txing) {
        return ESP_ERR_INVALID_STATE;
    }
    set_mode(MODE_STDBY);
    apply_tx_power(dbm);
    start_rx();
    return ESP_OK;
}

/* radio_tx_power() -- the transmit power in use, dBm. */
int radio_tx_power(void)
{
    if (s_sx1262) {
        return sx1262_tx_power();
    }
    return s_cfg.tx_dbm;
}

/* radio_tx_dbm_max() -- see radio.h. */
int radio_tx_dbm_max(void)
{
    if (!s_present) {
        return RADIO_TX_DBM_MAX;
    }
    return s_sx1262 ? RADIO_TX_DBM_MAX : RADIO_TX_DBM_MAX_SX1276;
}

/* radio_set_sf() -- see radio.h. The modem registers are written in
 * standby, then receiving starts again. */
esp_err_t radio_set_sf(int sf)
{
    if (sf < RADIO_SF_MIN || sf > RADIO_SF_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sx1262 && s_present) {
        esp_err_t err = sx1262_set_sf(sf);
        if (err == ESP_OK) {
            s_cfg.sf = (uint8_t)sf;
        }
        return err;
    }
    if (!s_present || s_txing) {
        return ESP_ERR_INVALID_STATE;
    }
    set_mode(MODE_STDBY);
    s_cfg.sf = (uint8_t)sf;
    apply_modem();
    start_rx();
    ESP_LOGI(TAG, "SF%d", sf);
    return ESP_OK;
}

/* radio_sf() -- the spreading factor in use. */
int radio_sf(void)
{
    return s_cfg.sf;
}

/*
 * radio_transmit() -- standby, DIO0 to "TxDone", packet into the FIFO,
 * preamble length for this packet, then TX. The chip returns to standby by
 * itself when done; radio_service() then restarts receiving.
 */
esp_err_t radio_transmit(const uint8_t *buf, int len, uint16_t preamble)
{
    if (s_sx1262 && s_present) {
        return sx1262_transmit(buf, len, preamble);
    }
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len <= 0 || len > 255) {
        return ESP_ERR_INVALID_ARG;
    }
    set_mode(MODE_STDBY);
    wr(REG_DIO_MAPPING1, DIO0_TX_DONE);
    wr(REG_IRQ_FLAGS, 0xFF);
    wr(REG_FIFO_ADDR_PTR, 0);
    fifo_write(buf, len);
    wr(REG_PAYLOAD_LENGTH, (uint8_t)len);
    if (preamble != s_cfg.rx_preamble) {
        set_preamble(preamble);
    }
    s_txing = true;
    set_mode(MODE_TX);
    return ESP_OK;
}

/* radio_transmitting() -- between radio_transmit() and its TX_DONE. */
bool radio_transmitting(void)
{
    if (s_sx1262) {
        return sx1262_transmitting();
    }
    return s_txing;
}

/*
 * radio_service() -- see radio.h.
 * A received packet is accepted only if the chip reports no CRC error AND
 * the packet's own header says it carried a CRC (a sender with CRC off
 * would otherwise get through unchecked).
 * Signal strength (datasheet §5.5.5, high-frequency port):
 *   SNR >= 0: RSSI = -157 + 16/15 * PacketRssi
 *   SNR <  0: RSSI = -157 + PacketRssi + SNR
 */
radio_ev_t radio_service(uint8_t *buf, int maxlen, radio_rx_t *rx)
{
    if (s_sx1262 && s_present) {
        return sx1262_service(buf, maxlen, rx);
    }
    if (!s_present) {
        return RADIO_EV_NONE;
    }
    uint8_t flags = rd(REG_IRQ_FLAGS);
    wr(REG_IRQ_FLAGS, flags); /* clear exactly what we are handling */
    if (s_txing && (flags & IRQ_TX_DONE)) {
        s_txing = false;
        s_stats.tx++;
        set_preamble(s_cfg.rx_preamble); /* back to the receive setting */
        start_rx();
        return RADIO_EV_TX_DONE;
    }
    if (!(flags & IRQ_RX_DONE)) {
        return RADIO_EV_NONE;
    }
    bool crc_present = (rd(REG_HOP_CHANNEL) & 0x40) != 0; /* CrcOnPayload, from the received header */
    if ((flags & IRQ_CRC_ERROR) || !crc_present) {
        s_stats.rx_bad++;
        return RADIO_EV_RX_BAD;
    }
    int n = rd(REG_RX_NB_BYTES);
    wr(REG_FIFO_ADDR_PTR, rd(REG_FIFO_RX_CURRENT)); /* start of the packet just received */
    if (n > maxlen) {
        n = maxlen;
    }
    fifo_read(buf, n);
    int8_t snr = (int8_t)rd(REG_PKT_SNR);
    int prssi = rd(REG_PKT_RSSI);
    int rssi = snr >= 0 ? RSSI_OFFSET_HF + prssi * 16 / 15 : RSSI_OFFSET_HF + prssi + snr / 4;
    rx->len = n;
    rx->rssi = (int16_t)rssi;
    rx->snr_x4 = snr;
    s_stats.rx++;
    return RADIO_EV_RX;
}

/* radio_irq_pending() -- DIO0 is still high: an event is waiting (covers
 * an edge the interrupt missed). */
bool radio_irq_pending(void)
{
    if (s_sx1262) {
        return s_present && sx1262_irq_pending();
    }
    return s_present && gpio_get_level(PIN_LORA_DIO0) == 1;
}

/* radio_channel_busy() -- RegModemStat: bit 0 signal detected, bit 1
 * synchronised, bit 3 header received. Any of them means a packet from
 * someone else is in progress. */
bool radio_channel_busy(void)
{
    if (s_sx1262) {
        return s_present && sx1262_channel_busy();
    }
    if (!s_present || s_txing) {
        return false;
    }
    return (rd(REG_MODEM_STAT) & 0x0B) != 0;
}

/* radio_noise_dbm() -- the channel's RSSI right now (RegRssiValue): the
 * noise floor when nobody transmits. */
int radio_noise_dbm(void)
{
    if (s_sx1262) {
        return s_present ? sx1262_noise_dbm() : 0;
    }
    return s_present ? RSSI_OFFSET_HF + rd(REG_RSSI) : 0;
}

/* radio_recover() -- a transmission never reported TX_DONE: reset the chip,
 * set it up again and listen. */
void radio_recover(void)
{
    if (s_sx1262) {
        if (s_present) {
            sx1262_recover();
        }
        return;
    }
    if (!s_present) {
        return;
    }
    ESP_LOGW(TAG, "transmission never finished: resetting the radio");
    s_txing = false;
    s_stats.resets++;
    hard_reset();
    configure();
    start_rx();
}

/* radio_check() -- every few seconds while idle: is the chip still in
 * continuous receive? A brown-out resets it to FSK standby (0x09). */
bool radio_check(void)
{
    if (s_sx1262) {
        return s_present && sx1262_check();
    }
    if (!s_present || s_txing) {
        return false;
    }
    uint8_t mode = rd(REG_OPMODE);
    if (mode == (MODE_LORA | MODE_RX_CONT)) {
        return false;
    }
    ESP_LOGW(TAG, "radio found in mode 0x%02x (expected 0x85): setting it up again", mode);
    s_stats.resets++;
    configure();
    start_rx();
    return true;
}

/*
 * radio_bench_standby() / radio_bench_carrier() / radio_bench_listen() --
 * see radio.h. The SX1276 has no plain-carrier command in LoRa mode, so
 * its "continuous transmit" mode (RegModemConfig2 bit 3, meant for spectrum
 * measurements) sends a few bytes over and over: the same current as any
 * transmission at that power, which is what the measurement wants.
 */
esp_err_t radio_bench_standby(void)
{
    if (s_sx1262 && s_present) {
        return sx1262_bench_standby();
    }
    if (!s_present || s_txing) {
        return ESP_ERR_INVALID_STATE;
    }
    set_mode(MODE_STDBY);
    return ESP_OK;
}

/* radio_bench_carrier() -- the SX1276 half: power, continuous mode, a FIFO
 * of filler, TX. */
esp_err_t radio_bench_carrier(int dbm)
{
    if (s_sx1262 && s_present) {
        return sx1262_bench_carrier(dbm);
    }
    if (!s_present || s_txing) {
        return ESP_ERR_INVALID_STATE;
    }
    set_mode(MODE_STDBY);
    if (!s_bench_cw) {
        s_bench_keep_dbm = s_cfg.tx_dbm;
    }
    apply_tx_power(dbm);
    wr(REG_MODEM_CONFIG2, (uint8_t)(rd(REG_MODEM_CONFIG2) | 0x08)); /* TxContinuousMode */
    wr(REG_IRQ_FLAGS, 0xFF);
    wr(REG_FIFO_ADDR_PTR, 0);
    for (int i = 0; i < 16; i++) {
        wr(REG_FIFO, 0x55);
    }
    wr(REG_PAYLOAD_LENGTH, 16);
    set_mode(MODE_TX);
    s_bench_cw = true;
    return ESP_OK;
}

/* radio_bench_listen() -- the SX1276 half: carrier off (continuous mode
 * off, the usual power back), then receive. */
esp_err_t radio_bench_listen(void)
{
    if (s_sx1262 && s_present) {
        return sx1262_bench_listen();
    }
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    set_mode(MODE_STDBY);
    if (s_bench_cw) {
        wr(REG_MODEM_CONFIG2, (uint8_t)(rd(REG_MODEM_CONFIG2) & ~0x08));
        apply_tx_power(s_bench_keep_dbm);
        s_bench_cw = false;
    }
    start_rx();
    return ESP_OK;
}

/* radio_get_stats() -- packet counters and interrupts seen. */
void radio_get_stats(radio_stats_t *st)
{
    *st = s_stats;
    st->irqs = s_irqs;
}
