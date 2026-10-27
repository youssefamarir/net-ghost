#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "poe_decode.h"
int main(void) {
    poe_info_t p; poe_reset(&p);
    assert(!p.valid && p.allocated_mw == -1 && p.power_class == -1);
    uint8_t dot3[] = {0,0x12,0x0f,2,0x0f,1,5,0x12,0,130,1,44};
    for (size_t n=0;n<7;n++) assert(!poe_lldp(&p,dot3,n));
    assert(poe_lldp(&p,dot3,sizeof dot3));
    assert(p.mdi && p.pse && p.supported && p.enabled && p.pair_control);
    assert(p.pairs==1 && p.power_class==4 && p.type==2 && p.source==1 && p.priority==2);
    assert(p.requested_mw==13000 && p.allocated_mw==30000);
    poe_reset(&p);dot3[4]=0;dot3[5]=0;dot3[6]=0;
    assert(poe_lldp(&p,dot3,7));
    assert(!p.pse && !p.supported && !p.extended && p.pairs==-1 && p.power_class==-1 && p.allocated_mw==-1);
    uint8_t med[]={0,0x12,0xbb,4,0x11,0,154};
    assert(poe_lldp(&p,med,7) && p.med_mw==15400);
    uint8_t avail[]={0,1,0,2,0,0,0x75,0x30,0xde,0xad,0xbe,0xef};
    poe_reset(&p);
    assert(!poe_cdp(&p,0x001a,avail,7));
    assert(poe_cdp(&p,0x001a,avail,8) && p.cdp_available_mw==30000);
    assert(poe_cdp(&p,0x001a,avail,12) && p.cdp_available_mw==30000);
    uint8_t used[]={0x13,0x88};
    assert(poe_cdp(&p,0x0010,used,2) && p.cdp_used_mw==5000);
    assert(!poe_cdp(&p,0x0001,avail,8));
    uint8_t huge[]={0,0,0,0,0xff,0xff,0xff,0xff};
    assert(!poe_cdp(&p,0x001a,huge,8));
    puts("PoE decoder: malformed/short TLVs, role/class/pairs, 802.3 power scaling, MED, CDP offset and range checks PASS");
}
