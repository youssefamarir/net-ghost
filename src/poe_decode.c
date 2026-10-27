#include "poe_decode.h"
#include <string.h>
#include <limits.h>
static unsigned u16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t u32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void poe_reset(poe_info_t *p)
{
    memset(p, 0, sizeof(*p));
    p->pairs = p->power_class = p->type = p->source = p->priority = -1;
    p->requested_mw = p->allocated_mw = p->med_mw = p->cdp_available_mw = p->cdp_used_mw = -1;
}
bool poe_lldp(poe_info_t *p, const uint8_t *v, size_t n)
{
    if (n < 7) return false;
    if (v[0] == 0 && v[1] == 0x12 && v[2] == 0x0f && v[3] == 2) {
        p->valid = p->mdi = true;
        strcpy(p->protocol, "LLDP 802.3");
        p->pse = (v[4] & 1) != 0;
        p->supported = (v[4] & 2) != 0;
        p->enabled = (v[4] & 4) != 0;
        p->pair_control = (v[4] & 8) != 0;
        p->pairs = v[5] == 1 || v[5] == 2 ? v[5] : -1;
        p->power_class = v[6] >= 1 && v[6] <= 5 ? v[6] - 1 : -1;
        p->extended = n >= 12;
        p->requested_mw = p->allocated_mw = -1;
        if (p->extended) {
            p->type = (v[7] & 0x80) ? 1 : 2;
            p->source = (v[7] >> 4) & 3;
            p->priority = v[7] & 3;
            p->requested_mw = (int)u16(v + 8) * 100;
            p->allocated_mw = (int)u16(v + 10) * 100;
        }
        return true;
    }
    if (v[0] == 0 && v[1] == 0x12 && v[2] == 0xbb && v[3] == 4) {
        p->valid = true;
        if (!p->mdi) {
            strcpy(p->protocol, "LLDP-MED");
            p->priority = v[4] & 0xf;
            p->source = (v[4] >> 4) & 3;
        }
        p->med_mw = (int)u16(v + 5) * 100;
        return true;
    }
    return false;
}
bool poe_cdp(poe_info_t *p, uint16_t type, const uint8_t *v, size_t n)
{
    if (type == 0x0010 && n >= 2) p->cdp_used_mw = (int)u16(v);
    else if (type == 0x001a && n >= 8) {
        uint32_t value = u32(v + 4); // request ID (2), management ID (2), available (4)
        if (value > INT_MAX) return false;
        p->cdp_available_mw = (int)value;
    } else return false;
    p->valid = true;
    if (!p->mdi) strcpy(p->protocol, "CDP/FDP");
    return true;
}
