/* harness for check_mdns.py: reads "<legacy 0|1> <hex query>" lines, prints the answer as hex ("" = none) */
#include <stdio.h>
#include <string.h>
#include "mdns_min.h"

int main(void)
{
    char line[2100];
    while (fgets(line, sizeof line, stdin)) {
        int legacy = line[0] == '1';
        uint8_t in[1024], out[128];
        int n = 0;
        for (char *p = line + 2; p[0] && p[1] && p[0] != '\n'; p += 2) {
            unsigned v; sscanf(p, "%2x", &v); in[n++] = (uint8_t)v;
        }
        uint32_t ip; uint8_t b[4] = {192, 168, 10, 42}; memcpy(&ip, b, 4);
        int r = mdns_min_answer(in, n, "teddybox-3a7f", ip, legacy, out, sizeof out);
        for (int i = 0; i < r; i++) printf("%02x", out[i]);
        printf("\n");
    }
    uint32_t ip; uint8_t b[4] = {192, 168, 10, 42}; memcpy(&ip, b, 4);
    uint8_t out[128];
    int r = mdns_min_announce("teddybox-3a7f", ip, out, sizeof out);
    for (int i = 0; i < r; i++) printf("%02x", out[i]);
    printf("\n");
    return 0;
}
