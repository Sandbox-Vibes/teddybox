#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* teddyCloud patch of the original firmware on the box, see ofw_patch.c */

#define OFW_HOST_MAX 16      /* "prod.de.tbs.toys" */
#define OFW_HOST_RTNL_MAX 12 /* "rtnl.bxcl.de": longer hosts leave the RTNL name as it is */
#define OFW_MAX_HITS 16
#define OFW_NAME_KINDS 2 /* prod.de.tbs.toys, rtnl.bxcl.de */

typedef struct
{
    bool ofw_found;           /* original app in a slot */
    bool ofw_valid;           /* its image verifies */
    int names_found[OFW_NAME_KINDS]; /* original server names still in it */
    bool patched;             /* patched by TeddyBox (record in NVS) */
    char host[OFW_HOST_MAX + 1];
    bool flash_ca_teddycloud; /* the CA in flash is teddyCloud's */
    bool boxine_ca_saved;     /* original CA kept on the SD card */
} ofw_patch_status_t;

bool ofw_patch_host_valid(const char *host);
esp_err_t ofw_patch_status(ofw_patch_status_t *st);
/* settings may change (boxine_ca, ca_path): the caller saves them */
esp_err_t ofw_patch_apply(const char *host, char *err, size_t errlen);
esp_err_t ofw_patch_undo(char *err, size_t errlen);
