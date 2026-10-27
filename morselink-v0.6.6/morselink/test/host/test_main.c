/*
 * MorseLink — host-side tests for the pure logic (Morse table, IDs, haptic
 * sequences, calibration, decoder) using a simulated human "fist", plus the
 * packet layer and the link protocol (test_link.c) using simulated radios.
 *
 *   make -C test/host && ./test/host/run_tests [-v | -vv]
 *   DBG_LINK=1 ./test/host/run_tests      prints every link event as it happens
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calib.h"
#include "config.h"
#include "convo.h"
#include "decoder.h"
#include "hseq.h"
#include "morse.h"

int g_fail, g_pass; /* shared with test_link.c */
static int g_verbose;

void test_link_all(void);  /* test_link.c */
void test_phone_all(void); /* test_phone.c */
void test_ttest_all(void); /* test_ttest.c */

#define CHECK(cond, ...)                                                                                       \
    do {                                                                                                       \
        if (cond) {                                                                                            \
            g_pass++;                                                                                          \
        } else {                                                                                               \
            g_fail++;                                                                                          \
            printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);                                         \
            printf(__VA_ARGS__);                                                                               \
            printf("\n");                                                                                      \
        }                                                                                                      \
    } while (0)

/* ------------------------------------------------------------------------- */
/* Deterministic RNG + Gaussian                                               */
/* ------------------------------------------------------------------------- */

static uint64_t g_rng = 88172645463325252ull;
static void rng_seed(uint64_t s)
{
    g_rng = s ? s : 1;
}
static double rng_u(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (double)(g_rng >> 11) / 9007199254740992.0;
}
static double rng_gauss(void)
{
    double u1 = rng_u(), u2 = rng_u();
    if (u1 < 1e-12) {
        u1 = 1e-12;
    }
    return sqrt(-2.0 * log(u1)) * cos(2 * M_PI * u2);
}

/* ------------------------------------------------------------------------- */
/* Simulated fist                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    double dot_ms;
    double ratio;
    double intra_x, letter_x, word_x; /* gaps as multiples of the dot */
    double jitter;                    /* relative sd per duration */
    double think_prob, think_x;       /* occasional thinking pause at letter gaps */
    double drift;                     /* dot_ms multiplier per character */
} fist_t;

typedef struct {
    int64_t down, up;
} press_t;

#define MAX_PRESSES 20000
static press_t g_press[MAX_PRESSES];
static int g_np;

static double jit(const fist_t *f, double v)
{
    double x = v * (1.0 + f->jitter * rng_gauss());
    return x < 25 ? 25 : x;
}

/* Append the presses for `text` starting at time *t (ms). */
static void sim_text(fist_t *f, const char *text, int64_t *t)
{
    bool first = true;
    bool space = false;
    for (const char *p = text; *p; p++) {
        if (*p == ' ') {
            space = true;
            continue;
        }
        const char *pat = morse_encode(*p);
        if (!pat) {
            continue;
        }
        if (!first) {
            double g = space ? f->word_x : f->letter_x;
            if (!space && rng_u() < f->think_prob) {
                g += f->think_x * (0.5 + rng_u());
            }
            *t += (int64_t)jit(f, g * f->dot_ms);
        }
        first = false;
        space = false;
        for (int i = 0; pat[i]; i++) {
            if (i) {
                *t += (int64_t)jit(f, f->intra_x * f->dot_ms);
            }
            double len = (pat[i] == '-') ? f->ratio * f->dot_ms : f->dot_ms;
            int64_t d = (int64_t)jit(f, len);
            g_press[g_np].down = *t;
            g_press[g_np].up = *t + d;
            g_np++;
            *t += d;
        }
        f->dot_ms *= f->drift;
    }
}

/* Raw pattern string -> presses (for prosigns / SOS shapes). */
static void sim_pattern(fist_t *f, const char *pat, int64_t *t, bool lead_gap)
{
    for (int i = 0; pat[i]; i++) {
        if (i || lead_gap) {
            *t += (int64_t)jit(f, (i ? f->intra_x : f->letter_x) * f->dot_ms);
        }
        double len = (pat[i] == '-') ? f->ratio * f->dot_ms : f->dot_ms;
        int64_t d = (int64_t)jit(f, len);
        g_press[g_np].down = *t;
        g_press[g_np].up = *t + d;
        g_np++;
        *t += d;
    }
}

/* PARIS calibration keyed by this fist. */
static cal_err_t sim_paris(calib_t *c, fist_t *f)
{
    g_np = 0;
    int64_t t = 0;
    sim_text(f, "PARIS", &t);
    float el[PARIS_ELEMENTS], gaps[PARIS_ELEMENTS];
    for (int i = 0; i < g_np && i < PARIS_ELEMENTS; i++) {
        el[i] = (float)(g_press[i].up - g_press[i].down);
        if (i) {
            gaps[i - 1] = (float)(g_press[i].down - g_press[i - 1].up);
        }
    }
    cal_err_t e = cal_from_paris(c, el, g_np, gaps, g_np - 1);
    g_np = 0;
    return e;
}

/* Feed the presses through the decoder, polling at deadlines exactly as the
 * device's UI loop does. Builds the decoded text the way a draft would be. */
typedef struct {
    char text[8192];
    int len;
    int n_sos, n_err, n_unknown;
    int sos_undo;
} decode_out_t;

static void apply_evt(decode_out_t *o, const dec_evt_t *e)
{
    switch (e->kind) {
    case DEC_CHAR:
    case DEC_UNKNOWN:
        if (e->kind == DEC_UNKNOWN) {
            o->n_unknown++;
        }
        if (o->len < (int)sizeof(o->text) - 1) {
            o->text[o->len++] = e->c;
        }
        break;
    case DEC_SPACE:
        if (o->len && o->text[o->len - 1] != ' ' && o->len < (int)sizeof(o->text) - 1) {
            o->text[o->len++] = ' ';
        }
        break;
    case DEC_ERROR:
        o->n_err++;
        if (o->len) {
            o->len--;
        }
        break;
    case DEC_SOS:
        if (getenv("DBG_SOS")) {
            printf("    SOS after '%.40s' (undo %d) pattern %s els:", o->len > 40 ? o->text + o->len - 40 : o->text,
                   e->sos_undo, e->pattern);
            for (int i = 0; i < e->n_el; i++) {
                printf(" %d", e->el_ms[i]);
            }
            printf("\n");
        }
        o->n_sos++;
        o->sos_undo = e->sos_undo;
        for (int k = 0; k < e->sos_undo && o->len; k++) {
            o->len--;
        }
        while (o->len && o->text[o->len - 1] == ' ') {
            o->len--;
        }
        break;
    }
    o->text[o->len] = '\0';
    if (g_verbose && e->kind != DEC_SPACE) {
        printf("    [%c] %-10s", e->c ? e->c : '#', e->pattern);
        for (int i = 0; i < e->n_el; i++) {
            printf(" %d", e->el_ms[i]);
        }
        printf("\n");
    }
}

static void drain(decoder_t *d, decode_out_t *o)
{
    dec_evt_t e;
    while (dec_get(d, &e)) {
        apply_evt(o, &e);
    }
}

static void run_decoder(decoder_t *d, decode_out_t *o)
{
    memset(o, 0, sizeof(*o));
    for (int i = 0; i < g_np; i++) {
        for (;;) {
            int64_t dl = dec_deadline(d);
            if (dl > g_press[i].down) {
                break;
            }
            dec_poll(d, dl);
            drain(d, o);
        }
        int64_t dur = g_press[i].up - g_press[i].down;
        dec_key_down(d, g_press[i].down);
        drain(d, o);
        /* The device polls on a timer while the key is held too: nothing
         * may time out mid-press. */
        for (int64_t tp = g_press[i].down + 50; tp < g_press[i].up; tp += 50) {
            dec_poll(d, tp);
        }
        CHECK(dec_deadline(d) == INT64_MAX, "no deadline while the key is down");
        drain(d, o);
        if (dur >= 1500) {
            dec_flush(d); /* the UI treats this as a hold */
        } else {
            dec_element(d, g_press[i].down, g_press[i].up);
        }
        drain(d, o);
    }
    int64_t end = g_np ? g_press[g_np - 1].up : 0;
    for (int k = 0; k < 4; k++) {
        int64_t dl = dec_deadline(d);
        if (dl == INT64_MAX) {
            break;
        }
        dec_poll(d, dl);
        drain(d, o);
    }
    (void)end;
    /* trim trailing space */
    while (o->len && o->text[o->len - 1] == ' ') {
        o->text[--o->len] = '\0';
    }
}

static int levenshtein(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    int *prev = malloc(sizeof(int) * (lb + 1)), *cur = malloc(sizeof(int) * (lb + 1));
    for (int j = 0; j <= lb; j++) {
        prev[j] = j;
    }
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        for (int j = 1; j <= lb; j++) {
            int c = prev[j - 1] + (a[i - 1] != b[j - 1]);
            int del = prev[j] + 1, ins = cur[j - 1] + 1;
            cur[j] = c < del ? (c < ins ? c : ins) : (del < ins ? del : ins);
        }
        int *t = prev;
        prev = cur;
        cur = t;
    }
    int r = prev[lb];
    free(prev);
    free(cur);
    return r;
}

static const char *CORPUS =
    "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG "
    "MEET ME AT THE NORTH GATE AT SIX AND BRING WATER "
    "I AM RUNNING LATE BY TWENTY MINUTES SORRY "
    "CAN YOU HEAR ME NOW THE SIGNAL IS WEAK HERE "
    "WE REACHED THE CAMP SAFELY ALL GOOD SEE YOU TOMORROW "
    "PLEASE CALL HOME WHEN YOU GET THIS MESSAGE "
    "BATTERY AT 40 PERCENT HEADING BACK NOW "
    "WHERE ARE YOU I CANNOT SEE THE PATH IN THE DARK "
    "TURN LEFT AFTER THE BRIDGE THEN WALK 200 METRES "
    "THANKS FOR THE HELP TODAY IT MEANT A LOT";

typedef struct {
    double cer;       /* character error rate incl. spaces */
    double cer_chars; /* ignoring word spaces */
    int paris_tries;
    float d_prime;
    cal_health_t health;
    float dot_mean, dash_mean;
} scen_result_t;

static scen_result_t scenario(const char *name, fist_t f, const char *text, uint64_t seed)
{
    scen_result_t r;
    calib_t cal;
    decoder_t dec;
    decode_out_t out;
    rng_seed(seed);
    cal_err_t ce = CAL_ERR_COUNT;
    int tries = 0;
    while (ce != CAL_OK && tries < 6) { /* a user re-keys PARIS when told it was ambiguous */
        fist_t paris_fist = f;
        ce = sim_paris(&cal, &paris_fist);
        tries++;
    }
    CHECK(ce == CAL_OK, "%s: PARIS failed: %s", name, cal_err_str(ce));
    dec_init(&dec, &cal);
    g_np = 0;
    int64_t t = 5000;
    sim_text(&f, text, &t);
    run_decoder(&dec, &out);
    int dist = levenshtein(text, out.text);
    r.cer = (double)dist / (double)strlen(text);
    char a[8192], b[8192];
    int na = 0, nb = 0;
    for (const char *q = text; *q; q++) {
        if (*q != ' ') {
            a[na++] = *q;
        }
    }
    for (const char *q = out.text; *q; q++) {
        if (*q != ' ') {
            b[nb++] = *q;
        }
    }
    a[na] = b[nb] = 0;
    r.cer_chars = (double)levenshtein(a, b) / (double)na;
    r.paris_tries = tries;
    r.d_prime = cal_dprime(&cal);
    r.health = cal_health(&cal);
    r.dot_mean = cal_profile(&cal)->dot_mean;
    r.dash_mean = cal_profile(&cal)->dash_mean;
    printf("  %-30s CER %5.2f%% (letters only %5.2f%%)  d'=%4.1f health=%d dot=%5.1f dash=%5.1f PARIS x%d\n",
           name, 100 * r.cer, 100 * r.cer_chars, r.d_prime, r.health, r.dot_mean, r.dash_mean, r.paris_tries);
    if (g_verbose > 1 || (r.cer > 0.03 && g_verbose)) {
        printf("    in : %s\n    out: %s\n", text, out.text);
    }
    return r;
}

/* ------------------------------------------------------------------------- */

static void test_morse(void)
{
    printf("morse table / IDs\n");
    const char *all = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?'!/()&:;=+-_\"$@";
    for (const char *p = all; *p; p++) {
        const char *pat = morse_encode(*p);
        CHECK(pat != NULL, "encode %c", *p);
        CHECK(pat && morse_decode(pat) == *p, "round trip %c", *p);
    }
    CHECK(morse_encode('a') && strcmp(morse_encode('a'), ".-") == 0, "lower case");
    CHECK(morse_decode("......") == 0, "6 dots invalid");
    CHECK(morse_is_error_prosign("........"), "8 dots");
    CHECK(morse_is_error_prosign("......."), "7 dots");
    CHECK(!morse_is_error_prosign("......"), "6 dots not error");
    CHECK(morse_is_sos_prosign("...---..."), "sos");
    CHECK(morse_text_units("PARIS") + 7 == 50, "PARIS = 50 units incl. word gap, got %u",
          morse_text_units("PARIS") + 7);
    char buf[32];
    morse_sanitize("  hello   world#  ", buf, sizeof buf);
    CHECK(strcmp(buf, "HELLO WORLD?") == 0, "sanitize -> '%s'", buf);

    CHECK(strlen(ID_ALPHABET) == ID_ALPHABET_LEN, "alphabet length");
    CHECK(!strchr(ID_ALPHABET, 'O') && !strchr(ID_ALPHABET, 'I') && !strchr(ID_ALPHABET, 'L') &&
              !strchr(ID_ALPHABET, '0') && !strchr(ID_ALPHABET, '1'),
          "ambiguous glyphs excluded");
    CHECK(id_valid("K7Q") && id_valid("k7q") && id_valid("K7QZ"), "valid ids");
    CHECK(!id_valid("K0Q") && !id_valid("KOQ") && !id_valid("K7") && !id_valid("K7QZ2"), "invalid ids");
    char id[5];
    int bad = 0;
    for (uint32_t a = 0; a < 31u * 31u * 31u; a += 7) {
        if (!addr_to_id(a, id) || id_to_addr(id) != a || strlen(id) != 3) {
            bad++;
        }
    }
    CHECK(bad == 0, "3-char addr round trip (%d bad)", bad);
    uint32_t a4 = id_to_addr("2222");
    CHECK(a4 == 29791, "first 4-char addr follows last 3-char (%u)", a4);
    CHECK(addr_to_id(id_to_addr("ZZZZ"), id) && strcmp(id, "ZZZZ") == 0, "4-char round trip");
    CHECK(id_to_addr("ZZZZ") < ADDR_NONE, "4-char space below reserved values");
    CHECK(!addr_to_id(ADDR_BROADCAST, id), "broadcast is not an ID");
}

static void test_hseq(void)
{
    printf("haptic sequences\n");
    hparams_t p;
    hparams_default(&p);
    p.unit_ms = 200;
    hjob_t j;
    hjob_init(&j);
    hjob_morse(&j, "SOS");
    uint32_t d = hjob_duration_ms(&j, &p);
    CHECK(d == morse_text_units("SOS") * 200u, "SOS duration %u", d);

    /* A single dash (v0.6.4, agreed 24 Oct): M1 alone 1.1U, both 0.3U, M2
     * alone 1.6U, so M1 runs 1.4U and M2 1.9U: 220 / 60 / 320 ms at Q5. */
    hjob_init(&j);
    hjob_morse(&j, "T");
    hseq_t s;
    hseg_t seg[8];
    int n = 0;
    hseq_start(&s, &j, &p);
    while (n < 8 && hseq_next(&s, &seg[n])) {
        n++;
    }
    CHECK(n == 3, "dash = 3 segments, got %d", n);
    CHECK(seg[0].ms == 220 && seg[0].m1 && !seg[0].m2, "M1 lead %u", seg[0].ms);
    CHECK(seg[1].ms == 60 && seg[1].m1 && seg[1].m2, "overlap %u", seg[1].ms);
    CHECK(seg[2].ms == 320 && !seg[2].m1 && seg[2].m2, "M2 tail %u", seg[2].ms);
    CHECK(seg[0].ms + seg[1].ms == 280 && seg[1].ms + seg[2].ms == 380, "M1 1.4U, M2 1.9U");
    /* ... and it scales with the speed: Q9 (120 ms): 132 / 36 / 192. */
    p.unit_ms = 120;
    hseq_start(&s, &j, &p);
    n = 0;
    while (n < 8 && hseq_next(&s, &seg[n])) {
        n++;
    }
    CHECK(n == 3 && seg[0].ms == 132 && seg[1].ms == 36 && seg[2].ms == 192, "the overlap in units: %u / %u / %u",
          seg[0].ms, seg[1].ms, seg[2].ms);
    p.unit_ms = 200;

    /* Dots never touch M2; tones always use both motors together. */
    hjob_init(&j);
    hjob_morse(&j, "EISH5");
    hseq_start(&s, &j, &p);
    bool m2_used = false;
    hseg_t g;
    while (hseq_next(&s, &g)) {
        m2_used |= g.m2 != 0;
    }
    CHECK(!m2_used, "dots only on M1");
    for (int t = 0; t < TONE__COUNT; t++) {
        hjob_init(&j);
        hjob_tone(&j, (tone_t)t);
        hseq_start(&s, &j, &p);
        bool ok = true;
        while (hseq_next(&s, &g)) {
            ok &= (g.m1 == g.m2);
        }
        CHECK(ok, "tone %d synchronous", t);
    }

    /* Composite: attention, then ID, then message, with gaps between items. */
    hjob_init(&j);
    hjob_tone(&j, TONE_ATTENTION);
    hjob_morse(&j, "K7Q");
    hjob_morse(&j, "HI THERE");
    uint32_t att = 300 + 150 + 80 + 100 + 80 + 100 + 80;
    uint32_t expect = att + p.item_gap_ms + morse_text_units("K7Q") * 200 + 7 * 200 + morse_text_units("HI THERE") * 200;
    d = hjob_duration_ms(&j, &p);
    CHECK(d == expect, "composite %u vs %u", d, expect);

    /* Farnsworth stretches only gaps between characters/words. */
    p.farnsworth_x10 = 20;
    hjob_init(&j);
    hjob_morse(&j, "EE");
    d = hjob_duration_ms(&j, &p);
    CHECK(d == 200 + 1200 + 200, "farnsworth letter gap %u", d);

    CHECK(hspeed_unit_ms(5) == 200, "default speed is U=200");
    CHECK(hspeed_unit_ms(1) > hspeed_unit_ms(9), "Q1 slowest");
}

/*
 * phone_model() -- the phone app's way of following a Morse part it was
 * told about (PL line, v0.6.2), written out in C so the host tests can hold
 * it against the generator: from the part's start, each element of each
 * character in turn (spaces skipped; a character without Morse plays as
 * '?'), dot 1U, dash 3U, 1U inside a letter, 3U x fx10/10 between letters
 * and 7U x fx10/10 between words, with the generator's integer arithmetic.
 * Fills start[] / len[] for up to max elements; returns how many.
 * (lib/protocol.dart's playTimeline() is the same algorithm.)
 */
static int phone_model(const char *text, uint32_t t0, uint32_t unit, uint32_t fx10, uint32_t *start, uint32_t *len,
                       int max)
{
    int n = 0;
    uint32_t t = t0;
    size_t i = 0, L = strlen(text);
    while (i < L && text[i] == ' ') {
        i++;
    }
    while (i < L) {
        const char *pat = morse_encode(text[i]);
        if (!pat) {
            pat = morse_encode('?');
        }
        for (int e = 0; pat[e]; e++) {
            uint32_t d = pat[e] == '-' ? 3 * unit : unit;
            if (n < max) {
                start[n] = t;
                len[n] = d;
                n++;
            }
            t += d;
            if (pat[e + 1]) {
                t += unit;
            }
        }
        i++;
        bool space = false;
        while (i < L && text[i] == ' ') {
            space = true;
            i++;
        }
        if (i < L) {
            t += space ? 7u * unit * fx10 / 10u : 3u * unit * fx10 / 10u;
        }
    }
    return n;
}

/* test_play_timeline() -- hjob_item_starts() (v0.6.2), and the phone's
 * model of a playback against the generator's real segments: every Morse
 * element must start, and last, exactly where the phone thinks. */
static void test_play_timeline(void)
{
    printf("haptic sequences: Morse start times for the phone (PL)\n");
    hparams_t p;
    hparams_default(&p);
    p.unit_ms = 200;
    hjob_t j;
    uint32_t st[HJOB_MAX_ITEMS];

    /* attention, ID, message (an incoming message, spec §9) */
    hjob_init(&j);
    hjob_tone(&j, TONE_ATTENTION);
    hjob_morse(&j, "K7Q");
    hjob_morse(&j, "HI THERE");
    hjob_item_starts(&j, &p, st);
    uint32_t att = 300 + 150 + 80 + 100 + 80 + 100 + 80;
    CHECK(st[0] == HSEQ_NO_START, "a tone has no start (%u)", st[0]);
    CHECK(st[1] == att + p.item_gap_ms, "the ID starts after attention + item gap (%u)", st[1]);
    CHECK(st[2] == att + p.item_gap_ms + morse_text_units("K7Q") * 200 + 7 * 200, "the text after a word gap (%u)",
          st[2]);
    CHECK(st[3] == HSEQ_NO_START, "no item 3");

    /* an explicit gap replaces the automatic one; an empty Morse item has no start */
    hjob_init(&j);
    hjob_morse(&j, "E");
    hjob_gap(&j, 500);
    hjob_morse(&j, "   ");
    hjob_morse(&j, " T");
    hjob_item_starts(&j, &p, st);
    CHECK(st[0] == 0, "the first item starts at once (%u)", st[0]);
    CHECK(st[1] == HSEQ_NO_START && st[2] == HSEQ_NO_START, "a gap and an empty item: no start");
    CHECK(st[3] == 200 + 500 + 7 * 200, "after the gap, the empty item's word gap, then T (%u)", st[3]);

    /* The phone's model against the generator, on random texts at every
     * speed and Farnsworth setting, behind a tone and an ID. */
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789?.,/  #";
    rng_seed(77);
    int bad = 0, jobs = 0;
    for (int round = 0; round < 400; round++) {
        p.unit_ms = hspeed_unit_ms(1 + (int)(rng_u() * 9));
        p.farnsworth_x10 = (uint8_t)(10 + (int)(rng_u() * 31));
        p.dash_m1_cu = (uint8_t)(50 + (int)(rng_u() * 150));
        p.dash_style = rng_u() < 0.5 ? HDASH_SWEEP : HDASH_M2; /* (v0.6.3: the timing is the same either way) */
        p.drive_pct = hstrength_drive_pct(1 + (int)(rng_u() * 9)); /* (v0.6.4: ... and at every strength) */
        p.kick_ms = 30;
        p.overlap_cu = (uint8_t)(rng_u() * 60);
        char text[61];
        int len = 1 + (int)(rng_u() * 60);
        for (int k = 0; k < len; k++) {
            text[k] = alphabet[(int)(rng_u() * (sizeof alphabet - 1))];
        }
        text[len] = '\0';
        hjob_init(&j);
        if (round % 2) {
            hjob_tone(&j, TONE_ATTENTION);
            hjob_morse(&j, "DEA");
        }
        hjob_morse(&j, text);
        hjob_item_starts(&j, &p, st);
        /* The generator's elements: runs of segments with a motor on. A run
         * that starts with both motors is a system tone, not Morse (a Morse
         * element always starts on M1 alone): left out. */
        uint32_t g_start[400], g_len[400];
        int gn = 0;
        hseq_t s;
        hseg_t g;
        uint32_t t = 0;
        bool on = false, tone_run = false;
        hseq_start(&s, &j, &p);
        while (hseq_next(&s, &g)) {
            bool now_on = g.m1 || g.m2;
            if (now_on && !on) {
                tone_run = g.m1 && g.m2;
                if (!tone_run && gn < 400) {
                    g_start[gn] = t;
                    g_len[gn] = 0;
                    gn++;
                }
            }
            if (now_on && !tone_run && gn > 0) {
                g_len[gn - 1] += g.ms;
            }
            on = now_on;
            t += g.ms;
        }
        /* The model: every Morse item from its start. */
        uint32_t m_start[400], m_len[400];
        int mn = 0;
        for (int i = 0; i < j.n; i++) {
            if (j.items[i].kind == HI_MORSE && st[i] != HSEQ_NO_START) {
                mn += phone_model(&j.text[j.items[i].arg], st[i], p.unit_ms, p.farnsworth_x10, m_start + mn,
                                  m_len + mn, 400 - mn);
            }
        }
        jobs++;
        if (gn != mn) {
            bad++;
            if (g_verbose) {
                printf("  \"%s\": generator %d elements, model %d\n", text, gn, mn);
            }
            continue;
        }
        for (int k = 0; k < mn; k++) {
            if (g_start[k] != m_start[k] || g_len[k] != m_len[k]) {
                bad++;
                if (g_verbose) {
                    printf("  \"%s\" element %d: generator %u+%u, model %u+%u\n", text, k, g_start[k], g_len[k],
                           m_start[k], m_len[k]);
                }
                break;
            }
        }
    }
    CHECK(bad == 0, "the phone's timing matches the motors element for element (%d of %d jobs differ)", bad, jobs);

    /* Dashes on M2 only (H2, v0.6.3): a dash is one 3U segment on M2, a dot
     * stays on M1, and the whole job lasts exactly as long as with the sweep. */
    hparams_default(&p);
    CHECK(p.dash_style == HDASH_SWEEP, "the sweep by default");
    p.unit_ms = 200;
    hjob_init(&j);
    hjob_morse(&j, "T");
    {
        hseq_t s;
        hseg_t g[4];
        int n = 0;
        p.dash_style = HDASH_M2;
        hseq_start(&s, &j, &p);
        while (n < 4 && hseq_next(&s, &g[n])) {
            n++;
        }
        CHECK(n == 1 && g[0].ms == 600 && g[0].m1 == 0 && g[0].m2 == HLEVEL_FULL, "H2: a dash is M2 alone for 3U");
        hjob_init(&j);
        hjob_morse(&j, "EISH5 PARIS SOS");
        bool m2_dot = false, m1_dash = false;
        hseq_start(&s, &j, &p);
        hseg_t x;
        while (hseq_next(&s, &x)) {
            m2_dot |= x.m2 && x.ms == 200;
            m1_dash |= x.m1 && x.ms > 200;
        }
        CHECK(!m2_dot && !m1_dash, "H2: dots only on M1, dashes only on M2");
        uint32_t d2 = hjob_duration_ms(&j, &p);
        p.dash_style = HDASH_SWEEP;
        CHECK(d2 == hjob_duration_ms(&j, &p), "H2 takes exactly as long as the sweep (%u ms)", d2);
        p.dash_style = 0;
        hseq_start(&s, &j, &p);
        CHECK(s.p.dash_style == HDASH_SWEEP, "0 (never set) plays the sweep");
    }

    /* Vibration strength (I, 1-9 since v0.6.4): I9 the old maximum (full at
     * 3.4 V), I8 the default (full at 3.3 V), I1-I7 less drive at 3.3 V. */
    CHECK(HSTRENGTH_MAX == 9 && HSTRENGTH_DEFAULT == 8, "1-9, the default 8");
    CHECK(hstrength_mv(9) == 3400 && hstrength_drive_pct(9) == 100, "I9 = full at 3.4 V");
    CHECK(hstrength_mv(8) == 3300 && hstrength_drive_pct(8) == 100, "I8 = full at 3.3 V");
    CHECK(hstrength_mv(0) == 3300 && hstrength_drive_pct(0) == 100 && hstrength_drive_pct(10) == 100,
          "unset or out of range: the default");
    bool rising = true;
    for (int l = HSTRENGTH_MIN; l < 8; l++) {
        rising &= hstrength_drive_pct(l) < hstrength_drive_pct(l + 1) && hstrength_mv(l) == 3300;
    }
    CHECK(rising && hstrength_drive_pct(1) >= 40, "I1-I8 rise, at 3.3 V; I1 at %d %%", hstrength_drive_pct(1));

    /* Below full drive: each motor starts with a kick at full, then the
     * level; the timing never changes. A dot at I1, Q5: 30 ms full, 170 ms
     * at 45 %. A dash: M1 kicks at its start, M2 at the overlap. */
    hparams_default(&p);
    p.unit_ms = 200;
    p.drive_pct = hstrength_drive_pct(1);
    p.kick_ms = 30;
    uint8_t lvl = (uint8_t)((255u * p.drive_pct + 50u) / 100u);
    {
        hseq_t s;
        hseg_t g[12];
        int n = 0;
        hjob_init(&j);
        hjob_morse(&j, "E");
        hseq_start(&s, &j, &p);
        while (n < 12 && hseq_next(&s, &g[n])) {
            n++;
        }
        CHECK(n == 2 && g[0].ms == 30 && g[0].m1 == 255 && g[1].ms == 170 && g[1].m1 == lvl && !g[1].m2,
              "a dot at I1: kick then the level (%d segments)", n);
        hjob_init(&j);
        hjob_morse(&j, "T");
        n = 0;
        hseq_start(&s, &j, &p);
        while (n < 12 && hseq_next(&s, &g[n])) {
            n++;
        }
        /* M1 kick 30 (M1 full) | M1 190 at the level | overlap: M2 kick 30 (M1 level, M2 full) | overlap 30 both
         * at the level | M2 320 at the level */
        CHECK(n == 5 && g[0].ms == 30 && g[0].m1 == 255 && !g[0].m2 && g[1].ms == 190 && g[1].m1 == lvl &&
                  g[2].ms == 30 && g[2].m1 == lvl && g[2].m2 == 255 && g[3].ms == 30 && g[3].m2 == lvl &&
                  g[4].ms == 320 && !g[4].m1 && g[4].m2 == lvl,
              "a dash at I1: M1 kicks, then M2 kicks at the overlap (%d segments)", n);
        /* every job lasts exactly as long at every strength, tones included, and nothing is dropped */
        hjob_init(&j);
        hjob_tone(&j, TONE_SOS_ALARM);
        hjob_tone(&j, TONE_ATTENTION);
        hjob_morse(&j, "K7Q");
        hjob_morse(&j, "PARIS 0 SOS");
        hparams_t full = p;
        full.drive_pct = 100;
        uint32_t want = hjob_duration_ms(&j, &full);
        bool same = true;
        for (int l = HSTRENGTH_MIN; l <= HSTRENGTH_MAX; l++) {
            p.drive_pct = hstrength_drive_pct(l);
            same &= hjob_duration_ms(&j, &p) == want;
        }
        CHECK(same, "the same length (%u ms) at every strength", want);
    }
}

/* test_open_pattern() -- what the phone's live keying view reads from the
 * decoder (v0.6.2): the letter being keyed, the latest press, and whether a
 * release counted as an element. */
static void test_open_pattern(void)
{
    printf("decoder: the letter being keyed (KY)\n");
    calib_t cal;
    decoder_t dec;
    fist_t f = {100, 3.0, 1.0, 3.0, 7.0, 0.05, 0, 0, 1.0};
    rng_seed(51);
    CHECK(sim_paris(&cal, &f) == CAL_OK, "paris");
    dec_init(&dec, &cal);
    char pat[DEC_MAX_EL + 1];
    CHECK(dec_open_pattern(&dec, pat, sizeof pat) == 0 && pat[0] == '\0', "nothing open at first");
    uint32_t n0 = dec.n_pressed;
    int64_t t = 5000;
    dec_key_down(&dec, t);
    dec_element(&dec, t, t + 100); /* a dot */
    t += 100 + 100;
    CHECK(dec_open_pattern(&dec, pat, sizeof pat) == 1 && strcmp(pat, ".") == 0, "after a dot: \"%s\"", pat);
    CHECK(dec.n_pressed == n0 + 1 && dec.last_ms == 100 && !dec.last_dash, "the dot counted (%u, %u ms)",
          dec.n_pressed - n0, dec.last_ms);
    dec_key_down(&dec, t);
    dec_element(&dec, t, t + 8); /* a bounce */
    CHECK(dec.n_pressed == n0 + 1 && dec.last_ms == 100, "a bounce doesn't count");
    dec_key_down(&dec, t + 20);
    dec_element(&dec, t + 20, t + 320); /* a dash */
    t += 320;
    CHECK(dec_open_pattern(&dec, pat, sizeof pat) == 2 && strcmp(pat, ".-") == 0, "after a dash: \"%s\"", pat);
    CHECK(dec.n_pressed == n0 + 2 && dec.last_ms == 300 && dec.last_dash, "the dash counted (%u ms)", dec.last_ms);
    char tiny[2];
    CHECK(dec_open_pattern(&dec, tiny, sizeof tiny) == 1 && strcmp(tiny, ".") == 0, "cut to fit");
    dec_poll(&dec, t + 2000); /* well past the letter gap */
    dec_evt_t e;
    bool got = dec_get(&dec, &e);
    CHECK(got && e.kind == DEC_CHAR && e.c == 'A' && strcmp(e.pattern, ".-") == 0, "it closes as A (.-)");
    CHECK(dec_open_pattern(&dec, pat, sizeof pat) == 0, "and nothing is open after it");
    dec_reset(&dec);
    CHECK(dec.n_pressed == n0 + 2, "a reset keeps the count (the UI compares before / after)");
}

static void test_paris(void)
{
    printf("PARIS bootstrap\n");
    calib_t c;
    fist_t good = {100, 3.0, 1.0, 3.0, 7.0, 0.08, 0, 0, 1.0};
    rng_seed(1);
    CHECK(sim_paris(&c, &good) == CAL_OK, "good fist");
    CHECK(fabsf(cal_profile(&c)->dot_mean - 100) < 15, "dot mean %.1f", cal_profile(&c)->dot_mean);
    CHECK(fabsf(cal_profile(&c)->ratio - 3.0f) < 0.4f, "ratio %.2f", cal_profile(&c)->ratio);
    CHECK(cal_health(&c) == CAL_HEALTHY, "healthy after PARIS");

    /* Low-ratio keyer (2.2) must start healthy (the flat-20% MAD bug). */
    fist_t low = {120, 2.2, 1.0, 3.0, 7.0, 0.06, 0, 0, 1.0};
    rng_seed(2);
    cal_err_t e = sim_paris(&c, &low);
    CHECK(e == CAL_OK, "low ratio PARIS: %s", cal_err_str(e));
    CHECK(cal_health(&c) == CAL_HEALTHY, "low ratio starts healthy, d'=%.2f", cal_dprime(&c));

    /* Sloppy: dots and dashes overlap. */
    float el[14], gaps[13];
    for (int i = 0; i < 14; i++) {
        el[i] = 150 + (i % 3) * 40;
    }
    for (int i = 0; i < 13; i++) {
        gaps[i] = 150;
    }
    CHECK(cal_from_paris(&c, el, 14, gaps, 13) == CAL_ERR_ELEMENTS, "overlapping elements rejected");
    CHECK(cal_from_paris(&c, el, 13, gaps, 12) == CAL_ERR_COUNT, "wrong count rejected");

    /* Letter gaps no longer than intra gaps. */
    rng_seed(3);
    fist_t nogap = {100, 3.0, 1.0, 1.0, 7.0, 0.05, 0, 0, 1.0};
    CHECK(sim_paris(&c, &nogap) == CAL_ERR_GAPS, "no letter gaps rejected");
}

static void test_scenarios(void)
{
    printf("decoder + adaptive calibration (simulated keying)\n");
    scen_result_t r;

    r = scenario("standard 12 WPM, 10% jitter", (fist_t){100, 3.0, 1.0, 3.0, 7.0, 0.10, 0, 0, 1.0}, CORPUS, 11);
    CHECK(r.cer < 0.01, "CER %.3f", r.cer);

    r = scenario("beginner, thinking pauses", (fist_t){180, 2.6, 1.1, 3.5, 10.0, 0.16, 0.15, 3.0, 1.0}, CORPUS, 12);
    /* Thinking pauses (5-8 units) overlap this keyer's word gaps (8-12 units):
     * some word spaces are inherently ambiguous. Letters must stay right. */
    CHECK(r.cer < 0.09, "CER %.3f", r.cer);
    CHECK(r.cer_chars < 0.025, "letter CER %.3f", r.cer_chars);

    r = scenario("low ratio 2.2", (fist_t){120, 2.2, 1.0, 3.0, 7.0, 0.10, 0, 0, 1.0}, CORPUS, 13);
    CHECK(r.cer < 0.02, "CER %.3f", r.cer);
    CHECK(r.health == CAL_HEALTHY, "low ratio stays healthy (d'=%.2f)", r.d_prime);

    r = scenario("speeding up 140->~85 ms", (fist_t){140, 3.0, 1.0, 3.0, 7.0, 0.10, 0, 0, 0.9985}, CORPUS, 14);
    CHECK(r.cer < 0.02, "CER %.3f", r.cer);
    CHECK(r.dot_mean < 105, "tracked speed-up, dot_mean %.1f", r.dot_mean);

    r = scenario("slowing down 90->~150 ms", (fist_t){90, 3.0, 1.0, 3.0, 7.0, 0.10, 0, 0, 1.0015}, CORPUS, 15);
    CHECK(r.cer < 0.02, "CER %.3f", r.cer);
    CHECK(r.dot_mean > 125, "tracked slow-down, dot_mean %.1f", r.dot_mean);

    r = scenario("sloppy 22% jitter", (fist_t){130, 3.0, 1.0, 3.0, 7.0, 0.22, 0.1, 3.0, 1.0}, CORPUS, 16);
    /* 22 % jitter on a 390 ms dash puts ~3 % of dashes below the boundary:
     * the point here is robustness, not accuracy. */
    CHECK(r.cer_chars < 0.10, "letter CER %.3f", r.cer_chars);
    CHECK(r.health != CAL_COLLAPSED, "sloppy keyer not collapsed");
}

static void test_hesitation(void)
{
    printf("hesitation dash right after PARIS (MAD clipping fix)\n");
    calib_t cal;
    decoder_t dec;
    decode_out_t out;
    fist_t f = {100, 3.0, 1.0, 3.0, 7.0, 0.08, 0, 0, 1.0};
    rng_seed(21);
    CHECK(sim_paris(&cal, &f) == CAL_OK, "paris");
    float d0 = cal_dprime(&cal);
    dec_init(&dec, &cal);
    g_np = 0;
    int64_t t = 3000;
    /* "T" keyed as one 1.4 s dash, then normal text. */
    g_press[g_np++] = (press_t){t, t + 1400};
    t += 1400 + 300;
    sim_text(&f, "THE QUICK BROWN FOX", &t);
    run_decoder(&dec, &out);
    printf("  d' before %.2f, after %.2f, health %d, out '%s'\n", d0, cal_dprime(&cal), cal_health(&cal), out.text);
    CHECK(cal_health(&cal) == CAL_HEALTHY, "stays healthy");
    CHECK(strcmp(out.text, "TTHE QUICK BROWN FOX") == 0, "decoded '%s'", out.text);
}

static void test_error_rollback(void)
{
    printf("error prosign rollback\n");
    calib_t cal;
    decoder_t dec;
    decode_out_t out;
    fist_t f = {100, 3.0, 1.0, 3.0, 7.0, 0.05, 0, 0, 1.0};
    rng_seed(31);
    CHECK(sim_paris(&cal, &f) == CAL_OK, "paris");
    dec_init(&dec, &cal);
    g_np = 0;
    int64_t t = 2000;
    sim_text(&f, "HELL", &t);
    run_decoder(&dec, &out);
    cal_profile_t before = *cal_profile(&cal);
    /* Now a badly keyed "O" (dashes far too long: will still decode as O) and
     * the error prosign straight after it. */
    decode_out_t out2;
    g_np = 0;
    t += 400;
    g_press[g_np++] = (press_t){t, t + 520};
    t += 520 + 100;
    g_press[g_np++] = (press_t){t, t + 520};
    t += 520 + 100;
    g_press[g_np++] = (press_t){t, t + 520};
    t += 520 + 300;
    for (int i = 0; i < 8; i++) {
        g_press[g_np++] = (press_t){t, t + 100};
        t += 100 + 100;
    }
    run_decoder(&dec, &out2);
    const cal_profile_t *after = cal_profile(&cal);
    CHECK(out2.n_err == 1, "one error prosign (%d)", out2.n_err);
    CHECK(out2.len == 0, "no word space after the correction pause ('%s')", out2.text);
    CHECK(memcmp(&before, after, sizeof before) == 0, "learning from the deleted O undone (dash %.1f vs %.1f)",
          before.dash_mean, after->dash_mean);
    CHECK(cal.widen_left > 0, "ambiguity band widened");
}

static void test_phantom(void)
{
    printf("long run of dots at a new speed, then dashes (phantom updates)\n");
    calib_t cal;
    decoder_t dec;
    decode_out_t out;
    fist_t f = {100, 3.0, 1.0, 3.0, 7.0, 0.06, 0, 0, 1.0};
    rng_seed(41);
    CHECK(sim_paris(&cal, &f) == CAL_OK, "paris");
    dec_init(&dec, &cal);
    g_np = 0;
    int64_t t = 2000;
    /* Slow down by 35 % while keying only dot characters... */
    f.drift = 1.012;
    sim_text(&f, "EIS HIS SHE IS HIS SHES HISS EH SIS HE IS", &t);
    f.drift = 1.0;
    printf("  fist dot now %.0f ms\n", f.dot_ms);
    /* ...then dash-heavy text at that speed. */
    t += (int64_t)(f.word_x * f.dot_ms);
    sim_text(&f, "MOTTO TOM OMIT TIM MOM", &t);
    run_decoder(&dec, &out);
    const char *want = "EIS HIS SHE IS HIS SHES HISS EH SIS HE IS MOTTO TOM OMIT TIM MOM";
    int dist = levenshtein(want, out.text);
    printf("  out: %s (dist %d) dot=%.1f dash=%.1f\n", out.text, dist, cal_profile(&cal)->dot_mean,
           cal_profile(&cal)->dash_mean);
    CHECK(dist <= 1, "dashes still recognised after the dot run (dist %d)", dist);
}

static void test_sos(void)
{
    printf("SOS detection\n");
    calib_t cal;
    decoder_t dec;
    decode_out_t out;
    fist_t f = {100, 3.0, 1.0, 3.0, 7.0, 0.06, 0, 0, 1.0};
    rng_seed(51);
    CHECK(sim_paris(&cal, &f) == CAL_OK, "paris");

    /* Panicked prosign: slow and with a squashed ratio -> classification is
     * wrecked, but the shape still says SOS. */
    dec_init(&dec, &cal);
    g_np = 0;
    int64_t t = 1000;
    fist_t panic = {260, 1.9, 0.6, 3.0, 7.0, 0.08, 0, 0, 1.0};
    sim_pattern(&panic, "...---...", &t, false);
    run_decoder(&dec, &out);
    CHECK(out.n_sos == 1, "panicked prosign -> SOS (n=%d, split into %d groups)", out.n_sos, out.sos_undo);
    CHECK(out.len == 0, "nothing left in the text: '%s'", out.text);

    /* S O S as letters at the start of a word, mid-message. */
    dec_init(&dec, &cal);
    g_np = 0;
    t = 1000;
    sim_text(&f, "HELP SOS", &t);
    run_decoder(&dec, &out);
    CHECK(out.n_sos == 1 && out.sos_undo == 3, "letters -> SOS (n=%d undo=%d)", out.n_sos, out.sos_undo);
    CHECK(strcmp(out.text, "HELP") == 0, "S and O removed from text: '%s'", out.text);

    /* Words containing those letters must not trigger it. */
    const char *nots[] = {"GHOST", "ISOS", "SOSO TEST", "HOSES", "VOS", "SORRY", "SOUND", "DOG", "GOD", "ROD", "HIS", "HIS HIS HIS"};
    for (unsigned k = 0; k < sizeof(nots) / sizeof(nots[0]); k++) {
        dec_init(&dec, &cal);
        g_np = 0;
        t = 1000;
        sim_text(&f, nots[k], &t);
        run_decoder(&dec, &out);
        CHECK(out.n_sos == 0, "'%s' -> sos=%d", nots[k], out.n_sos);
    }
    /* "SORRY" whose R is keyed with a dash short enough to read as a dot:
     * the letters say S O S but the word goes on, so no emergency. */
    dec_init(&dec, &cal);
    g_np = 0;
    t = 1000;
    sim_text(&f, "SO", &t);
    t += 300;
    sim_pattern(&(fist_t){100, 1.5, 1.0, 3.0, 7.0, 0.0, 0, 0, 1.0}, ".-.", &t, false);
    t += 300;
    sim_text(&f, "RY", &t);
    run_decoder(&dec, &out);
    CHECK(out.n_sos == 0, "misread SORRY -> no SOS ('%s')", out.text);

    /* SOS then a 2 s 'send' hold straight away still counts as SOS. */
    dec_init(&dec, &cal);
    g_np = 0;
    t = 1000;
    sim_text(&f, "SOS", &t);
    t += 250;
    g_press[g_np++] = (press_t){t, t + 2100};
    run_decoder(&dec, &out);
    CHECK(out.n_sos == 1, "SOS then hold -> SOS (%d)", out.n_sos);

    /* No calibration at all: the shape test alone still catches it. */
    calib_t none;
    cal_reset(&none);
    dec_init(&dec, &none);
    g_np = 0;
    t = 1000;
    fist_t fast = {60, 3.0, 1.0, 3.0, 7.0, 0.08, 0, 0, 1.0};
    sim_pattern(&fast, "...---...", &t, false);
    run_decoder(&dec, &out);
    CHECK(out.n_sos == 1, "uncalibrated prosign -> SOS");
}

static void test_collapse_recovery(void)
{
    printf("degraded keying recovers without recalibration\n");
    calib_t cal;
    decoder_t dec;
    decode_out_t out;
    fist_t f = {100, 3.0, 1.0, 3.0, 7.0, 0.06, 0, 0, 1.0};
    rng_seed(61);
    CHECK(sim_paris(&cal, &f) == CAL_OK, "paris");
    dec_init(&dec, &cal);
    /* A stretch of very sloppy keying (ratio squashed, 35 % jitter). */
    fist_t bad = {110, 1.8, 1.0, 3.0, 7.0, 0.35, 0, 0, 1.0};
    g_np = 0;
    int64_t t = 2000;
    sim_text(&bad, "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG THE QUICK BROWN FOX", &t);
    run_decoder(&dec, &out);
    cal_health_t h_bad = cal_health(&cal);
    bool told = cal_take_degraded(&cal);
    printf("  after sloppy stretch: health %d d'=%.2f notified=%d\n", h_bad, cal_dprime(&cal), told);
    /* Back to normal keying. */
    g_np = 0;
    t += 3000;
    sim_text(&f, CORPUS, &t);
    run_decoder(&dec, &out);
    int dist = levenshtein(CORPUS, out.text);
    printf("  after normal keying: health %d d'=%.2f CER %.2f%%\n", cal_health(&cal), cal_dprime(&cal),
           100.0 * dist / strlen(CORPUS));
    CHECK(cal_health(&cal) == CAL_HEALTHY, "recovered to healthy");
    CHECK((double)dist / strlen(CORPUS) < 0.02, "decodes well again");
}


static void test_sos_sweep(void)
{
    printf("SOS sweep (randomised fists)\n");
    int false_sos = 0, runs = 0;
    double cer_sum = 0;
    for (int seed = 0; seed < 40; seed++) {
        rng_seed(1000 + seed);
        fist_t f = {70 + 140 * rng_u(), 2.3 + 1.2 * rng_u(), 0.9 + 0.3 * rng_u(), 2.8 + 1.0 * rng_u(),
                    6.5 + 3.0 * rng_u(), 0.06 + 0.12 * rng_u(), 0.1 * rng_u(), 3.0, 1.0};
        calib_t cal;
        decoder_t dec;
        decode_out_t out;
        cal_err_t ce = CAL_ERR_COUNT;
        for (int k = 0; k < 6 && ce != CAL_OK; k++) {
            fist_t pf = f;
            ce = sim_paris(&cal, &pf);
        }
        if (ce != CAL_OK) {
            continue;
        }
        dec_init(&dec, &cal);
        g_np = 0;
        int64_t t = 2000;
        sim_text(&f, CORPUS, &t);
        run_decoder(&dec, &out);
        false_sos += out.n_sos;
        cer_sum += (double)levenshtein(CORPUS, out.text) / strlen(CORPUS);
        runs++;
    }
    printf("  normal text: %d runs, false SOS = %d, mean CER %.2f%%\n", runs, false_sos, 100 * cer_sum / runs);
    CHECK(false_sos == 0, "no false SOS in normal text (%d)", false_sos);

    int hit = 0, tries = 0;
    for (int seed = 0; seed < 60; seed++) {
        rng_seed(5000 + seed);
        fist_t f = {100, 3.0, 1.0, 3.0, 7.0, 0.08, 0, 0, 1.0};
        calib_t cal;
        decoder_t dec;
        decode_out_t out;
        if (sim_paris(&cal, &f) != CAL_OK) {
            continue;
        }
        dec_init(&dec, &cal);
        /* panicked: 0.6x..2.5x the calibrated speed, squashed ratio, jumpy gaps */
        fist_t pf = {60 + 190 * rng_u(), 1.9 + 1.2 * rng_u(), 0.6 + 0.9 * rng_u(), 2.5 + 2.0 * rng_u(),
                     7.0, 0.10 + 0.10 * rng_u(), 0, 0, 1.0};
        g_np = 0;
        int64_t t = 2000;
        if (seed % 2) {
            sim_pattern(&pf, "...---...", &t, false);
        } else {
            sim_text(&pf, "SOS", &t);
        }
        run_decoder(&dec, &out);
        hit += out.n_sos > 0;
        tries++;
        if (!out.n_sos && getenv("DBG_SOS")) {
            printf("    MISS %s dot=%.0f ratio=%.2f intra=%.2f letter=%.2f jit=%.2f -> '%s' els:", seed % 2 ? "prosign" : "letters",
                   pf.dot_ms, pf.ratio, pf.intra_x, pf.letter_x, pf.jitter, out.text);
            for (int i = 0; i < g_np; i++) {
                printf(" %d", (int)(g_press[i].up - g_press[i].down));
                if (i < g_np - 1) {
                    printf("(%d)", (int)(g_press[i + 1].down - g_press[i].up));
                }
            }
            printf("\n");
        }
    }
    printf("  panicked SOS: detected %d / %d\n", hit, tries);
    CHECK(hit >= tries * 95 / 100, "SOS detection rate %d/%d", hit, tries);
}

/* ------------------------------------------------------------------------- */
/* Conversation rule (convo.h): when a received message skips attention + ID */
/* ------------------------------------------------------------------------- */

/*
 * test_convo() -- the agreed rule, case by case. A and B are two devices;
 * times are ms since boot. "bare" = the message plays without attention + ID.
 */
static void test_convo(void)
{
    const uint32_t A = id_to_addr("ABC"), B = id_to_addr("K7Q");
    const int64_t MIN = 60 * 1000;
    convo_t c;

    /* Fresh start (or after a restart): nothing is a conversation yet, even
     * with a device paired in an earlier session. */
    convo_init(&c);
    CHECK(!convo_active(&c, A, 0), "fresh start: first message announced");
    CHECK(!convo_active(&c, A, 100 * MIN), "fresh start, later: still announced");
    CHECK(!convo_active(&c, ADDR_NONE, 0), "no address is never a conversation");

    /* Paired, nothing said yet: within 5 min the first message is bare ... */
    convo_note(&c, A, 10 * MIN);
    CHECK(convo_active(&c, A, 10 * MIN + 1000), "1 s after pairing: bare");
    CHECK(convo_active(&c, A, 10 * MIN + CONVO_QUIET_MS), "exactly 5 min after pairing: still bare");
    /* ... after more than 5 min it is announced. */
    CHECK(!convo_active(&c, A, 10 * MIN + CONVO_QUIET_MS + 1), "just over 5 min after pairing: announced");
    CHECK(!convo_active(&c, A, 30 * MIN), "20 min after pairing: announced");

    /* Talking: each message (either way) restarts the 5 minutes. */
    convo_note(&c, A, 40 * MIN);              /* A's message arrives (announced: it was the first in a while) */
    CHECK(convo_active(&c, A, 43 * MIN), "reply 3 min later: bare");
    convo_note(&c, A, 43 * MIN);              /* that reply was received ... */
    convo_note(&c, A, 47 * MIN);              /* ... and ours was confirmed 4 min later */
    CHECK(convo_active(&c, A, 51 * MIN), "4 min after our message: bare");
    CHECK(!convo_active(&c, A, 52 * MIN + 1), "5 min and a bit after our message: announced");

    /* Someone else: always announced, and they become the conversation, so a
     * bare message is never mistaken for the partner's. */
    convo_note(&c, A, 60 * MIN);
    CHECK(!convo_active(&c, B, 60 * MIN + 1000), "another device in the middle of a conversation: announced");
    convo_note(&c, B, 60 * MIN + 1000);       /* B's message arrives */
    CHECK(convo_active(&c, B, 61 * MIN), "B again a minute later: bare");
    CHECK(!convo_active(&c, A, 61 * MIN), "A after B: announced again");

    /* A restart forgets everything. */
    convo_init(&c);
    CHECK(!convo_active(&c, B, 61 * MIN), "after a restart: announced");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        g_verbose = 1;
    }
    if (argc > 1 && strcmp(argv[1], "-vv") == 0) {
        g_verbose = 2;
    }
    test_morse();
    test_hseq();
    test_play_timeline();
    test_paris();
    test_scenarios();
    test_hesitation();
    test_error_rollback();
    test_phantom();
    test_sos();
    test_collapse_recovery();
    test_sos_sweep();
    test_convo();
    test_open_pattern();
    test_link_all();
    test_phone_all();
    test_ttest_all();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
