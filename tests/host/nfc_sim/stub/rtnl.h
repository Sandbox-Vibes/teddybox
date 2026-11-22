#pragma once
#include <stdbool.h>
#include <stdint.h>
#define RTNL3_CHARGER_ON 7
#define RTNL3_CHARGER_OFF 8
static inline void rtnl_tag(uint64_t uid, bool valid) {}
static inline void rtnl_tag_removed(uint64_t uid) {}
static inline void rtnl_event(uint32_t type) {}
