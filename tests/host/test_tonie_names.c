#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tonie_names.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    mkdir(TONIE_NAMES_DIR, 0777);
    unlink(TONIE_ALIAS_FILE);
    unlink(TONIE_TCNAME_FILE);
    char n[TONIE_NAME_LEN];
    const uint64_t A = 0xE004035011223344ULL, B = 0xE00403501234ABCDULL;

    CHECK(!tonie_name_lookup(A, n, sizeof(n)) && !n[0]);
    uint32_t v = tonie_names_version();
    CHECK(tonie_names_set(TONIE_TCNAME_FILE, A, "Bibi Blocksberg – Hexen") && tonie_names_version() != v);
    CHECK(tonie_name_lookup(A, n, sizeof(n)) && !strcmp(n, "Bibi Blocksberg – Hexen"));
    /* own name wins */
    CHECK(tonie_names_set(TONIE_ALIAS_FILE, A, "Lieblings-Tonie"));
    CHECK(tonie_name_lookup(A, n, sizeof(n)) && !strcmp(n, "Lieblings-Tonie"));
    /* removing the own name shows teddyCloud's again */
    CHECK(tonie_names_set(TONIE_ALIAS_FILE, A, ""));
    CHECK(tonie_name_lookup(A, n, sizeof(n)) && !strcmp(n, "Bibi Blocksberg – Hexen"));
    /* several entries, replace, load */
    CHECK(tonie_names_set(TONIE_TCNAME_FILE, B, "Zwei"));
    CHECK(tonie_names_set(TONIE_TCNAME_FILE, B, "Zwei neu"));
    tonie_name_t a[4];
    int k = tonie_names_load(TONIE_TCNAME_FILE, a, 4);
    CHECK(k == 2 && !strcmp(tonie_names_find(a, k, B), "Zwei neu") && !tonie_names_find(a, k, 0x1234));
    /* control characters and line breaks never end up in the file */
    CHECK(tonie_names_set(TONIE_ALIAS_FILE, B, "a\nb\tc"));
    CHECK(tonie_name_lookup(B, n, sizeof(n)) && !strcmp(n, "a b c"));
    /* cut at a UTF-8 boundary */
    char s[8] = "abcdeä";      /* 'ä' = 2 bytes at 5..6 */
    tonie_names_clean(s, 7);  /* room for 6 bytes: the 'ä' does not fit */
    CHECK(!strcmp(s, "abcde"));
    char t[16] = "x\xc3";      /* an incomplete sequence at the end */
    tonie_names_clean(t, sizeof(t));
    CHECK(!strcmp(t, "x"));
    char u[16] = "Bär";
    tonie_names_clean(u, sizeof(u));
    CHECK(!strcmp(u, "Bär"));

    printf("tonie_names: %s\n", fails ? "FAILED" : "all tests ok");
    return fails != 0;
}
