#include <string.h>
#include <time.h>
#include "nvs.h"
#include "tonie_seen.h"

#define SEEN_NVS "TB_SEEN"

int tonie_seen_get(tonie_seen_t out[TONIE_SEEN_MAX])
{
    memset(out, 0, sizeof(tonie_seen_t) * TONIE_SEEN_MAX);
    nvs_handle_t h;
    if (nvs_open(SEEN_NVS, NVS_READONLY, &h) != ESP_OK)
    {
        return 0;
    }
    size_t len = sizeof(tonie_seen_t) * TONIE_SEEN_MAX;
    nvs_get_blob(h, "list", out, &len);
    nvs_close(h);
    int n = 0;
    while (n < TONIE_SEEN_MAX && out[n].uid)
    {
        n++;
    }
    return n;
}

void tonie_seen_add(uint64_t uid)
{
    tonie_seen_t list[TONIE_SEEN_MAX];
    int n = tonie_seen_get(list);
    if (n && list[0].uid == uid)
    {
        return; /* same tonie again: no flash write */
    }
    /* move it to the front */
    int pos = n < TONIE_SEEN_MAX ? n : TONIE_SEEN_MAX - 1;
    for (int i = 0; i < n; i++)
    {
        if (list[i].uid == uid)
        {
            pos = i;
            break;
        }
    }
    memmove(&list[1], &list[0], sizeof(tonie_seen_t) * pos);
    time_t now = time(NULL);
    list[0].uid = uid;
    list[0].time = now > 1700000000 ? (uint32_t)now : 0;

    nvs_handle_t h;
    if (nvs_open(SEEN_NVS, NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_blob(h, "list", list, sizeof(list));
        nvs_commit(h);
        nvs_close(h);
    }
}
