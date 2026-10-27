/*
 * MorseLink — host tests for the phone protocol (phone.c): base64, the lines
 * each way, assembling lines from BLE chunks, and which topics the relay
 * publishes a packet to. (The relay as a whole, between simulated devices,
 * is tested in test_link.c.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "morse.h"
#include "phone.h"
#include "proto.h"

extern int g_fail, g_pass; /* shared with test_main.c */

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

/* Base64: known answers, round trips of every length a packet can have, and
 * everything the strict decoder must refuse. */
static void test_b64(void)
{
    printf("phone: base64\n");
    static const struct {
        const char *in, *out;
    } known[] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"foobar", "Zm9vYmFy"}};
    char enc[256];
    uint8_t dec[256];
    for (size_t k = 0; k < sizeof known / sizeof known[0]; k++) {
        int n = phone_b64_encode((const uint8_t *)known[k].in, (int)strlen(known[k].in), enc, sizeof enc);
        CHECK(n == (int)strlen(known[k].out) && strcmp(enc, known[k].out) == 0, "encode \"%s\" -> \"%s\"", known[k].in,
              enc);
        int m = phone_b64_decode(known[k].out, (int)strlen(known[k].out), dec, sizeof dec);
        CHECK(m == (int)strlen(known[k].in) && memcmp(dec, known[k].in, (size_t)m) == 0, "decode \"%s\"", known[k].out);
    }
    int ok = 0;
    srand(7);
    for (int len = 0; len <= PROTO_MAX_PKT; len++) {
        uint8_t raw[PROTO_MAX_PKT];
        for (int i = 0; i < len; i++) {
            raw[i] = (uint8_t)rand();
        }
        int n = phone_b64_encode(raw, len, enc, sizeof enc);
        int m = phone_b64_decode(enc, n, dec, sizeof dec);
        ok += n == PHONE_B64_LEN(len) && m == len && memcmp(raw, dec, (size_t)len) == 0;
    }
    CHECK(ok == PROTO_MAX_PKT + 1, "round trip of every length 0..%d (%d ok)", PROTO_MAX_PKT, ok);
    CHECK(phone_b64_decode("Zm9", 3, dec, sizeof dec) < 0, "length not a multiple of 4");
    CHECK(phone_b64_decode("Zm9!", 4, dec, sizeof dec) < 0, "a character outside the alphabet");
    CHECK(phone_b64_decode("Zg==Zm9v", 8, dec, sizeof dec) < 0, "padding in the middle");
    CHECK(phone_b64_decode("Z===", 4, dec, sizeof dec) < 0, "three padding characters");
    CHECK(phone_b64_decode("Zg=v", 4, dec, sizeof dec) < 0, "data after padding");
    CHECK(phone_b64_decode("Zm9vYmFy", 8, dec, 5) < 0, "output too small");
    CHECK(phone_b64_encode((const uint8_t *)"foo", 3, enc, 4) < 0, "encode: no room for the NUL");
}

/* The device -> phone lines, exactly as the app will read them. */
static void test_lines_out(void)
{
    printf("phone: lines to the phone\n");
    char s[PHONE_LINE_MAX + 1];
    uint32_t k7q = id_to_addr("K7Q"), abc = id_to_addr("ABC");
    phone_fmt_hi(s, sizeof s, k7q, "0.4.0");
    CHECK(strcmp(s, "HI K7Q 4 0.4.0") == 0, "%s", s);
    phone_fmt_status(s, sizeof s, abc, "connected", 87, false, "lost", "connected");
    CHECK(strcmp(s, "ST ABC connected 87 0 lost connected") == 0, "%s", s);
    phone_fmt_status(s, sizeof s, ADDR_NONE, "none", -1, true, "none", "off");
    CHECK(strcmp(s, "ST - none ? 1 none off") == 0, "%s", s);
    /* Protocol 2 (v0.6.0): the radio, command mode, events with a detail. */
    phone_fmt_radio(s, sizeof s, 12, 125, 22);
    CHECK(strcmp(s, "RD 12 125 22") == 0, "%s", s);
    phone_fmt_cmd(s, sizeof s, "");
    CHECK(strcmp(s, "CM") == 0, "%s", s);
    phone_fmt_cmd(s, sizeof s, "SK7Q");
    CHECK(strcmp(s, "CM SK7Q") == 0, "%s", s);
    phone_fmt_cmd_end(s, sizeof s, "done", "G4");
    CHECK(strcmp(s, "CE done G4") == 0, "%s", s);
    phone_fmt_cmd_end(s, sizeof s, "timeout", "");
    CHECK(strcmp(s, "CE timeout") == 0, "%s", s);
    phone_fmt_event_detail(s, sizeof s, PHONE_EV_LOST, abc, "no-receipt");
    CHECK(strcmp(s, "EV LOST ABC no-receipt") == 0, "%s", s);
    phone_fmt_event_detail(s, sizeof s, PHONE_EV_LORA, abc, "they-cant-hear");
    CHECK(strcmp(s, "EV LORA ABC they-cant-hear") == 0, "%s", s);
    phone_fmt_event_detail(s, sizeof s, PHONE_EV_RESTORED, abc, NULL);
    CHECK(strcmp(s, "EV RESTORED ABC") == 0, "%s", s);
    phone_fmt_hi(s, sizeof s, abc, "0.6.2");
    CHECK(strcmp(s, "HI ABC 4 0.6.2") == 0, "protocol 4: %s", s);
    /* Protocol 3 (v0.6.1): the meter session's BN lines, W's EV BLE_OFF. */
    phone_fmt_bench(s, sizeof s, "start", "543 22");
    CHECK(strcmp(s, "BN start 543 22") == 0, "%s", s);
    phone_fmt_bench(s, sizeof s, "wait-usb", NULL);
    CHECK(strcmp(s, "BN wait-usb") == 0, "%s", s);
    phone_fmt_bench(s, sizeof s, "step", "3b 60");
    CHECK(strcmp(s, "BN step 3b 60") == 0, "%s", s);
    phone_fmt_event(s, sizeof s, PHONE_EV_BLE_OFF, abc);
    CHECK(strcmp(s, "EV BLE_OFF ABC") == 0, "%s", s);
    phone_fmt_draft(s, sizeof s, "HELLO WO");
    CHECK(strcmp(s, "DR HELLO WO") == 0, "%s", s);
    phone_fmt_draft(s, sizeof s, "");
    CHECK(strcmp(s, "DR") == 0, "empty draft: \"%s\"", s);
    phone_fmt_tx(s, sizeof s, 3, abc, "HI THERE");
    CHECK(strcmp(s, "TX 3 ABC HI THERE") == 0, "%s", s);
    phone_fmt_result(s, sizeof s, 3, true);
    CHECK(strcmp(s, "OK 3") == 0, "%s", s);
    phone_fmt_result(s, sizeof s, 4, false);
    CHECK(strcmp(s, "NO 4") == 0, "%s", s);
    phone_fmt_rx(s, sizeof s, k7q, "MEET AT SIX");
    CHECK(strcmp(s, "RX K7Q MEET AT SIX") == 0, "%s", s);
    phone_fmt_event(s, sizeof s, PHONE_EV_SOS_SENT, ADDR_BROADCAST);
    CHECK(strcmp(s, "EV SOS_SENT ALL") == 0, "%s", s);
    phone_fmt_event(s, sizeof s, PHONE_EV_LOST, k7q);
    CHECK(strcmp(s, "EV LOST K7Q") == 0, "%s", s);
    int named = 0;
    for (int e = 0; e < PHONE_EV__COUNT; e++) {
        named += strcmp(phone_ev_name((uint8_t)e), "?") != 0;
    }
    CHECK(named == PHONE_EV__COUNT && strcmp(phone_ev_name(PHONE_EV__COUNT), "?") == 0, "every event has a name");

    /* The longest lines still fit: a 100-character message, and a relay of
     * the biggest packet. */
    char text[MSG_MAX_CHARS + 1];
    memset(text, 'E', MSG_MAX_CHARS);
    text[MSG_MAX_CHARS] = '\0';
    CHECK(phone_fmt_tx(s, sizeof s, 65535, id_to_addr("2345"), text) > 0, "a full message fits in a TX line");
    uint8_t big[PROTO_MAX_PKT];
    memset(big, 0xA5, sizeof big);
    int n = phone_fmt_relay(s, sizeof s, abc, big, sizeof big);
    CHECK(n > 0 && n <= PHONE_LINE_MAX && strncmp(s, "RL ABC ", 7) == 0, "the biggest packet fits in an RL line (%d)", n);
    char tiny[8];
    CHECK(phone_fmt_rx(tiny, sizeof tiny, k7q, "TOO LONG FOR THIS") < 0 && tiny[0] == '\0',
          "a line that doesn't fit is refused whole, never cut");
}

/* Protocol 4 (v0.6.2): the learning view's lines. */
static void test_lines_learning(void)
{
    printf("phone: the learning view (KY, KC, CB, SP, PL)\n");
    char s[PHONE_LINE_MAX + 1];
    phone_fmt_key(s, sizeof s, "key", ".-", 540, 330);
    CHECK(strcmp(s, "KY key .- 540 330") == 0, "%s", s);
    phone_fmt_key(s, sizeof s, "cal", ".--..-", 120, 300);
    CHECK(strcmp(s, "KY cal .--..- 120 300") == 0, "%s", s);
    phone_fmt_key(s, sizeof s, NULL, NULL, 0, 0);
    CHECK(strcmp(s, "KY") == 0, "nothing open: \"%s\"", s);
    phone_fmt_key(s, sizeof s, "key", "", 80, 300);
    CHECK(strcmp(s, "KY") == 0, "an empty pattern is \"nothing open\": \"%s\"", s);
    phone_fmt_key_char(s, sizeof s, "char", ".-", 'A');
    CHECK(strcmp(s, "KC char .- A") == 0, "%s", s);
    phone_fmt_key_char(s, sizeof s, "unknown", ".-.--", '?');
    CHECK(strcmp(s, "KC unknown .-.-- ?") == 0, "%s", s);
    phone_fmt_key_char(s, sizeof s, "error", "........", 0);
    CHECK(strcmp(s, "KC error ........") == 0, "%s", s);
    phone_fmt_cal_result(s, sizeof s, true, "count");
    CHECK(strcmp(s, "CB ok") == 0, "%s", s);
    phone_fmt_cal_result(s, sizeof s, false, "gaps");
    CHECK(strcmp(s, "CB retry gaps") == 0, "%s", s);
    phone_fmt_cal_result(s, sizeof s, false, NULL);
    CHECK(strcmp(s, "CB retry other") == 0, "%s", s);
    phone_speeds_t sp = {.valid = true,
                         .calibrating = false,
                         .dot_ms = 180,
                         .dash_ms = 560,
                         .gap_in_ms = 170,
                         .gap_letter_ms = 620,
                         .boundary_ms = 330,
                         .letter_ms = 420,
                         .word_ms = 1300,
                         .health = "healthy",
                         .q = 5,
                         .unit_ms = 200,
                         .fx10 = 10,
                         .strength = 4,
                         .strength_mv = 3300,
                         .dash_style = 2};
    phone_fmt_speeds(s, sizeof s, &sp);
    CHECK(strcmp(s, "SP 1 0 180 560 170 620 330 420 1300 healthy 5 200 10 4 3300 2") == 0, "%s", s);
    sp.health = NULL;
    sp.valid = false;
    sp.calibrating = true;
    phone_fmt_speeds(s, sizeof s, &sp);
    CHECK(strncmp(s, "SP 0 1 ", 7) == 0 && strstr(s, " none 5 ") != NULL, "no health: \"none\": %s", s);
    phone_fmt_play(s, sizeof s, 0, 1250, 200, 10, "DEA");
    CHECK(strcmp(s, "PL 0 1250 200 10 DEA") == 0, "%s", s);
    phone_fmt_play(s, sizeof s, 1, 4400, 250, 20, "HELLO THERE ");
    CHECK(strcmp(s, "PL 1 4400 250 20 HELLO THERE") == 0, "no trailing space: \"%s\"", s);
    phone_fmt_play_end(s, sizeof s);
    CHECK(strcmp(s, "PL end") == 0, "%s", s);
    /* A text longer than the line is cut to fit, never refused; an empty one isn't sent. */
    char text[260];
    memset(text, 'E', sizeof text - 1);
    text[sizeof text - 1] = '\0';
    int n = phone_fmt_play(s, sizeof s, 0, 1000, 120, 10, text);
    CHECK(n == PHONE_LINE_MAX && (int)strlen(s) == n && strncmp(s, "PL 0 1000 120 10 EEE", 20) == 0,
          "a long text is cut to the line (%d)", n);
    CHECK(phone_fmt_play(s, sizeof s, 0, 0, 200, 10, "") < 0 && s[0] == '\0', "an empty text makes no line");
    char tiny[12];
    CHECK(phone_fmt_play(tiny, sizeof tiny, 0, 123456, 200, 10, "HI") < 0 && tiny[0] == '\0',
          "no room for any text: no line");
    /* The longest KY (a 20-press PARIS attempt) and the longest SP fit easily. */
    char pat[21];
    memset(pat, '-', 20);
    pat[20] = '\0';
    CHECK(phone_fmt_key(s, sizeof s, "cal", pat, 1499, 1499) > 0, "a full PARIS attempt fits in a KY line");
    sp.dot_ms = sp.dash_ms = sp.gap_in_ms = sp.gap_letter_ms = sp.boundary_ms = sp.letter_ms = sp.word_ms = 99999;
    sp.health = "collapsed";
    n = phone_fmt_speeds(s, sizeof s, &sp);
    CHECK(n > 0 && n < 96, "the longest SP line fits the UI's 96-byte copy (%d)", n);
}

/* The phone -> device lines. */
static void test_lines_in(void)
{
    printf("phone: lines from the phone\n");
    phone_in_t in;
    phone_parse("HI", &in);
    CHECK(in.kind == PHONE_IN_HI, "HI");
    phone_parse("HI MorseLinkApp 1.0", &in);
    CHECK(in.kind == PHONE_IN_HI, "HI with words after it");
    phone_parse("HIGH", &in);
    CHECK(in.kind == PHONE_IN_UNKNOWN, "HIGH is not HI");
    phone_parse("XX something", &in);
    CHECK(in.kind == PHONE_IN_UNKNOWN, "unknown lines are ignored");

    /* A real packet through the relay line, as the receiving phone hands it down. */
    pkt_t p, q;
    proto_init_pkt(&p, PT_MSG, id_to_addr("K7Q"), id_to_addr("ABC"), 4711);
    memcpy(p.payload, "HELLO", 5);
    p.plen = 5;
    uint8_t raw[PROTO_MAX_PKT];
    int len = proto_encode(&p, raw, sizeof raw);
    char out[PHONE_LINE_MAX + 1], back[PHONE_LINE_MAX + 2];
    phone_fmt_relay(out, sizeof out, p.dst, raw, len); /* "RL ABC <b64>" from the sender's device */
    snprintf(back, sizeof back, "RL %s\r", out + 7);  /* "RL <b64>" to the receiver's device, CR and all */
    phone_parse(back, &in);
    CHECK(in.kind == PHONE_IN_RELAY && in.len == len && memcmp(in.pkt, raw, (size_t)len) == 0 &&
              proto_decode(in.pkt, in.len, &q) == PROTO_OK && q.id == 4711,
          "a relayed packet comes back byte for byte");
    phone_parse("RL ####", &in);
    CHECK(in.kind == PHONE_IN_BAD, "RL with bad base64 is flagged");
    phone_parse("RL ", &in);
    CHECK(in.kind == PHONE_IN_BAD, "RL with nothing");

    /* Test mode commands. */
    phone_parse("TEST START test=dynamic paths=both role=base session=20261014-101500 seed=99\r", &in);
    CHECK(in.kind == PHONE_IN_TEST_START &&
              strcmp(in.args, "test=dynamic paths=both role=base session=20261014-101500 seed=99") == 0,
          "TEST START with its parameters (\"%s\")", in.args);
    phone_parse("TEST START", &in);
    CHECK(in.kind == PHONE_IN_TEST_START && in.args[0] == '\0', "TEST START without parameters");
    phone_parse("TEST STOP", &in);
    CHECK(in.kind == PHONE_IN_TEST_STOP, "TEST STOP");
    phone_parse("BENCH POWER", &in);
    CHECK(in.kind == PHONE_IN_BENCH_POWER, "BENCH POWER (protocol 3)");
    phone_parse("BENCH STOP\r", &in);
    CHECK(in.kind == PHONE_IN_BENCH_STOP, "BENCH STOP");
    phone_parse("BENCH POWERFUL", &in);
    CHECK(in.kind == PHONE_IN_UNKNOWN, "BENCH POWERFUL is not BENCH POWER");
    char pt[PHONE_LINE_MAX + 1];
    snprintf(pt, sizeof pt, "TEST START test=point paths=lora role=rover session=20261021-101500 seed=4294967295 "
                            "plan=9:2,12:22,9:22,10:17,11:20,12:17,9:20,10:22 n=20");
    phone_parse(pt, &in);
    CHECK(in.kind == PHONE_IN_TEST_START && strlen(in.args) > 100 && strstr(in.args, "n=20"),
          "a point test's long parameters fit (%zu characters)", strlen(in.args));
    phone_parse("TEST STARTLE", &in);
    CHECK(in.kind == PHONE_IN_UNKNOWN, "TEST STARTLE is not TEST START");
    char lng[PHONE_ARGS_MAX + 40] = "TEST START ";
    memset(lng + 11, 'x', PHONE_ARGS_MAX + 10);
    lng[11 + PHONE_ARGS_MAX + 10] = '\0';
    phone_parse(lng, &in);
    CHECK(in.kind == PHONE_IN_BAD, "over-long parameters refused");
}

/* Lines from BLE chunks of any size, CRLF, empty and over-long lines. */
static void test_assembly(void)
{
    printf("phone: lines from BLE chunks\n");
    phone_rx_t r;
    phone_rx_init(&r);
    const char *stream = "HI\r\n\nRL QUJD\nST?\n";
    const char *want[] = {"HI", "RL QUJD", "ST?"};
    int got = 0, right = 0;
    for (const char *c = stream; *c; c++) {
        if (phone_rx_byte(&r, (uint8_t)*c)) {
            right += got < 3 && strcmp(r.buf, want[got]) == 0;
            got++;
        }
    }
    CHECK(got == 3 && right == 3, "three lines, the empty one skipped (%d, %d right)", got, right);

    phone_rx_init(&r);
    got = 0;
    for (int i = 0; i < PHONE_LINE_MAX + 60; i++) {
        got += phone_rx_byte(&r, 'X');
    }
    got += phone_rx_byte(&r, '\n');
    const char *after = "HI\n";
    int hi = 0;
    for (const char *c = after; *c; c++) {
        if (phone_rx_byte(&r, (uint8_t)*c)) {
            hi += strcmp(r.buf, "HI") == 0;
        }
    }
    CHECK(got == 0 && hi == 1, "an over-long line is dropped whole; the next one is fine");
}

/* Which topics the relay publishes a packet to. */
static void test_relay_targets(void)
{
    printf("phone: relay targets\n");
    uint32_t own = id_to_addr("K7Q"), b = id_to_addr("ABC"), c = id_to_addr("XYZ"), d = id_to_addr("MNP");
    uint32_t contacts[] = {c, b, d, own};
    uint32_t to[PHONE_RELAY_MAX];
    uint8_t raw[PROTO_MAX_PKT];
    pkt_t p;

    /* targets() -- encode p and ask for its topics (partner b). */
#define TARGETS(max) phone_relay_targets(raw, proto_encode(&p, raw, sizeof raw), own, contacts, 4, b, to, (max))

    proto_init_pkt(&p, PT_MSG, own, c, 1);
    p.payload[0] = 'A';
    p.plen = 1;
    CHECK(TARGETS(PHONE_RELAY_MAX) == 1 && to[0] == c, "a message: its recipient's topic");
    proto_init_pkt(&p, PT_KEEPALIVE, own, b, 2);
    CHECK(TARGETS(PHONE_RELAY_MAX) == 1 && to[0] == b, "a keepalive: the partner's topic");
    proto_init_pkt(&p, PT_SOS, own, ADDR_BROADCAST, 3);
    int n = TARGETS(PHONE_RELAY_MAX);
    CHECK(n == 3 && to[0] == b && to[1] == c && to[2] == d, "a general SOS: partner, then each contact once, not us (%d)",
          n);
    CHECK(TARGETS(2) == 2, "never more topics than asked for");
    proto_init_pkt(&p, PT_SOS_CANCEL, own, ADDR_BROADCAST, 4);
    CHECK(TARGETS(PHONE_RELAY_MAX) == 3, "a general cancel goes the same way");
    proto_init_pkt(&p, PT_SOS_ACK, own, d, 5);
    CHECK(TARGETS(PHONE_RELAY_MAX) == 1 && to[0] == d, "an SOS acknowledgement: the SOS sender's topic");

    proto_init_pkt(&p, PT_DISCOVER, own, ADDR_BROADCAST, 6);
    p.payload[0] = 16, p.payload[1] = 19, p.plen = 2;
    CHECK(TARGETS(PHONE_RELAY_MAX) == 0, "discovery stays on the radio");
    proto_init_pkt(&p, PT_PRESENCE, own, c, 7);
    p.plen = 2;
    CHECK(TARGETS(PHONE_RELAY_MAX) == 0, "so do discovery answers");
    proto_init_pkt(&p, PT_PING, own, ADDR_BROADCAST, 8);
    p.plen = 4;
    CHECK(TARGETS(PHONE_RELAY_MAX) == 0, "and pings");
    proto_init_pkt(&p, PT_SOS, c, ADDR_BROADCAST, 9);
    p.flags = 1; /* someone else's SOS we are relaying */
    CHECK(TARGETS(PHONE_RELAY_MAX) == 0, "a relayed SOS from someone else stays on the radio");
    CHECK(phone_relay_targets(raw, 5, own, contacts, 4, b, to, PHONE_RELAY_MAX) == 0, "garbage: nowhere");
#undef TARGETS
}

/* The TX tracker (OK n / NO n for the phone): numbers count from 1; a
 * receipt settles the oldest message for that recipient, a refusal the
 * newest, a failure all of them, forgetting someone only theirs. */
static void test_tx_tracker(void)
{
    printf("phone: TX numbers until OK / NO\n");
    phone_tx_t t;
    phone_tx_init(&t);
    uint32_t a = id_to_addr("K7Q"), b = id_to_addr("ABC");
    unsigned d[PHONE_TX_MAX];
    unsigned n1 = phone_tx_add(&t, a), n2 = phone_tx_add(&t, a), n3 = phone_tx_add(&t, a);
    CHECK(n1 == 1 && n2 == 2 && n3 == 3, "numbered from 1 (%u %u %u)", n1, n2, n3);
    int k = phone_tx_settle(&t, PHONE_TX_DELIVERED, a, d, PHONE_TX_MAX);
    CHECK(k == 1 && d[0] == 1, "a receipt settles the oldest (%d: %u)", k, k ? d[0] : 0);
    k = phone_tx_settle(&t, PHONE_TX_REFUSED, a, d, PHONE_TX_MAX);
    CHECK(k == 1 && d[0] == 3, "a refusal settles the newest (%d: %u)", k, k ? d[0] : 0);
    k = phone_tx_settle(&t, PHONE_TX_DELIVERED, a, d, PHONE_TX_MAX);
    CHECK(k == 1 && d[0] == 2 && t.n_pend == 0, "then 2 is delivered and nothing waits");
    CHECK(phone_tx_settle(&t, PHONE_TX_DELIVERED, a, d, PHONE_TX_MAX) == 0, "a stray receipt settles nothing");
    CHECK(phone_tx_settle(&t, PHONE_TX_REFUSED, b, d, PHONE_TX_MAX) == 0, "nor does a refusal with nothing waiting");
    phone_tx_add(&t, a); /* 4 */
    phone_tx_add(&t, b); /* 5 */
    phone_tx_add(&t, a); /* 6 */
    CHECK(phone_tx_settle(&t, PHONE_TX_DELIVERED, b, d, PHONE_TX_MAX) == 1 && d[0] == 5,
          "a receipt from ABC settles ABC's message, not the older one for K7Q");
    phone_tx_add(&t, b); /* 7 */
    k = phone_tx_settle(&t, PHONE_TX_DROPPED, a, d, PHONE_TX_MAX);
    CHECK(k == 2 && d[0] == 4 && d[1] == 6 && t.n_pend == 1 && t.pend[0].n == 7,
          "forgetting K7Q settles 4 and 6, keeps 7 (%d)", k);
    phone_tx_add(&t, a); /* 8 */
    k = phone_tx_settle(&t, PHONE_TX_ALL_FAILED, ADDR_NONE, d, PHONE_TX_MAX);
    CHECK(k == 2 && d[0] == 7 && d[1] == 8 && t.n_pend == 0, "a failure settles everything waiting, oldest first");
    for (int i = 0; i < PHONE_TX_MAX + 2; i++) {
        phone_tx_add(&t, a);
    }
    CHECK(t.n_pend == PHONE_TX_MAX && t.pend[0].n == t.last - PHONE_TX_MAX + 1 && t.last == 8 + PHONE_TX_MAX + 2,
          "full: the oldest is forgotten, the numbers go on");
    k = phone_tx_settle(&t, PHONE_TX_ALL_FAILED, ADDR_NONE, d, 2);
    CHECK(k == 2 && t.n_pend == 0, "more settled than done[] holds: reported up to max, all settled");
}

/* test_phone_all() -- every phone-protocol test, called from main(). */
void test_phone_all(void)
{
    test_b64();
    test_lines_out();
    test_lines_learning();
    test_lines_in();
    test_assembly();
    test_relay_targets();
    test_tx_tracker();
}
