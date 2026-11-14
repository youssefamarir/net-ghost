
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    bool valid, mdi, pse, supported, enabled, pair_control, extended;
    int pairs, power_class, type, source, priority;
    int requested_mw, allocated_mw, med_mw, cdp_available_mw, cdp_used_mw;
    char protocol[16];
    int64_t seen_ms;
} poe_info_t;
void poe_reset(poe_info_t *p);
bool poe_lldp(poe_info_t *p, const uint8_t *v, size_t n);
bool poe_cdp(poe_info_t *p, uint16_t type, const uint8_t *v, size_t n);
