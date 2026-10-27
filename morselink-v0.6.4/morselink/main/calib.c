/*
 * MorseLink — adaptive keying calibration (see calib.h).
 */
#include "calib.h"

#include <math.h>
#include <string.h>

#include "morse.h"

#define DOT_MIN 30.f
#define DOT_MAX 500.f
#define DASH_MIN 60.f
#define DASH_MAX 1200.f
#define GI_MIN 15.f
#define GI_MAX 1500.f
#define GL_MIN 40.f
#define GL_MAX 4000.f

#define ELEM_MAD_FLOOR 0.04f   /* running floor, fraction of mean */
#define GAP_MAD_FLOOR 0.06f
#define PARIS_ELEM_FLOOR 0.08f /* floor on the spread measured during PARIS */
#define PARIS_GAP_FLOOR 0.10f

#define ALPHA_START 0.15f
#define ALPHA_STEADY 0.04f
#define ALPHA_DECAY_ELEMS 50.f
#define PHANTOM_IDLE 15

static inline float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static float mean_of(const float *v, int n)
{
    float s = 0;
    for (int i = 0; i < n; i++) {
        s += v[i];
    }
    return n ? s / (float)n : 0;
}

static float mad_of(const float *v, int n, float m)
{
    float s = 0;
    for (int i = 0; i < n; i++) {
        s += fabsf(v[i] - m);
    }
    return n ? s / (float)n : 0;
}

/* ------------------------------------------------------------------------- */

void cal_reset(calib_t *c)
{
    memset(c, 0, sizeof(*c));
    cal_profile_t *p = &c->s.p;
    p->version = CAL_PROFILE_VERSION;
    p->valid = 0;
    p->dot_mean = 120;
    p->dot_mad = 15;
    p->dash_mean = 360;
    p->dash_mad = 40;
    p->ratio = 3;
    p->gi_mean = 120;
    p->gi_mad = 20;
    p->gl_mean = 360;
    p->gl_mad = 60;
    p->last_good_boundary = sqrtf(p->dot_mean * p->dash_mean);
    c->s.bump_elems = 0xFFFF;
    c->health = CAL_NONE;
}

static bool profile_sane(const cal_profile_t *p)
{
    return p->version == CAL_PROFILE_VERSION && p->valid && p->dot_mean >= DOT_MIN &&
           p->dot_mean <= DOT_MAX && p->dash_mean > p->dot_mean && p->dash_mean <= DASH_MAX &&
           p->dot_mad > 0 && p->dash_mad > 0 && p->gi_mean > 0 && p->gl_mean > p->gi_mean &&
           p->gi_mad > 0 && p->gl_mad > 0 && isfinite(p->ratio) && isfinite(p->last_good_boundary);
}

static float boundary_live(const calib_t *c)
{
    const cal_profile_t *p = &c->s.p;
    float b;
    if (p->n_dot >= 20 && p->n_dash >= 20 && (p->dot_mad + p->dash_mad) > 1e-3f) {
        /* Spread-weighted: sits closer to whichever cluster is tighter. */
        b = (p->dot_mean * p->dash_mad + p->dash_mean * p->dot_mad) / (p->dot_mad + p->dash_mad);
    } else {
        /* Stable with almost no data. */
        b = sqrtf(p->dot_mean * p->dash_mean);
    }
    float lo = p->dot_mean * 1.15f, hi = p->dash_mean * 0.87f;
    return (lo < hi) ? clampf(b, lo, hi) : 0.5f * (p->dot_mean + p->dash_mean);
}

static void health_update(calib_t *c)
{
    cal_profile_t *p = &c->s.p;
    float d = cal_dprime(c);
    if (d >= 1.6f || (c->health == CAL_HEALTHY && d >= 1.5f)) {
        c->health = CAL_HEALTHY;
        c->frozen = false;
        p->last_good_boundary = boundary_live(c);
    } else if (d >= 1.0f) {
        if (!c->frozen) {
            c->frozen = true;
            c->frozen_boundary = p->last_good_boundary;
        }
        c->health = CAL_MARGINAL;
    } else {
        if (c->health != CAL_COLLAPSED) {
            c->degraded_pending = true;
        }
        c->health = CAL_COLLAPSED;
        c->frozen = true;
        c->frozen_boundary = p->last_good_boundary;
    }
}

bool cal_load(calib_t *c, const cal_profile_t *p)
{
    cal_reset(c);
    if (!p || !profile_sane(p)) {
        return false;
    }
    c->s.p = *p;
    c->health = CAL_HEALTHY;
    health_update(c);
    return true;
}

const cal_profile_t *cal_profile(const calib_t *c)
{
    return &c->s.p;
}

bool cal_valid(const calib_t *c)
{
    return c->s.p.valid != 0;
}

cal_health_t cal_health(const calib_t *c)
{
    return c->s.p.valid ? c->health : CAL_NONE;
}

const char *cal_err_str(cal_err_t e)
{
    switch (e) {
    case CAL_OK:
        return "ok";
    case CAL_ERR_COUNT:
        return "wrong element count (PARIS has 14)";
    case CAL_ERR_RANGE:
        return "element outside 20 ms..1.5 s";
    case CAL_ERR_ELEMENTS:
        return "dots and dashes overlap";
    case CAL_ERR_GAPS:
        return "letter gaps not longer than gaps inside letters";
    }
    return "?";
}

cal_err_t cal_from_paris(calib_t *c, const float *el, int n_el, const float *gaps, int n_gaps)
{
    /* Build the expected element labels and letter-gap positions from the
     * code table rather than hard-coding them. */
    uint8_t is_dash[PARIS_ELEMENTS];
    uint8_t letter_gap[PARIS_ELEMENTS - 1];
    int k = 0;
    memset(letter_gap, 0, sizeof(letter_gap));
    for (const char *w = "PARIS"; *w; w++) {
        const char *pat = morse_encode(*w);
        for (const char *e = pat; *e; e++) {
            is_dash[k++] = (*e == '-');
        }
        if (w[1]) {
            letter_gap[k - 1] = 1;
        }
    }
    if (n_el != PARIS_ELEMENTS || n_gaps != PARIS_ELEMENTS - 1 || k != PARIS_ELEMENTS) {
        return CAL_ERR_COUNT;
    }

    float dots[PARIS_ELEMENTS], dashes[PARIS_ELEMENTS], gi[PARIS_ELEMENTS], gl[PARIS_ELEMENTS];
    int nd = 0, nD = 0, ni = 0, nl = 0;
    float max_dot = 0, min_dash = 1e9f;
    for (int i = 0; i < PARIS_ELEMENTS; i++) {
        if (el[i] < 20.f || el[i] >= 1500.f) {
            return CAL_ERR_RANGE;
        }
        if (is_dash[i]) {
            dashes[nD++] = el[i];
            if (el[i] < min_dash) {
                min_dash = el[i];
            }
        } else {
            dots[nd++] = el[i];
            if (el[i] > max_dot) {
                max_dot = el[i];
            }
        }
    }
    for (int i = 0; i < PARIS_ELEMENTS - 1; i++) {
        if (letter_gap[i]) {
            gl[nl++] = gaps[i];
        } else {
            gi[ni++] = gaps[i];
        }
    }

    cal_profile_t p;
    memset(&p, 0, sizeof(p));
    p.version = CAL_PROFILE_VERSION;
    p.dot_mean = mean_of(dots, nd);
    p.dash_mean = mean_of(dashes, nD);
    p.dot_mad = fmaxf(mad_of(dots, nd, p.dot_mean), PARIS_ELEM_FLOOR * p.dot_mean);
    p.dash_mad = fmaxf(mad_of(dashes, nD, p.dash_mean), PARIS_ELEM_FLOOR * p.dash_mean);
    p.gi_mean = mean_of(gi, ni);
    p.gl_mean = mean_of(gl, nl);
    p.gi_mad = fmaxf(mad_of(gi, ni, p.gi_mean), PARIS_GAP_FLOOR * p.gi_mean);
    p.gl_mad = fmaxf(mad_of(gl, nl, p.gl_mean), PARIS_GAP_FLOOR * p.gl_mean);

    if (p.dot_mean < DOT_MIN || p.dot_mean > DOT_MAX || p.dash_mean < DASH_MIN || p.dash_mean > DASH_MAX) {
        return CAL_ERR_RANGE;
    }
    p.ratio = p.dash_mean / p.dot_mean;
    float dprime = (p.dash_mean - p.dot_mean) / (1.25f * (p.dot_mad + p.dash_mad));
    /* The profile we are about to create must classify its own calibration
     * word correctly at the starting (geometric-mean) boundary. */
    float b0 = sqrtf(p.dot_mean * p.dash_mean);
    if (!(max_dot < b0 && min_dash >= b0) || p.ratio < 1.8f || dprime < 1.5f) {
        return CAL_ERR_ELEMENTS;
    }
    float min_gl = 1e9f;
    for (int i = 0; i < nl; i++) {
        if (gl[i] < min_gl) {
            min_gl = gl[i];
        }
    }
    if (p.gl_mean < 1.5f * p.gi_mean || min_gl <= p.gi_mean) {
        return CAL_ERR_GAPS;
    }

    p.valid = 1;
    p.n_dot = (uint32_t)nd;
    p.n_dash = (uint32_t)nD;
    p.n_conf = 0;

    cal_reset(c);
    c->s.p = p;
    c->s.p.last_good_boundary = boundary_live(c);
    c->health = CAL_HEALTHY;
    health_update(c);
    return CAL_OK;
}

/* ------------------------------------------------------------------------- */

float cal_boundary(const calib_t *c)
{
    return c->frozen ? c->frozen_boundary : boundary_live(c);
}

float cal_letter_threshold(const calib_t *c)
{
    const cal_profile_t *p = &c->s.p;
    /* Midpoint (geometric) between the two gap clusters, kept above the
     * jitter tail of intra gaps and below typical letter gaps... */
    float g = sqrtf(p->gi_mean * p->gl_mean);
    float lo = p->gi_mean + 3.f * p->gi_mad;
    float hi = 0.75f * p->gl_mean;
    float t = (lo <= hi) ? clampf(g, lo, hi) : 0.5f * (lo + hi);
    /* ...and anchored to the element timing, so mislabelled gaps can never
     * drag it into a runaway. */
    return clampf(t, 1.5f * p->dot_mean, 4.0f * p->dot_mean);
}

float cal_word_threshold(const calib_t *c)
{
    /* Spec: 2 x letter gap. That sits at 6 units for standard spacing, so ~8 %
     * of 7-unit word gaps are missed by a consistent keyer. A consistent
     * keyer's letter gaps are tight, so use "well beyond the letter-gap
     * spread" and keep 2 x letter gap as the ceiling for keyers whose letter
     * gaps vary a lot (thinking pauses). */
    const cal_profile_t *p = &c->s.p;
    float l = cal_letter_threshold(c);
    float w = fminf(p->gl_mean + 4.5f * p->gl_mad, 2.f * p->gl_mean);
    return clampf(w, 1.6f * l, 5000.f);
}

bool cal_is_dash(const calib_t *c, float ms)
{
    return ms >= cal_boundary(c);
}

bool cal_learnable(const calib_t *c, float ms)
{
    /* The handover used a fixed +/-15 % band around the boundary. With the
     * spread-weighted boundary sitting close to the tighter (dot) cluster, that
     * band cuts off the upper tail of the dots, biases dot_mean low, and stops
     * it following a user who slows down (tests: 30 % errors). Instead a
     * sample is learnable when it is clearly not a member of the *other*
     * cluster (z-score), plus a small absolute band at the boundary itself. */
    const cal_profile_t *p = &c->s.p;
    float b = cal_boundary(c);
    bool wide = c->widen_left > 0;
    float band = (wide ? 0.10f : 0.05f) * (p->dash_mean - p->dot_mean);
    if (fabsf(ms - b) <= band) {
        return false;
    }
    float z = wide ? 3.0f : 2.0f;
    if (ms >= b) {
        return (ms - p->dot_mean) > z * 1.25f * p->dot_mad;
    }
    return (p->dash_mean - ms) > z * 1.25f * p->dash_mad;
}

float cal_dprime(const calib_t *c)
{
    const cal_profile_t *p = &c->s.p;
    float den = 1.25f * (p->dot_mad + p->dash_mad);
    return den > 1e-3f ? (p->dash_mean - p->dot_mean) / den : 99.f;
}

float cal_alpha(const calib_t *c)
{
    const cal_state_t *s = &c->s;
    float a = ALPHA_STEADY + (ALPHA_START - ALPHA_STEADY) * fmaxf(0.f, 1.f - (float)s->p.n_conf / ALPHA_DECAY_ELEMS);
    if (s->bump_elems != 0xFFFF) {
        float b = (s->bump_elems < 10) ? ALPHA_START
                                       : ALPHA_START - (float)(s->bump_elems - 10) * ((ALPHA_START - ALPHA_STEADY) / 20.f);
        if (b > a) {
            a = b;
        }
    }
    return a;
}

static void upd(float *mean, float *mad, float x, float a, float lo, float hi, float floor_frac)
{
    float delta = x - *mean;
    float cap = 0.15f * *mean; /* no sudden jumps */
    *mean += a * clampf(delta, -cap, cap);
    float dev = fabsf(delta);
    float lim = 3.f * *mad; /* one outlier can't blow up the spread */
    if (dev > lim) {
        dev = lim;
    }
    *mad += a * (dev - *mad);
    *mean = clampf(*mean, lo, hi);
    float fl = floor_frac * *mean;
    if (*mad < fl) {
        *mad = fl;
    }
}

/* One class unobserved for a while: follow the active class at quarter
 * weight, anchored on the measured ratio, so a long run of one element type
 * at a new speed doesn't strand the boundary. */
static void phantom(cal_state_t *s, bool updated_dash, float a)
{
    cal_profile_t *p = &s->p;
    if (updated_dash && s->since_dot > PHANTOM_IDLE) {
        float target = p->dash_mean / p->ratio;
        p->dot_mean = clampf(p->dot_mean + 0.25f * a * (target - p->dot_mean), DOT_MIN, DOT_MAX);
    } else if (!updated_dash && s->since_dash > PHANTOM_IDLE) {
        float target = p->dot_mean * p->ratio;
        p->dash_mean = clampf(p->dash_mean + 0.25f * a * (target - p->dash_mean), DASH_MIN, DASH_MAX);
    }
}

void cal_commit(calib_t *c, const float *el, const uint8_t *is_dash, const uint8_t *learn, int n,
                const float *intra, int n_intra, float letter_gap)
{
    if (!c->s.p.valid) {
        return;
    }
    c->snap[c->snap_head] = c->s;
    c->snap_head = (uint8_t)((c->snap_head + 1) % CAL_SNAPSHOTS);
    if (c->snap_count < CAL_SNAPSHOTS) {
        c->snap_count++;
    }

    cal_state_t *s = &c->s;
    cal_profile_t *p = &s->p;
    /* Gap thresholds as they were when this group was segmented. */
    float lthr = cal_letter_threshold(c);
    float wthr = cal_word_threshold(c);

    for (int i = 0; i < n; i++) {
        bool d = is_dash[i] != 0;
        if (d) {
            s->since_dash = 0;
            if (s->since_dot < 60000) {
                s->since_dot++;
            }
        } else {
            s->since_dot = 0;
            if (s->since_dash < 60000) {
                s->since_dash++;
            }
        }
        if (!learn[i]) {
            continue;
        }
        float a = cal_alpha(c);
        if (d) {
            upd(&p->dash_mean, &p->dash_mad, el[i], a, DASH_MIN, DASH_MAX, ELEM_MAD_FLOOR);
            p->n_dash++;
        } else {
            upd(&p->dot_mean, &p->dot_mad, el[i], a, DOT_MIN, DOT_MAX, ELEM_MAD_FLOOR);
            p->n_dot++;
        }
        p->n_conf++;
        if (s->bump_elems != 0xFFFF && ++s->bump_elems > 30) {
            s->bump_elems = 0xFFFF;
        }
        phantom(s, d, a);
        if (s->since_dot <= PHANTOM_IDLE && s->since_dash <= PHANTOM_IDLE && p->dot_mean > 1.f) {
            p->ratio += 0.05f * (p->dash_mean / p->dot_mean - p->ratio);
        }
    }

    float a = cal_alpha(c);
    /* Gaps inside a validated character: learn unless close to the threshold
     * (a merged pair of letters would put a short letter gap here). */
    for (int i = 0; i < n_intra; i++) {
        if (intra[i] > 0 && intra[i] < 0.85f * lthr) {
            upd(&p->gi_mean, &p->gi_mad, intra[i], a, GI_MIN, GI_MAX, GAP_MAD_FLOOR);
        }
    }
    /* Gap between two validated characters: learn when it is clearly not an
     * intra gap (z-score against the tight intra cluster). A fixed band above
     * the threshold would stop gl_mean following a user who speeds up. */
    if (letter_gap > 0 && letter_gap < wthr && (letter_gap - p->gi_mean) > 2.5f * 1.25f * p->gi_mad &&
        letter_gap > 1.05f * lthr) {
        upd(&p->gl_mean, &p->gl_mad, letter_gap, a, GL_MIN, GL_MAX, GAP_MAD_FLOOR);
    }
    if (p->gl_mean < 1.3f * p->gi_mean) {
        p->gl_mean = 1.3f * p->gi_mean; /* keep the clusters ordered */
    }

    c->widen_left = (c->widen_left > n) ? (uint8_t)(c->widen_left - n) : 0;
    health_update(c);
}

bool cal_undo(calib_t *c, bool widen)
{
    if (widen) {
        c->widen_left = 10;
    }
    if (c->snap_count == 0) {
        return false;
    }
    c->snap_head = (uint8_t)((c->snap_head + CAL_SNAPSHOTS - 1) % CAL_SNAPSHOTS);
    c->snap_count--;
    c->s = c->snap[c->snap_head];
    health_update(c);
    return true;
}

void cal_idle_bump(calib_t *c)
{
    c->s.bump_elems = 0;
}

bool cal_take_degraded(calib_t *c)
{
    bool d = c->degraded_pending;
    c->degraded_pending = false;
    return d;
}
