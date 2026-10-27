/*
 * MorseLink — behaviour constants.
 *
 * Everything a tester may want to tweak lives here, so the logic files never
 * contain magic numbers. Values marked (spec) come from docs/SPEC.md; values
 * marked (agreed) were decided in conversation and are recorded there too.
 *
 * This header is plain C with no ESP-IDF dependencies: the host tests in
 * test/host include it as well.
 */
#pragma once

/* Firmware version: shown in the boot log and told to the phone app (the HI
 * line, phone.h). Goes up with every delivered zip. */
#define MORSELINK_VERSION "0.6.6"

/* ------------------------------------------------------------------------- */
/* Input (spec §8)                                                            */
/* ------------------------------------------------------------------------- */

#define KEY_DEBOUNCE_MS 6         /* the line must be quiet this long before the level is trusted */
#define HOLD_NOT_MORSE_MS 1500    /* presses longer than this are not dots or dashes */
#define HOLD_SEND_MS 2000         /* pip: send the draft / execute a command */
#define HOLD_CMD_MS 3000          /* pip pip: enter command mode */
#define HOLD_CANCEL_MS 5000       /* long buzz: cancel everything */

/* ------------------------------------------------------------------------- */
/* Composing                                                                  */
/* ------------------------------------------------------------------------- */

#define MSG_MAX_CHARS 100            /* (agreed) longest message, in characters */
#define DRAFT_ABANDON_MS (60 * 1000) /* (agreed) an untouched half-keyed message is discarded (unless K kept it) */
#define CMD_IDLE_EXIT_MS (30 * 1000) /* leave command mode if nothing is keyed for this long */

/* (v0.6.2, the learning view) The SP line (phone.h: the keying and reading
 * speeds) goes when they change, but no more often than this: the
 * calibration moves a little with every letter keyed. */
#define PHONE_SP_GAP_MS 1000

/* ------------------------------------------------------------------------- */
/* Queued alerts (spec §9: "play once the user has been idle a few s")       */
/* ------------------------------------------------------------------------- */

#define ALERT_IDLE_MS 3000          /* key must rest this long before an alert plays (no draft) */
#define ALERT_IDLE_COMPOSE_MS 10000 /* ... and this long while a draft exists */
#define ALERT_QUEUE_LEN 8           /* alerts waiting to play; the oldest drops out when full */
#define HISTORY_LEN 9               /* received messages kept for R1..R9 */

/* (agreed 8 Oct) A received message normally plays attention + sender ID +
 * text. In an active conversation it plays the text alone: when the last
 * exchange with that same device (a message either way, or connecting) was
 * at most this long ago. See convo.h. */
#define CONVO_QUIET_MS (5 * 60 * 1000)

/* ------------------------------------------------------------------------- */
/* Power switch (agreed 8 Oct; see power.h)                                   */
/* ------------------------------------------------------------------------- */

#define POWER_ON_HOLD_MS 2000     /* switched off: hold the key this long to switch on */
#define POWER_KEY_UP_MS 50        /* the key counts as released after this long up (contact bounce) */
#define POWER_ON_BUZZ_MS 150      /* the "you can let go" buzz once the hold is long enough */
#define POWER_RELEASE_WAIT_MS 10000 /* start anyway if the key is still down after this long */

/* ------------------------------------------------------------------------- */
/* SOS (agreed)                                                               */
/* ------------------------------------------------------------------------- */

#define SOS_CONFIRM_NOARG_MS 3000       /* silence after SOS with no target: send a general SOS */
#define SOS_CONFIRM_ARG_MS 2000         /* silence after a target slot/ID */
#define SOS_RESEND_FAST_MS (30 * 1000)  /* resend every 30 s ... */
#define SOS_FAST_PHASE_MS (10 * 60 * 1000) /* ... for the first 10 minutes ... */
#define SOS_RESEND_SLOW_MS (2 * 60 * 1000) /* ... then every 2 minutes until cancelled */
#define SOS_ALARM_REPEAT_MS (30 * 1000) /* receiver's alarm repeats until tapped */

/* ------------------------------------------------------------------------- */
/* Calibration                                                                */
/* ------------------------------------------------------------------------- */

#define CALIB_END_SILENCE_MS 3000                  /* PARIS input ends after this much silence */
#define CALIB_SAVE_MIN_INTERVAL_MS (5 * 60 * 1000) /* write the profile at most this often */
#define CALIB_SAVE_IDLE_MS 20000                   /* ... and only when the key has rested */

/* ------------------------------------------------------------------------- */
/* Haptics                                                                    */
/* ------------------------------------------------------------------------- */

#define HAPTIC_PWM_HZ 20000           /* LEDC frequency for the motor transistors */
#define HAPTIC_KICK_MS 30             /* (v0.6.4) below full strength (I1-I7) each motor starts at full for this
                                         long, then drops to the level: the spec's 30-50 ms spin-up */
#define LED_M2_PCT 15                 /* test setup: LED brightness meaning "forearm motor only" */
#define HAPTIC_RAIL_GATING 1          /* switch DCDC1 off between vibrations */
#define HAPTIC_RAIL_OFF_DELAY_MS 1500 /* ... this long after the last vibration */
#define HAPTIC_RAIL_SETTLE_MS 3       /* wait after switching the rail on */

/* ------------------------------------------------------------------------- */
/* Battery (agreed: warn at 15 % and 5 %)                                     */
/* ------------------------------------------------------------------------- */

#define BATTERY_POLL_MS 30000
#define BATTERY_WARN1_PCT 15
#define BATTERY_WARN2_PCT 5

/* ------------------------------------------------------------------------- */
/* Radio (agreed 6 Oct; spec §6)                                              */
/* ------------------------------------------------------------------------- */

/* 869.4-869.65 MHz allows 500 mW e.r.p. at < 10 % duty cycle under ICASA. */
#define RADIO_FREQ_HZ 869525000u
#define RADIO_BW_HZ 125000u
#define RADIO_CR 1          /* coding rate 4/(4+CR): 1 = 4/5 */

/* Spreading factor (agreed 15 Oct): a setting, changed on the device with
 * G 1-4 (= SF9, SF10, SF11, SF12) or `radio sf 9-12` in the console, and
 * stored. Both devices must use the same one: on different settings they
 * can't hear each other at all, SOS included. Each step up decodes 2.5 dB
 * deeper (SNR -12.5 dB at SF9 ... -20 dB at SF12) and takes about twice the
 * airtime: a 10-byte packet is 0.14 s at SF9, 1.0 s at SF12. */
#define RADIO_SF_DEFAULT 9
#define RADIO_SF_MIN 9
#define RADIO_SF_MAX 12

/* Keepalives get rarer at the slow settings, so that they stay a small
 * share of the airtime (a keepalive takes 0.14 / 0.29 / 0.58 / 1.15 s at
 * SF9 / 10 / 11 / 12): every 30 s up to SF10 (KEEPALIVE_MS), 45 s at SF11,
 * 60 s at SF12. A path counts as lost after three periods (link_timing). */
#define RADIO_KA_SF11_MS (45 * 1000)
#define RADIO_KA_SF12_MS (60 * 1000)

/* Private sync word. Meshtastic shares 869.525 MHz with sync word 0x2B;
 * 0x12 is the SX127x default and 0x34 is LoRaWAN's. Neither nibble of 0x5E
 * matches theirs, so the radio itself filters their packets out. */
#define RADIO_SYNC_WORD 0x5E

/* Transmit power, dBm (agreed 15 Oct: the chip's maximum by default; lower
 * levels stay available with `radio power` for comparisons). The SX1262
 * (T-Beam V1.2) goes up to 22 dBm, the SX1276 (V1.1) up to 20 on PA_BOOST
 * (its high-power mode above 17, where the datasheet asks for a low duty
 * cycle); each driver clamps to its own chip (radio_tx_dbm_max). Both are
 * below the 500 mW (27 dBm) e.r.p. allowed in 869.4-869.65 MHz. Lowering
 * the power saves very little battery: the radio transmits only a few per
 * cent of the time. */
#define RADIO_TX_DBM_DEFAULT 22
#define RADIO_TX_DBM_MIN 2
#define RADIO_TX_DBM_MAX 22        /* the most any chip takes (SX1262) */
#define RADIO_TX_DBM_MAX_SX1276 20 /* the SX1276's limit */

/* Preamble lengths in symbols. Phase 1 receivers listen all the time, so both
 * are the standard 8. Phase 2 low-power listening (CAD) will raise LONG above
 * the sniff interval: packets that must wake a sleeping receiver (requests,
 * messages, SOS) use LONG, replies use SHORT. See proto.h, PF_LONG_PREAMBLE. */
#define RADIO_PREAMBLE_SHORT 8
#define RADIO_PREAMBLE_LONG 8

/* Duty-cycle budget: 10 % of any rolling hour, per device. SOS traffic is
 * exempt (spec §10). Normal use stays far below it; the limit is a safety
 * net against a bug flooding the band. */
#define RADIO_DUTY_PERMILLE 100
#define RADIO_DUTY_WINDOW_MIN 60

/* ------------------------------------------------------------------------- */
/* Link protocol (agreed 3 and 6 Oct; spec §7)                                */
/* ------------------------------------------------------------------------- */

/* The values below are for SF9; link_timing() (link.h) stretches the ones
 * that depend on airtime for the slower settings. */
#define LINK_BUSY_MS (10 * 60 * 1000) /* busy until 10 min after the last message sent or received */
#define KEEPALIVE_MS (30 * 1000)      /* linked devices say "still here" every 30 s (whatever else is sent) */
#define LINK_LOST_MS (90 * 1000)      /* a path not proven both ways for 90 s is down (3 keepalives) */

/* Keepalives go out 0..KEEPALIVE_JITTER_MS early, at random, so two devices
 * that started together don't keep transmitting on top of each other. Three
 * keepalive periods then always fit inside LINK_LOST_MS. */
#define KEEPALIVE_JITTER_MS 4000

/* Retries. A request, an accept and a message are each sent up to
 * LINK_TRIES times, RETRY_GAP_MS apart (counted from the end of the copy,
 * plus up to RETRY_JITTER_MS at random). After the last copy the sender
 * waits REPLY_WAIT_MS for an answer before giving up: about 10 s in all at
 * SF9, about 45 s for a long message at SF12. */
#define LINK_TRIES 5
#define RETRY_GAP_MS 1600
#define RETRY_JITTER_MS 800
#define REPLY_WAIT_MS 2000

/* A request still unanswered after this long, even though nothing could be
 * transmitted (no radio, duty cycle used up), is reported as unavailable. */
#define LINK_HARD_TIMEOUT_MS (30 * 1000)

/* Requests waiting for the user's A / N (spec §7):
 *  - at most LINK_PENDING_MAX at a time; further ones are ignored;
 *  - one that has been played waits LINK_PENDING_MS for an answer;
 *  - one queued while busy waits up to LINK_QUEUED_MS to be played;
 *  - the requester keeps its request open (a late accept still connects)
 *    for LINK_REQ_OPEN_MS, which covers both. */
#define LINK_PENDING_MAX 3
#define LINK_PENDING_MS (15 * 60 * 1000)
#define LINK_QUEUED_MS (60 * 60 * 1000)
#define LINK_REQ_OPEN_MS (80 * 60 * 1000)

/* "Link ended" notices and UNPAIR / decline packets get no answer, so they
 * are simply sent NOTICE_COPIES times, NOTICE_GAP_MS apart. */
#define NOTICE_COPIES 2
#define NOTICE_GAP_MS 2500

/* Up to this many messages can wait behind the one being delivered. */
#define MSG_QUEUE_MAX 3

/* SOS relaying (spec §10): every device relays each SOS-family packet once,
 * after a random pause, until it has been relayed SOS_MAX_HOPS times. */
#define SOS_MAX_HOPS 2
#define SOS_RELAY_MIN_MS 200
#define SOS_RELAY_MAX_MS 1500
#define SOS_COPIES 2         /* each SOS (and each resend) goes out twice */
#define SOS_CANCEL_COPIES 3  /* a cancel goes out three times */

/* ------------------------------------------------------------------------- */
/* Discovery, D (spec §7; agreed 13 Oct; see disc.h)                          */
/* ------------------------------------------------------------------------- */

/* The sweep. Every discoverable device answers in one of DISC_SLOTS slots,
 * picked at random (simultaneous packets destroy each other). A slot is a
 * PRESENCE's airtime (145 ms at SF9) plus DISC_SLOT_GUARD_MS, so 16 slots
 * take about 3 s (spec: "default 3 s, 16 slots"). */
#define DISC_SLOTS 16          /* (spec) */
#define DISC_SLOT_GUARD_MS 45  /* timing slack between two answers: slot ~190 ms */
#define DISC_TURN_MS 20        /* the first slot starts this long after the DISCOVER ends
                                  (the searcher's radio turning round to listen) */
#define DISC_LATE_MS 500       /* the searcher listens this long after the last slot, for
                                  answers held back by listen-before-talk */
#define DISC_SEND_HARD_MS 5000 /* a DISCOVER that can't go out this soon (channel busy, duty
                                  cycle used up) ends the sweep: nobody found */

/* Answer limits (spec v1.0 §7.3: nobody can track a device by sweeping over
 * and over): a device answers the same searcher at most DISC_ANSWERS_PER_SRC
 * times, and all searchers together at most DISC_ANSWERS_TOTAL times, in any
 * DISC_ANSWER_WINDOW_MS. Silent mode (Z) never answers. */
#define DISC_ANSWERS_PER_SRC 3
#define DISC_ANSWERS_TOTAL 20
#define DISC_ANSWER_WINDOW_MS (10 * 60 * 1000)

/* Near / mid / far, by path loss = their transmit power - the RSSI here.
 * At 17 dBm: near = heard above -63 dBm, far = below -88 dBm.
 * (placeholders 13 Oct, to be set from bench numbers: every sweep logs each
 * device's path loss) */
#define DISC_NEAR_LOSS_DB 80  /* below this: near (N) */
#define DISC_FAR_LOSS_DB 105  /* from this up: far (F); in between: mid (M) */

/* Walking the list. */
#define DISC_WALK_IDLE_MS (30 * 1000) /* nothing keyed for this long: the list ends (as command mode) */
#define DISC_SCAN_TIMEOUT_MS 15000    /* safety net: no result this long after D -> error */

/* ------------------------------------------------------------------------- */
/* Loopback test network (one board, no radio needed)                         */
/* ------------------------------------------------------------------------- */

#define LOOPBACK_DELAY_MS 1500     /* the simulated peer echoes a message after this long */
#define LOOPBACK_ACCEPT_MS 1000    /* ... accepts a link request after this long */
#define LOOPBACK_SOS_ACK_MS 3000   /* ... acknowledges an SOS after this long */
