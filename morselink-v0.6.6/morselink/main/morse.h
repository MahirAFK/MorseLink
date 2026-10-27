/*
 * MorseLink — Morse code table and device-ID codec.
 *
 * Pure C (no ESP-IDF dependencies) so it can be unit-tested on the host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MORSE_MAX_ELEMENTS 9 /* longest group we decode (SOS prosign) */

/* Pattern ('.'/'-' string) for character c, case-insensitive. NULL if not encodable. */
const char *morse_encode(char c);

/* Character for a pattern, or 0 if the pattern is not a known character. */
char morse_decode(const char *pattern);

/* Error prosign: 7 or more dots and nothing else (spec says 8; one short is tolerated). */
bool morse_is_error_prosign(const char *pattern);

/* SOS prosign keyed as one character: ...---... */
bool morse_is_sos_prosign(const char *pattern);

/* Copy text into out, upper-cased, with unsupported characters replaced by '?',
 * runs of spaces collapsed and leading/trailing spaces removed.
 * Returns the resulting length (always NUL-terminated if out_size > 0). */
int morse_sanitize(const char *in, char *out, int out_size);

/* Number of Morse units a text occupies at standard timing (dot=1, dash=3,
 * intra=1, letter=3, word=7), not counting a trailing gap. Used by tests. */
uint32_t morse_text_units(const char *text);

/* ------------------------------------------------------------------------- */
/* Device IDs                                                                 */
/* ------------------------------------------------------------------------- */

/* 31 symbols: digits 2-9 and A-Z without I, L, O (and no 0/1). */
#define ID_ALPHABET "23456789ABCDEFGHJKMNPQRSTUVWXYZ"
#define ID_ALPHABET_LEN 31
#define ID_MAX_LEN 4

/* Wire addresses are 24-bit. 3-char IDs occupy [0, 31^3), 4-char IDs
 * [31^3, 31^3 + 31^4), so the two lengths can never collide. */
#define ADDR_BROADCAST 0xFFFFFFu
#define ADDR_NONE 0xFFFFFEu

bool id_char_valid(char c);
/* Valid 3- or 4-character ID (upper case expected; lower case accepted). */
bool id_valid(const char *id);
/* ADDR_NONE if the ID is invalid. */
uint32_t id_to_addr(const char *id);
/* Writes a NUL-terminated ID (3 or 4 chars) into out. False for invalid/broadcast. */
bool addr_to_id(uint32_t addr, char out[ID_MAX_LEN + 1]);
