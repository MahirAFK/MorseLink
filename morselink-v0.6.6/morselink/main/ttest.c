/*
 * MorseLink — test traffic (see ttest.h).
 */
#include "ttest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h" /* RADIO_SF_MIN / MAX, RADIO_TX_DBM_MIN / MAX: what a plan may ask for */

static const char *const TEST_NAMES[TT__TESTS] = {"baseline", "dynamic", "rundown-a", "rundown-b", "rundown-m",
                                                   "rundown-l", "point"};

/* The point test's default plan (agreed 20 Oct): SF9 at 17 dBm (the setting
 * of the earlier field tests), SF9 at 22 dBm (what the power alone gains),
 * SF12 at 22 dBm (the most range). */
static const tt_step_t DEFAULT_PLAN[] = {{9, 17}, {9, 22}, {12, 22}};
#define DEFAULT_PLAN_N ((int)(sizeof DEFAULT_PLAN / sizeof DEFAULT_PLAN[0]))
static const char *const PATH_NAMES[TT__PATHS] = {"both", "lora", "relay"};
static const char *const ROLE_NAMES[TT__ROLES] = {"rover", "base"};

static const char FILLER[] = "PARIS "; /* repeated after the marker */
#define FILLER_LEN 6
static const char B36[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

/* tt_test_name() / tt_paths_name() / tt_role_name() -- see ttest.h. */
const char *tt_test_name(int test)
{
    return test >= 0 && test < TT__TESTS ? TEST_NAMES[test] : "?";
}

const char *tt_paths_name(int paths)
{
    return paths >= 0 && paths < TT__PATHS ? PATH_NAMES[paths] : "?";
}

const char *tt_role_name(int role)
{
    return role >= 0 && role < TT__ROLES ? ROLE_NAMES[role] : "?";
}

/* find_name() -- the index of `v` (n characters, not NUL-terminated) in
 * names[0..count), or -1. */
static int find_name(const char *const *names, int count, const char *v, size_t n)
{
    for (int i = 0; i < count; i++) {
        if (strlen(names[i]) == n && strncmp(names[i], v, n) == 0) {
            return i;
        }
    }
    return -1;
}

/* parse_uint() -- v[0..n) as a decimal number up to `max`; false if it
 * isn't one (empty, a stray character, too big). */
static bool parse_uint(const char *v, size_t n, uint32_t max, uint32_t *out)
{
    uint64_t x = 0;
    if (n == 0 || n > 10) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return false;
        }
        x = x * 10 + (uint64_t)(v[i] - '0');
    }
    if (x > max) {
        return false;
    }
    *out = (uint32_t)x;
    return true;
}

/*
 * parse_plan() -- "9:17,9:22,12:22" (n characters) into p->plan: each step
 * sf:dBm, separated by commas (or '/', as the T lines write it), SF RADIO_SF_MIN..MAX and power
 * RADIO_TX_DBM_MIN..MAX (a board whose chip can do less clamps it, and its
 * STEP line says what it used), at most TT_PLAN_MAX steps. False if
 * anything doesn't fit.
 */
static bool parse_plan(const char *v, size_t n, tt_params_t *p)
{
    p->n_steps = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i; /* one step: v[i..j) */
        while (j < n && v[j] != ',' && v[j] != '/') {
            j++;
        }
        const char *colon = memchr(v + i, ':', j - i);
        uint32_t sf, dbm;
        if (!colon || p->n_steps >= TT_PLAN_MAX || !parse_uint(v + i, (size_t)(colon - (v + i)), 99, &sf) ||
            !parse_uint(colon + 1, (size_t)(v + j - colon - 1), 99, &dbm) || sf < RADIO_SF_MIN || sf > RADIO_SF_MAX ||
            dbm < RADIO_TX_DBM_MIN || dbm > RADIO_TX_DBM_MAX) {
            return false;
        }
        p->plan[p->n_steps].sf = (uint8_t)sf;
        p->plan[p->n_steps].dbm = (int8_t)dbm;
        p->n_steps++;
        if (j + 1 == n) {
            return false; /* a comma at the end: a step is missing */
        }
        i = j + 1; /* past the comma (or the end) */
    }
    return p->n_steps > 0;
}

/*
 * tt_parse_params() -- see ttest.h. Walks the words one by one; each must
 * be key=value. The session name is copied (cut at TT_SESSION_MAX); the
 * seed is a decimal number up to 2^32 - 1. A point test gets the default
 * plan if none was given, and LoRa only whatever `paths` said.
 */
bool tt_parse_params(const char *s, tt_params_t *p, const char **why)
{
    memset(p, 0, sizeof(*p));
    p->test = 0xFF; /* not given yet */
    *why = NULL;
    while (s && *s) {
        while (*s == ' ') {
            s++;
        }
        if (!*s) {
            break;
        }
        const char *w = s; /* the word: w .. s */
        while (*s && *s != ' ') {
            s++;
        }
        const char *eq = memchr(w, '=', (size_t)(s - w));
        if (!eq) {
            *why = "not key=value";
            return false;
        }
        size_t klen = (size_t)(eq - w), vlen = (size_t)(s - eq - 1);
        const char *v = eq + 1;
        if (klen == 4 && strncmp(w, "test", 4) == 0) {
            int t = find_name(TEST_NAMES, TT__TESTS, v, vlen);
            if (t < 0) {
                *why = "unknown test";
                return false;
            }
            p->test = (uint8_t)t;
        } else if (klen == 5 && strncmp(w, "paths", 5) == 0) {
            int t = find_name(PATH_NAMES, TT__PATHS, v, vlen);
            if (t < 0) {
                *why = "unknown paths";
                return false;
            }
            p->paths = (uint8_t)t;
        } else if (klen == 4 && strncmp(w, "role", 4) == 0) {
            int t = find_name(ROLE_NAMES, TT__ROLES, v, vlen);
            if (t < 0) {
                *why = "unknown role";
                return false;
            }
            p->role = (uint8_t)t;
        } else if (klen == 7 && strncmp(w, "session", 7) == 0) {
            size_t n = vlen > TT_SESSION_MAX ? TT_SESSION_MAX : vlen;
            memcpy(p->session, v, n);
            p->session[n] = '\0';
        } else if (klen == 4 && strncmp(w, "seed", 4) == 0) {
            if (!parse_uint(v, vlen, 0xFFFFFFFFu, &p->seed)) {
                *why = "bad seed";
                return false;
            }
        } else if (klen == 4 && strncmp(w, "plan", 4) == 0) {
            if (!parse_plan(v, vlen, p)) {
                *why = "bad plan";
                return false;
            }
        } else if (klen == 1 && w[0] == 'n') {
            uint32_t k;
            if (!parse_uint(v, vlen, TT_POINT_N, &k) || k == 0) {
                *why = "bad n";
                return false;
            }
            p->n = (uint8_t)k;
        }
        /* any other key (size=, label=, ...): ignored */
    }
    if (p->test == 0xFF) {
        *why = "no test given";
        return false;
    }
    if (p->test == TT_POINT) {
        if (p->n_steps == 0) {
            memcpy(p->plan, DEFAULT_PLAN, sizeof DEFAULT_PLAN);
            p->n_steps = (uint8_t)DEFAULT_PLAN_N;
        }
        p->paths = TT_LORA; /* the relay plays no part in a point test */
    }
    return true;
}

/* tt_plan_text() -- see ttest.h. */
const char *tt_plan_text(const tt_params_t *p, char sep, char *out, int size)
{
    int k = 0;
    const char sp[2] = {sep, '\0'};
    out[0] = '\0';
    for (int i = 0; i < p->n_steps && k < size - 1; i++) {
        int w = snprintf(out + k, (size_t)(size - k), "%s%u:%d", i ? sp : "", p->plan[i].sf, p->plan[i].dbm);
        if (w < 0 || w >= size - k) {
            break; /* cut short: what fitted stays (snprintf NUL-terminated it) */
        }
        k += w;
    }
    return out;
}

/* tt_sends() / tt_is_rundown() / tt_plays() / tt_ble_off() /
 * tt_uses_lora() / tt_uses_relay() -- see ttest.h. */
bool tt_sends(int test)
{
    return test == TT_BASELINE || test == TT_DYNAMIC || test == TT_RUNDOWN_B || test == TT_RUNDOWN_L ||
           test == TT_POINT;
}

bool tt_is_rundown(int test)
{
    return test == TT_RUNDOWN_A || test == TT_RUNDOWN_B || test == TT_RUNDOWN_M || test == TT_RUNDOWN_L;
}

bool tt_plays(int test)
{
    return test == TT_RUNDOWN_M;
}

bool tt_ble_off(int test)
{
    return test == TT_RUNDOWN_A || test == TT_RUNDOWN_M || test == TT_RUNDOWN_L;
}

/* sf_index() -- 0..3 for SF9..SF12 (0 and anything else count as SF9). */
static int sf_index(int sf)
{
    return sf >= 10 && sf <= 12 ? sf - 9 : 0;
}

/* tt_dyn_gap() -- see ttest.h: 10-20 / 20-40 / 40-60 / 90-130 s. */
void tt_dyn_gap(int sf, int32_t *min_ms, int32_t *max_ms)
{
    static const int32_t lo[4] = {TT_DYN_GAP_MIN_MS, 20000, 40000, 90000};
    static const int32_t hi[4] = {TT_DYN_GAP_MAX_MS, 40000, 60000, 130000};
    *min_ms = lo[sf_index(sf)];
    *max_ms = hi[sf_index(sf)];
}

/* tt_baseline_gap_ms() -- see ttest.h: 20 / 30 / 45 / 90 s. */
int32_t tt_baseline_gap_ms(int sf)
{
    static const int32_t g[4] = {TT_BASELINE_GAP_MS, 30000, 45000, 90000};
    return g[sf_index(sf)];
}

/* tt_baseline_n() -- see ttest.h: 120, or 60 at SF12 (equal numbers of each
 * size either way). */
int tt_baseline_n(int sf)
{
    return sf_index(sf) == 3 ? TT_BASELINE_N / 2 : TT_BASELINE_N;
}

/* tt_point_gap_ms() -- see ttest.h: longer than a 25-character message takes
 * to fail at that SF (the host tests check it against link_timing). */
int32_t tt_point_gap_ms(int sf)
{
    static const int32_t g[4] = {15000, 20000, 25000, 40000};
    return g[sf_index(sf)];
}

/* tt_point_settle_ms() -- see ttest.h. */
int32_t tt_point_settle_ms(int sf)
{
    return sf_index(sf) >= 2 ? 20000 : 10000;
}

/* tt_point_n() -- see ttest.h. */
int tt_point_n(const tt_params_t *p)
{
    return p->n >= 1 && p->n <= TT_POINT_N ? p->n : TT_POINT_N;
}

/* tt_point_step_ms() -- see ttest.h: settle, n gaps of sending, one gap of tail. */
int32_t tt_point_step_ms(int sf, int n)
{
    if (n < 1 || n > TT_POINT_N) {
        n = TT_POINT_N;
    }
    return tt_point_settle_ms(sf) + (n + 1) * tt_point_gap_ms(sf);
}

/* tt_point_offset_ms() -- see ttest.h: the lengths of the steps before i, added up. */
int64_t tt_point_offset_ms(const tt_params_t *p, int i)
{
    int64_t t = 0;
    for (int k = 0; k < i && k < p->n_steps; k++) {
        t += tt_point_step_ms(p->plan[k].sf, tt_point_n(p));
    }
    return t;
}

/* tt_point_step_at() -- see ttest.h. Before the START counts as step 0. */
int tt_point_step_at(const tt_params_t *p, int64_t since_ms)
{
    int i = 0;
    while (i < p->n_steps && since_ms >= tt_point_offset_ms(p, i + 1)) {
        i++;
    }
    return i;
}

/* point_due() -- when message k of step i (both 0-based) is due for this
 * board: the step's start, its settle time, k gaps, and half a gap more for
 * the base. */
static int64_t point_due(const tt_sched_t *s, int i, int k)
{
    const int sf = s->p.plan[i].sf;
    const int32_t gap = tt_point_gap_ms(sf);
    return s->start_ms + tt_point_offset_ms(&s->p, i) + tt_point_settle_ms(sf) + (int64_t)k * gap +
           (s->p.role == TT_BASE ? gap / 2 : 0);
}

bool tt_uses_lora(int paths)
{
    return paths == TT_BOTH || paths == TT_LORA;
}

bool tt_uses_relay(int paths)
{
    return paths == TT_BOTH || paths == TT_RELAY;
}

/* rnd() -- the next xorshift32 number (never 0 while the state isn't 0). */
static uint32_t rnd(tt_sched_t *s)
{
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return x;
}

/* rnd_range() -- uniform in lo..hi (inclusive; the tiny modulo bias is
 * irrelevant here). */
static int rnd_range(tt_sched_t *s, int lo, int hi)
{
    return lo + (int)(rnd(s) % (uint32_t)(hi - lo + 1));
}

/*
 * tt_init() -- see ttest.h. The random state comes from the seed mixed with
 * the role, so the two boards of one session make different (but each
 * reproducible) sequences. Baseline: equal numbers of each size (30 each;
 * 15 at SF12), shuffled (Fisher-Yates). The first message is due 5 s after
 * the start (rover), or half a baseline grid later (base: 15 s at SF9);
 * rundown-a and rundown-m never send.
 */
void tt_init(tt_sched_t *s, const tt_params_t *p, int64_t start_ms)
{
    memset(s, 0, sizeof(*s));
    s->p = *p;
    s->rng = p->seed ^ (p->role == TT_BASE ? 0x9E3779B9u : 0x85EBCA6Bu);
    if (s->rng == 0) {
        s->rng = 1;
    }
    for (int i = 0; i < 10; i++) {
        rnd(s); /* stir: nearby seeds shouldn't start alike */
    }
    const int n = tt_baseline_n(p->sf);
    if (p->test == TT_BASELINE) {
        static const uint8_t sz[4] = {5, 25, 50, 100};
        for (int i = 0; i < n; i++) {
            s->sizes[i] = sz[i / (n / 4)];
        }
        for (int i = n - 1; i > 0; i--) {
            int j = (int)(rnd(s) % (uint32_t)(i + 1));
            uint8_t t = s->sizes[i];
            s->sizes[i] = s->sizes[j];
            s->sizes[j] = t;
        }
    }
    const int64_t base_first = TT_ROVER_FIRST_MS + tt_baseline_gap_ms(p->sf) / 2; /* 15 s at SF9 */
    s->next_ms = tt_sends(p->test) ? start_ms + (p->role == TT_BASE ? base_first : TT_ROVER_FIRST_MS) : INT64_MAX;
    s->start_ms = start_ms;
    s->step = 0;
    s->k = 0;
    if (p->test == TT_POINT) {
        s->next_ms = p->n_steps ? point_due(s, 0, 0) : INT64_MAX; /* the first step's first message */
    }
}

/* tt_next() -- see ttest.h. Baseline keeps a fixed grid (20 s at SF9);
 * the others draw each gap anew (10-20 s at SF9), counted from when the
 * message was due; a point test follows its timetable (point_due), each
 * message TT_POINT_SIZE characters, and remembers which step it belongs to
 * (s->step). */
bool tt_next(tt_sched_t *s, int64_t now, uint16_t *seq, int *size)
{
    if (s->next_ms == INT64_MAX || now < s->next_ms) {
        return false;
    }
    s->made++;
    *seq = (uint16_t)(s->made % TT_SEQ_MOD);
    if (s->p.test == TT_POINT) {
        const int n = tt_point_n(&s->p);
        int i = (s->made - 1) / n; /* this message's step (0-based) */
        *size = TT_POINT_SIZE;
        s->step = i + 1;
        s->k = (s->made - 1) % n + 1; /* the next one's number within its step */
        if (s->k >= n) {
            s->k = 0;
            i++; /* the next step's first message */
        }
        s->next_ms = i < s->p.n_steps ? point_due(s, i, s->k) : INT64_MAX;
        return true;
    }
    if (s->p.test == TT_BASELINE) {
        *size = s->sizes[s->made - 1];
        s->next_ms = s->made < tt_baseline_n(s->p.sf) ? s->next_ms + tt_baseline_gap_ms(s->p.sf) : INT64_MAX;
    } else {
        int32_t lo, hi;
        tt_dyn_gap(s->p.sf, &lo, &hi);
        *size = rnd_range(s, TT_SIZE_MIN, TT_SIZE_MAX);
        s->next_ms += rnd_range(s, lo, hi);
    }
    return true;
}

/* tt_finished() -- see ttest.h. */
bool tt_finished(const tt_sched_t *s)
{
    return s->p.test == TT_BASELINE && s->made >= tt_baseline_n(s->p.sf);
}

/* tt_text() -- see ttest.h. */
int tt_text(char *out, int out_size, uint16_t seq, char path, int len)
{
    if (len < TT_SIZE_MIN || len > TT_SIZE_MAX || out_size < len + 1 || (path != 'L' && path != 'R')) {
        return -1;
    }
    unsigned v = seq % TT_SEQ_MOD;
    out[0] = B36[v / 1296];
    out[1] = B36[(v / 36) % 36];
    out[2] = B36[v % 36];
    out[3] = path;
    for (int i = TT_MARKER_LEN; i < len; i++) {
        out[i] = FILLER[(i - TT_MARKER_LEN) % FILLER_LEN];
    }
    out[len] = '\0';
    return len;
}

/* b36() -- the value of a base-36 digit (0-9, A-Z), or -1. */
static int b36(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return -1;
}

/* tt_parse() -- see ttest.h. */
bool tt_parse(const char *text, int len, uint16_t *seq, char *path)
{
    if (len < TT_SIZE_MIN || len > TT_SIZE_MAX) {
        return false;
    }
    int d0 = b36(text[0]), d1 = b36(text[1]), d2 = b36(text[2]);
    if (d0 < 0 || d1 < 0 || d2 < 0 || (text[3] != 'L' && text[3] != 'R')) {
        return false;
    }
    for (int i = TT_MARKER_LEN; i < len; i++) {
        if (text[i] != FILLER[(i - TT_MARKER_LEN) % FILLER_LEN]) {
            return false;
        }
    }
    *seq = (uint16_t)(d0 * 1296 + d1 * 36 + d2);
    *path = text[3];
    return true;
}
