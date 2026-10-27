/*
 * MorseLink — haptic sequences (see hseq.h).
 *
 * A job (a short list of tones, Morse text and gaps) becomes a stream of
 * segments, each "M1 at level a, M2 at level b, for n ms". The generator
 * works lazily: hseq_next() hands out one segment at a time and refills a
 * small ring buffer (HSEQ_BUF) with the segments of one more element, tone
 * or gap only when it runs empty, so a whole message never has to be laid
 * out in memory. The player (haptics.c) walks the stream with esp_timer.
 *
 * Pure C (no ESP-IDF): the host tests compile it too.
 */
#include "hseq.h"

#include <string.h>

#include "morse.h"

#define F HLEVEL_FULL  /* a motor at full level */
#define Q HLEVEL_QUIET /* a motor at the quiet level (unverified peer's pip) */
#define PIP 80         /* ms: a system tone's short buzz */
#define LONG 300       /* ms: a system tone's long buzz */

/* One system tone: up to 12 segments, played as they stand. */
typedef struct {
    uint8_t n;
    hseg_t seg[12];
} tone_def_t;

/* System tones (spec §9): both motors together, so a tone can never be
 * mistaken for a Morse element (those use M1 alone, or M1 then M2).
 * Pip = 80 ms, long = 300 ms. */
static const tone_def_t s_tones[TONE__COUNT] = {
    [TONE_PIP] = {1, {{PIP, F, F}}},
    [TONE_SEND_ARMED] = {1, {{PIP, F, F}}},
    [TONE_CMD_ARMED] = {3, {{PIP, F, F}, {100, 0, 0}, {PIP, F, F}}},
    [TONE_CANCEL] = {1, {{LONG, F, F}}},
    [TONE_OK] = {3, {{PIP, F, F}, {100, 0, 0}, {LONG, F, F}}},
    [TONE_ERROR] = {5, {{PIP, F, F}, {60, 0, 0}, {PIP, F, F}, {60, 0, 0}, {PIP, F, F}}},
    [TONE_DEGRADED] = {3, {{LONG, F, F}, {100, 0, 0}, {PIP, F, F}}},
    [TONE_ATTENTION] = {7, {{LONG, F, F}, {150, 0, 0}, {PIP, F, F}, {100, 0, 0}, {PIP, F, F}, {100, 0, 0}, {PIP, F, F}}},
    [TONE_PENDING] = {5, {{PIP, F, F}, {400, 0, 0}, {PIP, F, F}, {400, 0, 0}, {PIP, F, F}}},
    [TONE_DELIVERED] = {3, {{PIP, F, F}, {500, 0, 0}, {PIP, F, F}}},
    [TONE_SOS_ARMED] = {5, {{500, F, F}, {200, 0, 0}, {500, F, F}, {200, 0, 0}, {500, F, F}}},
    [TONE_SOS_ALARM] = {9,
                        {{1000, F, F},
                         {300, 0, 0},
                         {1000, F, F},
                         {300, 0, 0},
                         {1000, F, F},
                         {300, 0, 0},
                         {1000, F, F},
                         {300, 0, 0},
                         {1000, F, F}}},
    [TONE_QUIET_PIP] = {1, {{PIP, Q, Q}}},
};

/* Q1 (slowest) .. Q9 (fastest). Coin ERMs need ~30-50 ms to spin up and coast
 * ~50-80 ms, so units much below ~120 ms smear dots together. */
static const uint16_t s_speed_table[9] = {320, 280, 250, 220, 200, 180, 160, 140, 120};

/* Vibration strength I1 .. I9 (hseq.h says why): the drive (PWM duty, %)
 * and the motor supply (DCDC1, mV). */
static const uint8_t s_strength_pct[HSTRENGTH_MAX] = {45, 52, 59, 66, 74, 82, 91, 100, 100};
static const uint16_t s_strength_mv[HSTRENGTH_MAX] = {3300, 3300, 3300, 3300, 3300, 3300, 3300, 3300, 3400};

/* hspeed_unit_ms() -- the reading speed's unit (one dot) in ms for a Q digit;
 * out-of-range digits are clamped to 1..9. */
uint16_t hspeed_unit_ms(int q)
{
    if (q < 1) {
        q = 1;
    }
    if (q > 9) {
        q = 9;
    }
    return s_speed_table[q - 1];
}

/* hstrength_mv() -- the motor supply's voltage for a strength level (I);
 * anything outside 1..9 (0 = never set) gives the default level's 3.3 V. */
uint16_t hstrength_mv(int level)
{
    if (level < HSTRENGTH_MIN || level > HSTRENGTH_MAX) {
        level = HSTRENGTH_DEFAULT;
    }
    return s_strength_mv[level - 1];
}

/* hstrength_drive_pct() -- the drive (% of full) for a strength level (I);
 * anything outside 1..9 gives the default level's 100 %. */
uint8_t hstrength_drive_pct(int level)
{
    if (level < HSTRENGTH_MIN || level > HSTRENGTH_MAX) {
        level = HSTRENGTH_DEFAULT;
    }
    return s_strength_pct[level - 1];
}

/* hparams_default() -- the factory playback parameters: Q5 (200 ms unit),
 * a dash of M1 1.4U and M2 1.9U overlapping by 0.3U (v0.6.4), standard
 * spacing, 350 ms between a tone and what follows it, full drive. */
void hparams_default(hparams_t *p)
{
    p->unit_ms = hspeed_unit_ms(5);
    p->overlap_cu = 30;
    p->dash_m1_cu = 125;
    p->farnsworth_x10 = 10;
    p->item_gap_ms = 350;
    p->dash_style = HDASH_SWEEP;
    p->drive_pct = 100;
    p->kick_ms = 0;
}

/* hjob_init() -- an empty job. */
void hjob_init(hjob_t *j)
{
    memset(j, 0, sizeof(*j));
}

/* hjob_tone() -- append a system tone. False if the job is full or the
 * tone unknown (the job is then unchanged). */
bool hjob_tone(hjob_t *j, tone_t t)
{
    if (j->n >= HJOB_MAX_ITEMS || t >= TONE__COUNT) {
        return false;
    }
    j->items[j->n++] = (hitem_t){.kind = HI_TONE, .tone = (uint8_t)t, .arg = 0};
    return true;
}

/* hjob_morse() -- append Morse text (copied into the job's text area, NUL-
 * terminated; spaces become word gaps when it plays). False if the job has
 * no item or no text room left. */
bool hjob_morse(hjob_t *j, const char *text)
{
    size_t len = strlen(text);
    if (j->n >= HJOB_MAX_ITEMS || j->text_len + len + 1 > HJOB_TEXT_MAX) {
        return false;
    }
    memcpy(&j->text[j->text_len], text, len + 1);
    j->items[j->n++] = (hitem_t){.kind = HI_MORSE, .tone = 0, .arg = j->text_len};
    j->text_len += (uint16_t)(len + 1);
    return true;
}

/* hjob_gap() -- append a silence of `ms`. It replaces the automatic gap
 * that would otherwise separate its neighbours. False if the job is full. */
bool hjob_gap(hjob_t *j, uint16_t ms)
{
    if (j->n >= HJOB_MAX_ITEMS) {
        return false;
    }
    j->items[j->n++] = (hitem_t){.kind = HI_GAP, .tone = 0, .arg = ms};
    return true;
}

/* ------------------------------------------------------------------------- */
/* The generator                                                              */
/* ------------------------------------------------------------------------- */

/* push_raw() -- add one segment to the ring buffer as it stands (zero-
 * length segments are skipped; longer than 65.5 s is cut, which nothing asks
 * for). Every pushed millisecond counts in pushed_ms, which
 * hjob_item_starts() reads. */
static void push_raw(hseq_t *s, uint32_t ms, uint8_t m1, uint8_t m2)
{
    if (ms == 0 || s->count >= HSEQ_BUF) {
        return;
    }
    if (ms > 65535) {
        ms = 65535;
    }
    s->buf[s->tail] = (hseg_t){(uint16_t)ms, m1, m2};
    s->tail = (uint8_t)((s->tail + 1) % HSEQ_BUF);
    s->count++;
    s->pushed_ms += ms;
}

/* scaled() -- a motor level (0-255) at the drive set (strength, I). */
static uint8_t scaled(const hseq_t *s, uint8_t level)
{
    return (uint8_t)(((uint32_t)level * s->p.drive_pct + 50u) / 100u);
}

/*
 * push() -- add a segment "M1 at m1, M2 at m2 for ms" (levels as asked for,
 * 255 = full) at the drive the strength sets. Below full drive, a motor
 * that comes on (it was off in the segment before) first runs at full for
 * kick_ms, then at the lower level; a kick longer than the segment carries
 * on into the next ones while the motor stays on. The segment is split
 * where a kick ends, so its total length never changes: the timing is the
 * same at every strength.
 */
static void push(hseq_t *s, uint32_t ms, uint8_t m1, uint8_t m2)
{
    if (ms == 0) {
        return;
    }
    if (s->p.drive_pct >= 100) {
        push_raw(s, ms, m1, m2); /* full drive: as asked, no kick needed */
        s->on1 = m1 != 0;
        s->on2 = m2 != 0;
        return;
    }
    /* A motor coming on starts its kick; one going off loses what is left. */
    if (m1 && !s->on1) {
        s->kick1 = s->p.kick_ms;
    }
    if (!m1) {
        s->kick1 = 0;
    }
    if (m2 && !s->on2) {
        s->kick2 = s->p.kick_ms;
    }
    if (!m2) {
        s->kick2 = 0;
    }
    s->on1 = m1 != 0;
    s->on2 = m2 != 0;
    while (ms > 0) {
        uint32_t piece = ms; /* up to the next kick's end */
        if (s->kick1 && s->kick1 < piece) {
            piece = s->kick1;
        }
        if (s->kick2 && s->kick2 < piece) {
            piece = s->kick2;
        }
        push_raw(s, piece, s->kick1 ? HLEVEL_FULL : scaled(s, m1), s->kick2 ? HLEVEL_FULL : scaled(s, m2));
        s->kick1 = s->kick1 > piece ? (uint16_t)(s->kick1 - piece) : 0;
        s->kick2 = s->kick2 > piece ? (uint16_t)(s->kick2 - piece) : 0;
        ms -= piece;
    }
}

/* letter_gap() -- the silence between two characters: 3U, stretched by the
 * Farnsworth factor. */
static uint32_t letter_gap(const hseq_t *s)
{
    return 3u * s->p.unit_ms * s->p.farnsworth_x10 / 10u;
}

/* word_gap() -- the silence between two words: 7U, stretched likewise. */
static uint32_t word_gap(const hseq_t *s)
{
    return 7u * s->p.unit_ms * s->p.farnsworth_x10 / 10u;
}

/* push_element() -- one Morse element: a dot is M1 alone for 1U; a dash is
 * 3U that starts on M1 (the wrist) and ends on M2 (the forearm), with an
 * overlap where both run, so it feels like a sweep up the arm (by default
 * M1 1.1U, both 0.3U, M2 1.6U); or, with dash_style HDASH_M2 (H2), M2 alone
 * for 3U. */
static void push_element(hseq_t *s, char e)
{
    uint32_t u = s->p.unit_ms;
    if (e == '.') {
        push(s, u, F, 0);
        return;
    }
    if (s->p.dash_style == HDASH_M2) {
        push(s, 3u * u, 0, F); /* H2: the whole dash on the forearm, so the motor alone says dot or dash */
        return;
    }
    /* Dash: leading pulse at the wrist, completion at the forearm. */
    uint32_t total = 3u * u;
    uint32_t m1_share = u * s->p.dash_m1_cu / 100u;
    uint32_t ov = u * s->p.overlap_cu / 100u; /* (v0.6.4: in units, so it scales with the reading speed) */
    if (ov > m1_share) {
        ov = m1_share; /* the overlap can't be longer than M1's own part ... */
    }
    if (ov > total - m1_share) {
        ov = total - m1_share; /* ... or M2's */
    }
    uint32_t a = m1_share - ov / 2; /* M1 alone */
    uint32_t b = ov;                /* both */
    uint32_t c = total - a - b;     /* M2 alone: the three always add up to 3U */
    push(s, a, F, 0);
    push(s, b, F, F);
    push(s, c, 0, F);
}

/* next_item() -- move on to the job's next item and push the automatic gap
 * between the two: a word gap between two Morse items, the item gap
 * between a tone and anything else, none next to an explicit gap item. */
static void next_item(hseq_t *s)
{
    const hjob_t *j = s->job;
    uint8_t cur = s->item;
    s->item++;
    s->pos = 0;
    s->el = 0;
    if (s->item >= j->n) {
        return; /* that was the last item: no trailing gap */
    }
    const hitem_t *a = &j->items[cur];
    const hitem_t *b = &j->items[s->item];
    if (a->kind == HI_GAP || b->kind == HI_GAP) {
        return; /* explicit gaps replace the automatic one */
    }
    if (a->kind == HI_MORSE && b->kind == HI_MORSE) {
        push(s, word_gap(s), 0, 0);
    } else {
        push(s, s->p.item_gap_ms, 0, 0);
    }
}

/* refill() -- with the buffer empty, push the next piece of the job: a whole
 * tone, a gap, or one Morse element with the gap after it. Loops past items
 * that push nothing (an empty Morse item, a zero gap) until something is in
 * the buffer or the job is over. */
static void refill(hseq_t *s)
{
    const hjob_t *j = s->job;
    while (s->count == 0 && s->item < j->n) {
        const hitem_t *it = &j->items[s->item];
        switch (it->kind) {
        case HI_TONE: {
            const tone_def_t *t = &s_tones[it->tone < TONE__COUNT ? it->tone : TONE_PIP];
            for (int i = 0; i < t->n; i++) {
                push(s, t->seg[i].ms, t->seg[i].m1, t->seg[i].m2);
            }
            next_item(s);
            break;
        }
        case HI_GAP:
            push(s, it->arg, 0, 0);
            s->item++;
            s->pos = 0;
            s->el = 0;
            break;
        case HI_MORSE: {
            const char *text = &j->text[it->arg];
            while (text[s->pos] == ' ') {
                s->pos++; /* leading (or repeated) spaces: their gap was pushed already */
            }
            if (!text[s->pos]) {
                next_item(s); /* the end of this item's text */
                break;
            }
            const char *pat = morse_encode(text[s->pos]);
            if (!pat) {
                pat = morse_encode('?'); /* a character without Morse plays as '?' */
            }
            if (s->starts && s->item < HJOB_MAX_ITEMS && s->starts[s->item] == HSEQ_NO_START) {
                /* The item's first element starts after everything pushed so
                 * far: noted for hjob_item_starts(). */
                s->starts[s->item] = s->pushed_ms;
            }
            push_element(s, pat[s->el]);
            s->el++;
            if (pat[s->el]) {
                push(s, s->p.unit_ms, 0, 0); /* intra-character gap */
            } else {
                /* The character is complete: on to the next one, with a
                 * letter gap, or a word gap if spaces come first (nothing
                 * after the item's last character). */
                s->el = 0;
                s->pos++;
                uint16_t k = s->pos;
                bool space = false;
                while (text[k] == ' ') {
                    space = true;
                    k++;
                }
                if (text[k]) {
                    push(s, space ? word_gap(s) : letter_gap(s), 0, 0);
                }
                s->pos = k;
            }
            break;
        }
        default:
            s->item++; /* unknown item kind: skip it */
            break;
        }
    }
}

/* hseq_start() -- set a generator up at the start of `job`, with the
 * parameters clamped to sane values (a unit of at least 20 ms, no squeezed
 * spacing, a dash split between 0.25U and 2.5U). The job must stay valid
 * while the generator runs. */
void hseq_start(hseq_t *s, const hjob_t *job, const hparams_t *p)
{
    memset(s, 0, sizeof(*s));
    s->job = job;
    s->p = *p;
    if (s->p.unit_ms < 20) {
        s->p.unit_ms = 20;
    }
    if (s->p.farnsworth_x10 < 10) {
        s->p.farnsworth_x10 = 10;
    }
    if (s->p.dash_m1_cu < 25 || s->p.dash_m1_cu > 250) {
        s->p.dash_m1_cu = 100;
    }
    if (s->p.dash_style != HDASH_M2) {
        s->p.dash_style = HDASH_SWEEP; /* (0 or anything unknown: the sweep) */
    }
    if (s->p.drive_pct == 0 || s->p.drive_pct > 100) {
        s->p.drive_pct = 100; /* (0 would play nothing at all) */
    }
}

/* hseq_next() -- the next segment into *out; false when the job is over. */
bool hseq_next(hseq_t *s, hseg_t *out)
{
    if (s->count == 0) {
        refill(s);
        if (s->count == 0) {
            return false;
        }
    }
    *out = s->buf[s->head];
    s->head = (uint8_t)((s->head + 1) % HSEQ_BUF);
    s->count--;
    return true;
}

/* hjob_duration_ms() -- how long a job plays, by running the generator to
 * the end and adding up its segments. */
uint32_t hjob_duration_ms(const hjob_t *j, const hparams_t *p)
{
    hseq_t s;
    hseg_t seg;
    uint32_t total = 0;
    hseq_start(&s, j, p);
    while (hseq_next(&s, &seg)) {
        total += seg.ms;
    }
    return total;
}

/* hjob_item_starts() -- see hseq.h. Runs the generator to the end with its
 * start notes switched on: refill() notes pushed_ms at each Morse item's
 * first element. Everything before that element has been pushed by then
 * (the buffer only refills when empty), so pushed_ms is exactly its start. */
void hjob_item_starts(const hjob_t *j, const hparams_t *p, uint32_t out[HJOB_MAX_ITEMS])
{
    for (int i = 0; i < HJOB_MAX_ITEMS; i++) {
        out[i] = HSEQ_NO_START;
    }
    hseq_t s;
    hseg_t seg;
    hseq_start(&s, j, p);
    s.starts = out;
    while (hseq_next(&s, &seg)) {
        /* (only the notes matter) */
    }
}
