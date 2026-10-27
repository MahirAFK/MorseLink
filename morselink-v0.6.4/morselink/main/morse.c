/*
 * MorseLink — Morse code table and device-ID codec.
 */
#include "morse.h"

#include <ctype.h>
#include <string.h>

typedef struct {
    char c;
    const char *p;
} morse_entry_t;

/* ITU-R M.1677 plus the common extras. '&' doubles as the AS ("wait") prosign,
 * '+' as AR and '=' as BT; they are sent as ordinary characters. */
static const morse_entry_t s_table[] = {
    {'A', ".-"},     {'B', "-..."},   {'C', "-.-."},   {'D', "-.."},    {'E', "."},
    {'F', "..-."},   {'G', "--."},    {'H', "...."},   {'I', ".."},     {'J', ".---"},
    {'K', "-.-"},    {'L', ".-.."},   {'M', "--"},     {'N', "-."},     {'O', "---"},
    {'P', ".--."},   {'Q', "--.-"},   {'R', ".-."},    {'S', "..."},    {'T', "-"},
    {'U', "..-"},    {'V', "...-"},   {'W', ".--"},    {'X', "-..-"},   {'Y', "-.--"},
    {'Z', "--.."},   {'0', "-----"},  {'1', ".----"},  {'2', "..---"},  {'3', "...--"},
    {'4', "....-"},  {'5', "....."},  {'6', "-...."},  {'7', "--..."},  {'8', "---.."},
    {'9', "----."},  {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'\'', ".----."},
    {'!', "-.-.--"}, {'/', "-..-."},  {'(', "-.--."},  {')', "-.--.-"}, {'&', ".-..."},
    {':', "---..."}, {';', "-.-.-."}, {'=', "-...-"},  {'+', ".-.-."},  {'-', "-....-"},
    {'_', "..--.-"}, {'"', ".-..-."}, {'$', "...-..-"}, {'@', ".--.-."},
};

#define TABLE_LEN (sizeof(s_table) / sizeof(s_table[0]))

const char *morse_encode(char c)
{
    c = (char)toupper((unsigned char)c);
    for (unsigned i = 0; i < TABLE_LEN; i++) {
        if (s_table[i].c == c) {
            return s_table[i].p;
        }
    }
    return NULL;
}

char morse_decode(const char *pattern)
{
    if (!pattern || !*pattern) {
        return 0;
    }
    for (unsigned i = 0; i < TABLE_LEN; i++) {
        if (strcmp(s_table[i].p, pattern) == 0) {
            return s_table[i].c;
        }
    }
    return 0;
}

bool morse_is_error_prosign(const char *pattern)
{
    int n = 0;
    for (const char *p = pattern; *p; p++) {
        if (*p != '.') {
            return false;
        }
        n++;
    }
    return n >= 7;
}

bool morse_is_sos_prosign(const char *pattern)
{
    return strcmp(pattern, "...---...") == 0;
}

int morse_sanitize(const char *in, char *out, int out_size)
{
    if (out_size <= 0) {
        return 0;
    }
    int n = 0;
    bool pending_space = false;
    for (const char *p = in; *p && n < out_size - 1; p++) {
        char c = (char)toupper((unsigned char)*p);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (n > 0) {
                pending_space = true;
            }
            continue;
        }
        if (!morse_encode(c)) {
            c = '?';
        }
        if (pending_space) {
            if (n >= out_size - 2) {
                break;
            }
            out[n++] = ' ';
            pending_space = false;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return n;
}

uint32_t morse_text_units(const char *text)
{
    uint32_t u = 0;
    bool first_char = true;
    bool space_before = false;
    for (const char *t = text; *t; t++) {
        if (*t == ' ') {
            space_before = true;
            continue;
        }
        const char *p = morse_encode(*t);
        if (!p) {
            p = morse_encode('?');
        }
        if (!first_char) {
            u += space_before ? 7 : 3;
        }
        space_before = false;
        first_char = false;
        for (int i = 0; p[i]; i++) {
            if (i > 0) {
                u += 1;
            }
            u += (p[i] == '-') ? 3 : 1;
        }
    }
    return u;
}

/* ------------------------------------------------------------------------- */

static int id_index(char c)
{
    c = (char)toupper((unsigned char)c);
    const char *p = strchr(ID_ALPHABET, c);
    return (p && c) ? (int)(p - ID_ALPHABET) : -1;
}

bool id_char_valid(char c)
{
    return id_index(c) >= 0;
}

bool id_valid(const char *id)
{
    if (!id) {
        return false;
    }
    size_t n = strlen(id);
    if (n != 3 && n != 4) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (id_index(id[i]) < 0) {
            return false;
        }
    }
    return true;
}

#define BASE3 (31u * 31u * 31u) /* 29791 */

uint32_t id_to_addr(const char *id)
{
    if (!id_valid(id)) {
        return ADDR_NONE;
    }
    uint32_t v = 0;
    size_t n = strlen(id);
    for (size_t i = 0; i < n; i++) {
        v = v * 31u + (uint32_t)id_index(id[i]);
    }
    return (n == 3) ? v : BASE3 + v;
}

bool addr_to_id(uint32_t addr, char out[ID_MAX_LEN + 1])
{
    int n;
    if (addr < BASE3) {
        n = 3;
    } else if (addr < BASE3 + BASE3 * 31u) {
        n = 4;
        addr -= BASE3;
    } else {
        out[0] = '\0';
        return false;
    }
    out[n] = '\0';
    for (int i = n - 1; i >= 0; i--) {
        out[i] = ID_ALPHABET[addr % 31u];
        addr /= 31u;
    }
    return true;
}
