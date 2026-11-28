#pragma once
/*
 * Box events to teddyCloud over RTNL, like the original firmware sends them
 * to rtnl.bxcl.de: teddyCloud shows them (web interface, its own MQTT/Home
 * Assistant) and knows which tonie is on the box. Only to teddyCloud, never
 * to Boxine; off with "rtnl = 0" in teddybox.ini. Events while teddyCloud is
 * not reachable are dropped.
 */
#include <stdbool.h>
#include <stdint.h>
#include "rtnl_encode.h"

void rtnl_init(void);
/* the connection to teddyCloud is open */
bool rtnl_is_connected(void);
void rtnl_tag(uint64_t uid, bool valid);
void rtnl_tag_removed(uint64_t uid);
void rtnl_audio_id(uint32_t audio_id);
void rtnl_volume(int percent);
/* log3 event, RTNL3_* */
void rtnl_event(uint32_t type);
