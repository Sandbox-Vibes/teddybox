#include "freshness.h"
#include "cloud.h"
#include "playback.h"
esp_err_t cloud_post(const char *p, const uint8_t *b, size_t bl, uint8_t *r, size_t rs, size_t *rl) { return ESP_FAIL; }
char *pb_build_filename(uint64_t id) { return NULL; }
esp_err_t pb_check_file(const char *f) { return ESP_FAIL; }
TonieboxAudioFileHeader *pb_toniefile_get_header(FILE *fd) { return NULL; }
bool pb_is_playing(void) { return false; }
uint64_t pb_get_current_uid(void) { return 0; }
void freshness_box_settings_received(const freshness_settings_t *s) {}
bool pb_file_is_own(const char *filename) { return false; }
