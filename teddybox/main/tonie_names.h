#pragma once
/*
 * Names for tonies: own names from the setup page (tb_alias.txt) win over
 * names from teddyCloud (tb_names.txt, "series – episode" from its
 * tonies.json, fetched by tonie_names_tc.c). Both files on the SD card,
 * one "UID=name" per line. Plain C (stdio), unit tested on a PC.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TONIE_NAME_LEN 96
#define TONIE_NAMES_MAX 300

#ifndef TONIE_NAMES_DIR
#define TONIE_NAMES_DIR "/sdcard"
#endif
#define TONIE_ALIAS_FILE TONIE_NAMES_DIR "/tb_alias.txt"
#define TONIE_TCNAME_FILE TONIE_NAMES_DIR "/tb_names.txt"

typedef struct
{
    uint64_t uid;
    char name[TONIE_NAME_LEN];
} tonie_name_t;

/* reads a name file; returns the number of entries */
int tonie_names_load(const char *path, tonie_name_t *a, int max);
const char *tonie_names_find(const tonie_name_t *a, int n, uint64_t uid);
/* sets (name not empty) or removes the name of uid in the file */
bool tonie_names_set(const char *path, uint64_t uid, const char *name);
/* cuts name to at most max-1 bytes at a UTF-8 boundary, control characters to spaces */
void tonie_names_clean(char *name, size_t max);
/* own name, else teddyCloud's; false if neither */
bool tonie_name_lookup(uint64_t uid, char *out, size_t len);
/* bumped on every change (to refresh what shows the current name) */
uint32_t tonie_names_version(void);
