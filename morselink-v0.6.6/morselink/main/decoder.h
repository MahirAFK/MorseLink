/*
 * MorseLink — keying decoder.
 *
 * Turns timed button presses into characters, using the adaptive calibration
 * for every decision (dot/dash boundary, letter and word gaps) and feeding
 * dictionary-validated characters back into it.
 *
 * SOS is recognised by shape as well as by classification, so it works even
 * with a wrecked or missing calibration profile:
 *   - one group of 9 elements whose middle three are the longest
 *     (the run-together prosign ...---..., anywhere), or
 *   - the last 9 elements having that shape, bounded by word-length silence
 *     on both sides, however they were split into characters or words (S O S
 *     keyed as letters, or a slow panicked SOS whose gaps broke it up).
 *     Checked when the closing silence arrives, so one misread element in
 *     "SORRY" can't raise an emergency mid-word.
 *
 * Time is in milliseconds (int64). Pure C, host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "calib.h"

#define DEC_MAX_EL 10 /* elements per group; longer groups decode as unknown */
#define DEC_NO_TIME INT64_MIN
#define DEC_IDLE_BUMP_MS (20LL * 60 * 1000) /* spec: re-converge after 20 min idle */

typedef enum {
    DEC_CHAR = 1, /* c holds the character */
    DEC_UNKNOWN,  /* pattern not in the table; c = '?' */
    DEC_SPACE,    /* word gap */
    DEC_ERROR,    /* error prosign: delete the previous character */
    DEC_SOS,      /* SOS; the sos_undo characters emitted just before it belong to it */
} dec_kind_t;

typedef struct {
    dec_kind_t kind;
    char c;
    uint8_t n_el;
    uint8_t sos_undo;
    char pattern[DEC_MAX_EL + 1];
    uint16_t el_ms[DEC_MAX_EL]; /* element durations, for logging */
} dec_evt_t;

typedef struct {
    calib_t *cal;
    /* current group */
    uint8_t n;
    bool overflow;
    bool group_open;
    float el[DEC_MAX_EL];
    uint8_t dash[DEC_MAX_EL];
    uint8_t learn[DEC_MAX_EL];
    float gap[DEC_MAX_EL]; /* gap[i] = silence before element i (i >= 1) */
    float lead_gap;        /* silence before the group's first element, < 0 if unknown */
    /* timing context */
    bool pressed;         /* key is down: silence stopped at press_t */
    int64_t press_t;
    int64_t last_up;      /* end of the last element; DEC_NO_TIME after reset */
    int64_t last_any_up;  /* survives resets; drives the idle alpha bump */
    bool word_open;       /* characters emitted since the last word space */
    bool prev_committed;  /* previous group was learned from and is adjacent */
    /* the last 9 elements, for SOS by shape */
    uint8_t r_n;               /* valid entries (<= 9) */
    float r_el[9];
    uint8_t r_dash[9];
    float r_gap[9];            /* silence before each element, < 0 if unknown */
    uint16_t r_text_before[9]; /* text events emitted before each element */
    uint16_t r_char_before[9]; /* character events emitted before each element */
    uint16_t text_evts;        /* CHAR/UNKNOWN/SPACE events emitted */
    uint16_t char_evts;        /* CHAR/UNKNOWN events emitted */
    /* which emitted characters were learned from (for error-prosign rollback) */
    uint8_t hist[16];
    uint8_t hist_n;
    /* output queue */
    dec_evt_t q[6];
    uint8_t qh, qn;
    /* the latest element (v0.6.2, for the phone's live keying view): never
     * reset, so the UI can tell whether a release became an element */
    uint32_t n_pressed; /* elements accepted since dec_init (bounces don't count) */
    uint16_t last_ms;   /* the latest element's length */
    uint8_t last_dash;  /* ... and its label (1 = dash) */
} decoder_t;

void dec_init(decoder_t *d, calib_t *cal);
/* Drop any open group and the timing context (state changes, after holds). */
void dec_reset(decoder_t *d);
/* The key went down: process timeouts up to t_down, then stop the gap clock
 * until the press ends (dec_element or dec_flush). */
void dec_key_down(decoder_t *d, int64_t t_down);
/* A completed press shorter than the command-hold threshold. Presses under
 * 20 ms are treated as switch bounce and ignored. */
void dec_element(decoder_t *d, int64_t t_down, int64_t t_up);
/* A hold has started: close any open group now, no word space. */
void dec_flush(decoder_t *d);
/* Emit any timeouts due at `now`. Call on every key-down (before the press is
 * processed) and whenever dec_deadline() passes. */
void dec_poll(decoder_t *d, int64_t now);
/* Next time dec_poll() has work to do, or INT64_MAX. */
int64_t dec_deadline(const decoder_t *d);
bool dec_get(decoder_t *d, dec_evt_t *out);
bool dec_group_open(const decoder_t *d);
/* The letter being keyed, as classified so far ("-." after a dash and a
 * dot; "" when no letter is open), into out[size] (NUL-terminated, cut to
 * fit). Returns its length. At most DEC_MAX_EL elements are kept: an
 * overlong group shows its first ten. */
int dec_open_pattern(const decoder_t *d, char *out, int size);

/* SOS shape test on 9 element durations (exposed for the calibration screen). */
bool dec_sos_shape9(const float el[9]);
