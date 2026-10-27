/*
 * MorseLink — haptic sequences.
 *
 * A job is a short list of items (system tone, Morse text, silence). A generator
 * turns a job into a stream of segments, each saying "hold M1 at level a and M2
 * at level b for N ms". The player (haptics.c) just walks that stream with
 * esp_timer, so everything here is pure C and host-testable.
 *
 * Channel rules (spec §9):
 *   dot  = M1 alone for 1U
 *   dash = M1 then M2 (sweep up the forearm), 3U total, with a short overlap
 *   system tones = both motors together (never confusable with a character)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HLEVEL_FULL 255
#define HLEVEL_QUIET 90

typedef struct {
    uint16_t ms;
    uint8_t m1; /* 0..255 */
    uint8_t m2; /* 0..255 */
} hseg_t;

typedef enum {
    TONE_PIP = 0,     /* single pip */
    TONE_SEND_ARMED,  /* hold threshold 2 s: pip */
    TONE_CMD_ARMED,   /* hold threshold 3 s: pip pip */
    TONE_CANCEL,      /* hold threshold 5 s / cancelled: long */
    TONE_OK,          /* pip-long: OK / accepted / sent */
    TONE_ERROR,       /* pip pip pip (fast): error / failed / unavailable */
    TONE_DEGRADED,    /* long-pip: keying degraded, suggest C */
    TONE_ATTENTION,   /* long + 3 pips: something arrived */
    TONE_PENDING,     /* slow pip-pip-pip: decision pending */
    TONE_DELIVERED,   /* two spaced pips */
    TONE_SOS_ARMED,   /* three long buzzes */
    TONE_SOS_ALARM,   /* 1 s on / 300 ms off x5 */
    TONE_QUIET_PIP,   /* one quiet pip (unverified peer, Phase 2) */
    TONE__COUNT
} tone_t;

typedef enum {
    HI_TONE = 0,
    HI_MORSE,
    HI_GAP,
} hitem_kind_t;

typedef struct {
    uint8_t kind; /* hitem_kind_t */
    uint8_t tone; /* tone_t for HI_TONE */
    uint16_t arg; /* HI_GAP: milliseconds; HI_MORSE: offset into job text */
} hitem_t;

#define HJOB_MAX_ITEMS 10
#define HJOB_TEXT_MAX 200

typedef struct {
    uint8_t n;
    hitem_t items[HJOB_MAX_ITEMS];
    uint16_t text_len;
    char text[HJOB_TEXT_MAX]; /* NUL-separated strings for HI_MORSE items */
} hjob_t;

/* How a dash feels (hparams_t.dash_style, command H 1 / 2). The timing is
 * the same either way (3U), only which motor runs changes. */
#define HDASH_SWEEP 1 /* H1, the default: wrist (M1) then forearm (M2) */
#define HDASH_M2 2    /* H2: forearm (M2) only; dots on the wrist (M1) only */

typedef struct {
    uint16_t unit_ms;       /* playback unit U (reading speed) */
    uint8_t overlap_cu;     /* (v0.6.4: in hundredths of U, was ms) M1 and M2 both on inside a dash: 30 = 0.3U */
    uint8_t dash_m1_cu;     /* where the dash hands over from M1 to M2, in hundredths of U: the middle of the
                               overlap. 125 with a 0.3U overlap (v0.6.4, agreed 24 Oct): M1 1.1U alone, both
                               0.3U, M2 1.6U alone, so M1 runs 1.4U and M2 1.9U (was 100 and 20 ms: 1U / 2U) */
    uint8_t farnsworth_x10; /* letter/word gap stretch x10 (10 = standard spacing) */
    uint16_t item_gap_ms;   /* silence between a tone and the next item */
    uint8_t dash_style;     /* (v0.6.3, command H) HDASH_SWEEP: a dash runs from M1 to M2; HDASH_M2: a dash is M2
                               alone for 3U (dots stay on M1), to compare the two in the participant sessions */
    uint8_t drive_pct;      /* (v0.6.4, command I) how hard the motors are driven, % of full (PWM): 100 = full */
    uint8_t kick_ms;        /* (v0.6.4) below full drive, each motor starts at full for this long, then drops to
                               drive_pct: a coin motor driven gently may not start from rest (0 = no kick) */
} hparams_t;

void hparams_default(hparams_t *p);

void hjob_init(hjob_t *j);
bool hjob_tone(hjob_t *j, tone_t t);
bool hjob_morse(hjob_t *j, const char *text); /* text is copied; spaces = word gaps */
bool hjob_gap(hjob_t *j, uint16_t ms);

#define HSEQ_BUF 24 /* (16 until v0.6.3; the start kicks split segments in two) */

/* "Not a Morse item, or one with nothing to play": hjob_item_starts()'s
 * value for such an item. */
#define HSEQ_NO_START UINT32_MAX

typedef struct {
    const hjob_t *job;
    hparams_t p;
    uint8_t item;
    uint16_t pos; /* char position in current Morse item */
    uint8_t el;   /* element index in current char */
    hseg_t buf[HSEQ_BUF];
    uint8_t head, tail, count;
    /* Bookkeeping for hjob_item_starts() (v0.6.2): the length of everything
     * pushed so far, and where to note when each Morse item's first element
     * starts (NULL: nobody asked). */
    uint32_t pushed_ms;
    uint32_t *starts; /* HJOB_MAX_ITEMS entries, indexed by item */
    /* The start kick (v0.6.4): which motors were on in the last segment
     * pushed, and how much of each one's kick is still to come (ms). */
    bool on1, on2;
    uint16_t kick1, kick2;
} hseq_t;

void hseq_start(hseq_t *s, const hjob_t *job, const hparams_t *p);
/* Next segment; false when the job is finished. Zero-length segments are never returned. */
bool hseq_next(hseq_t *s, hseg_t *out);

/* Total playing time of a job. */
uint32_t hjob_duration_ms(const hjob_t *j, const hparams_t *p);

/* When each Morse item of a job starts playing (v0.6.2, for the phone's
 * "now playing" view): out[i] = milliseconds from the start of the job to
 * the first element of item i, for every HI_MORSE item that has something
 * to play; HSEQ_NO_START for tones, gaps and empty Morse items. out has
 * HJOB_MAX_ITEMS entries. Inside an item the timing is the standard one
 * (dot 1U, dash 3U, 1U inside a character, 3U x Farnsworth between
 * characters, 7U x Farnsworth between words), so the phone can follow each
 * character from the item's start, the unit and the Farnsworth factor. */
void hjob_item_starts(const hjob_t *j, const hparams_t *p, uint32_t out[HJOB_MAX_ITEMS]);

/* Reading-speed table: Q digit 1..9 -> unit in ms (5 = 200 ms default). */
uint16_t hspeed_unit_ms(int q);

/* Vibration strength (command I; 1-5 in v0.6.2, 1-9 since v0.6.4, agreed
 * 24 Oct: a participant wanted it lower than the old I1, which only took the
 * supply down to 3.0 V). Level 9 is the old maximum: full drive with the
 * motor supply (DCDC1) at 3.4 V. Level 8 is full drive at 3.3 V, what the
 * board always had (the default). Levels 1-7 keep 3.3 V and drive the
 * motors with PWM (20 kHz, inaudible) at a lower duty, each motor starting
 * with a short kick at full (hparams_t.kick_ms). A coin motor's force goes
 * about with the square of its voltage, so the duties step the force by
 * roughly a quarter per level: level 1 is about a fifth of level 8. The
 * motor's recommended range is 2.5-3.5 V (Pololu #1638): levels 1-4 run it
 * below that once it is spinning, which is what the kick is for. */
#define HSTRENGTH_MIN 1
#define HSTRENGTH_MAX 9
#define HSTRENGTH_DEFAULT 8
/* Level (1..9; anything else counts as the default) -> the motor supply, mV. */
uint16_t hstrength_mv(int level);
/* Level (1..9; anything else counts as the default) -> drive, % of full. */
uint8_t hstrength_drive_pct(int level);
