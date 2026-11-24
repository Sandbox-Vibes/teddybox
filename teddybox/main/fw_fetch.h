#pragma once
/*
 * Online update from the setup page (setup mode in the home network):
 * the page asks GitHub for the latest release (api.github.com allows that
 * from a browser), the box downloads the image of that release itself over
 * HTTPS (GitHub's release downloads cannot be fetched by the page), checks
 * it against SHA256SUMS of the same release and installs it like an upload.
 *
 * Only releases of TEDDYBOX_REPO and tags like v1.2.3 are accepted: the
 * page cannot point the box to any other file.
 */
#include <stdbool.h>
#include "esp_http_server.h"

#define TEDDYBOX_REPO "Sandbox-Vibes/teddybox"

/* POST /api/firmware/fetch?tag=v1.2.3[&pd=1], GET /api/firmware/fetch/status;
   finished(): called after a successful install (the portal restarts) */
esp_err_t fw_fetch_register(httpd_handle_t server, void (*touch)(void), void (*finished)(void));
