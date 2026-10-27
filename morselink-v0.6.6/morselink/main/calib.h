/*
 * MorseLink — adaptive keying calibration (spec §10).
 *
 * Dots and dashes are tracked as two independent distributions (mean + mean
 * absolute deviation); so are intra-character and letter gaps. The dot/dash
 * ratio is a measured diagnostic, never an assumption.
 *
 * Changes from the handover spec, agreed in conversation:
 *  - MAD updates clip the deviation at 3x MAD, so one hesitant element can't
 *    collapse d' (e.g. a 1.4 s dash right after PARIS).
 *  - Initial MADs come from the spread measured during PARIS (with a floor),
 *    not a flat 20 % that starts low-ratio keyers as "marginal".
 *  - The letter-gap threshold is anchored to dot_mean and kept clear of the
 *    intra-gap tail, because dictionary validation can't catch bad gap labels
 *    (every 1-3 element pattern is a valid letter).
 *  - When keying degrades, the boundary is held at the last good value but the
 *    means keep learning in the background, so the profile can recover
 *    without a forced recalibration.
 *
 * Pure C, host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CAL_PROFILE_VERSION 1

typedef enum {
    CAL_NONE = 0,  /* not calibrated */
    CAL_HEALTHY,   /* d' > 1.5: adapt normally */
    CAL_MARGINAL,  /* 1.0..1.5: boundary frozen, means keep learning */
    CAL_COLLAPSED, /* < 1.0: boundary held at last good value, "keying degraded" */
} cal_health_t;

/* Everything learned; persisted to NVS as a blob. */
typedef struct {
    uint16_t version;
    uint8_t valid;
    uint8_t reserved;
    float dot_mean, dot_mad;
    float dash_mean, dash_mad;
    float ratio;
    float gi_mean, gi_mad; /* gap inside a character */
    float gl_mean, gl_mad; /* gap between characters */
    float last_good_boundary;
    uint32_t n_dot, n_dash; /* labelled samples (PARIS included) */
    uint32_t n_conf;        /* confident element updates since calibration */
} cal_profile_t;

typedef struct {
    cal_profile_t p;
    uint16_t since_dot, since_dash; /* elements since each class was last seen */
    uint16_t bump_elems;            /* elements since an idle bump, 0xFFFF = none */
} cal_state_t;

#define CAL_SNAPSHOTS 8

typedef struct {
    cal_state_t s;
    cal_state_t snap[CAL_SNAPSHOTS]; /* state before each recent committed group */
    uint8_t snap_head, snap_count;
    uint8_t widen_left; /* elements left with a widened ambiguity band */
    cal_health_t health;
    bool frozen;
    float frozen_boundary;
    bool degraded_pending; /* set when entering COLLAPSED; consumer clears */
} calib_t;

typedef enum {
    CAL_OK = 0,
    CAL_ERR_COUNT,    /* wrong number of elements for PARIS */
    CAL_ERR_RANGE,    /* an element outside 20 ms..1.5 s */
    CAL_ERR_ELEMENTS, /* dots and dashes not separable */
    CAL_ERR_GAPS,     /* letter gaps not clearly longer than intra gaps */
} cal_err_t;

#define PARIS_ELEMENTS 14

void cal_reset(calib_t *c); /* uncalibrated, generic guesses */
bool cal_load(calib_t *c, const cal_profile_t *p);
const cal_profile_t *cal_profile(const calib_t *c);
bool cal_valid(const calib_t *c);

/* Bootstrap from the PARIS calibration word: 14 element durations and the 13
 * gaps between them, in keying order. */
cal_err_t cal_from_paris(calib_t *c, const float *el, int n_el, const float *gaps, int n_gaps);
const char *cal_err_str(cal_err_t e);

float cal_boundary(const calib_t *c);        /* dot/dash decision boundary, ms */
float cal_letter_threshold(const calib_t *c); /* silence that closes a character */
float cal_word_threshold(const calib_t *c);   /* silence that inserts a word space */
bool cal_is_dash(const calib_t *c, float ms);
bool cal_learnable(const calib_t *c, float ms); /* outside the ambiguity band */
float cal_dprime(const calib_t *c);
float cal_alpha(const calib_t *c);
cal_health_t cal_health(const calib_t *c);

/* Commit one dictionary-validated character: its elements (with labels and
 * learnable flags), the gaps inside it, and the letter gap that preceded it
 * (<= 0 if unknown). */
void cal_commit(calib_t *c, const float *el, const uint8_t *is_dash, const uint8_t *learn, int n,
                const float *intra, int n_intra, float letter_gap);

/* Undo the most recent committed character. With widen=true (error prosign)
 * the ambiguity band is also widened for the next 10 elements.
 * False if there was nothing to undo. */
bool cal_undo(calib_t *c, bool widen);

/* Keying resumed after a long idle: raise alpha for a few elements. */
void cal_idle_bump(calib_t *c);

/* True once per transition into COLLAPSED (play "keying degraded"). */
bool cal_take_degraded(calib_t *c);
