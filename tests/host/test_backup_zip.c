/* backup_zip(): the archive must open with a normal unzip and hold the files unchanged */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "backup_zip.h"

static int run(const char *path)
{
    static uint8_t a[1419], b[1030], c[1192];
    for (size_t i = 0; i < sizeof a; i++) a[i] = (uint8_t)(i * 7 + 1);
    for (size_t i = 0; i < sizeof b; i++) b[i] = (uint8_t)(i * 13 + 5);
    for (size_t i = 0; i < sizeof c; i++) c[i] = (uint8_t)(i * 31 + 9);
    const char *names[] = {"ca.der", "client.der", "private.der"};
    const uint8_t *data[] = {a, b, c};
    const size_t sizes[] = {sizeof a, sizeof b, sizeof c};
    static uint8_t out[8192];
    int n = backup_zip(names, data, sizes, 3, out, sizeof out);
    int small = backup_zip(names, data, sizes, 3, out, 100);
    FILE *f = fopen(path, "wb"); fwrite(out, 1, n, f); fclose(f);
    for (int i = 0; i < 3; i++) { char p[64]; snprintf(p, sizeof p, "%s.%s", path, names[i]); f = fopen(p, "wb"); fwrite(data[i], 1, sizes[i], f); fclose(f); }
    printf("%s zip %d bytes, too small buffer -> %d\n", n > 0 && small == -1 ? "ok  " : "FAIL", n, small);
    return !(n > 0 && small == -1);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    return run(argv[1]);
}
