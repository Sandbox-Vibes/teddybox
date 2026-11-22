#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tonie_names.h"

static volatile uint32_t version;

uint32_t tonie_names_version(void) { return version; }

int tonie_names_load(const char *path, tonie_name_t *a, int max)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        return 0;
    }
    int n = 0;
    char line[TONIE_NAME_LEN + 24];
    while (n < max && fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || eq - line != 16)
        {
            continue;
        }
        *eq = 0;
        char *end;
        uint64_t uid = strtoull(line, &end, 16);
        if (*end || !uid || !eq[1])
        {
            continue;
        }
        a[n].uid = uid;
        strncpy(a[n].name, eq + 1, sizeof(a[n].name) - 1);
        a[n].name[sizeof(a[n].name) - 1] = 0;
        tonie_names_clean(a[n].name, sizeof(a[n].name));
        n++;
    }
    fclose(f);
    return n;
}

const char *tonie_names_find(const tonie_name_t *a, int n, uint64_t uid)
{
    for (int i = 0; a && i < n; i++)
    {
        if (a[i].uid == uid)
        {
            return a[i].name;
        }
    }
    return NULL;
}

void tonie_names_clean(char *name, size_t max)
{
    size_t got = strlen(name);
    if (got > max - 1)
    {
        /* do not end inside a UTF-8 sequence */
        got = max - 1;
        size_t lead = got;
        while (lead > 0 && ((unsigned char)name[lead] & 0xC0) == 0x80)
        {
            lead--;
        }
        got = lead;
    }
    else if (got > 0)
    {
        size_t lead = got - 1;
        while (lead > 0 && ((unsigned char)name[lead] & 0xC0) == 0x80)
        {
            lead--;
        }
        unsigned char c = (unsigned char)name[lead];
        size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (lead + need > got)
        {
            got = lead;
        }
    }
    name[got] = 0;
    for (char *p = name; *p; p++)
    {
        if ((unsigned char)*p < 0x20)
        {
            *p = ' ';
        }
    }
}

bool tonie_names_set(const char *path, uint64_t uid, const char *name)
{
    tonie_name_t *a = calloc(TONIE_NAMES_MAX + 1, sizeof(tonie_name_t));
    if (!a)
    {
        return false;
    }
    int n = tonie_names_load(path, a, TONIE_NAMES_MAX);
    int i = 0;
    while (i < n && a[i].uid != uid)
    {
        i++;
    }
    if (name && name[0])
    {
        a[i].uid = uid;
        strncpy(a[i].name, name, sizeof(a[i].name) - 1);
        a[i].name[sizeof(a[i].name) - 1] = 0;
        tonie_names_clean(a[i].name, sizeof(a[i].name));
        if (i == n)
        {
            n++;
        }
    }
    else if (i < n)
    {
        a[i] = a[--n];
    }
    else
    {
        free(a);
        return true; /* nothing to remove */
    }
    /* write a new file next to it, then replace (8.3-safe name) */
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *dot = strrchr(tmp, '.');
    if (dot)
    {
        strcpy(dot, ".new");
    }
    FILE *f = fopen(tmp, "w");
    bool ok = f != NULL;
    for (int k = 0; f && k < n; k++)
    {
        ok = fprintf(f, "%016llX=%s\n", (unsigned long long)a[k].uid, a[k].name) > 0 && ok;
    }
    if (f)
    {
        ok = fclose(f) == 0 && ok;
    }
    free(a);
    if (ok)
    {
        unlink(path);
        ok = rename(tmp, path) == 0;
    }
    if (ok)
    {
        version++;
    }
    return ok;
}

bool tonie_name_lookup(uint64_t uid, char *out, size_t len)
{
    static const char *files[] = {TONIE_ALIAS_FILE, TONIE_TCNAME_FILE};
    out[0] = 0;
    for (int i = 0; i < 2 && !out[0]; i++)
    {
        FILE *f = fopen(files[i], "r");
        if (!f)
        {
            continue;
        }
        char key[17];
        snprintf(key, sizeof(key), "%016llX", (unsigned long long)uid);
        char line[TONIE_NAME_LEN + 24];
        while (fgets(line, sizeof(line), f))
        {
            if (!strncmp(line, key, 16) && line[16] == '=' && line[17] && line[17] != '\n' && line[17] != '\r')
            {
                line[strcspn(line, "\r\n")] = 0;
                strncpy(out, line + 17, len - 1);
                out[len - 1] = 0;
                tonie_names_clean(out, len);
                break;
            }
        }
        fclose(f);
    }
    return out[0] != 0;
}
