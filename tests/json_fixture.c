#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "buster.h"
#include "json.h"
bst_t g; cfg_t cfg;
void bst_lock(void) {} void bst_unlock(void) {}
int64_t now_ms(void) { return 600000; }
float c2f(float c) { return c*1.8f+32; }
bool poe_is_fresh(void) { return g.poe.valid && g.eth_link && g.sw_found; }
int main(int argc, char **argv) {
    (void)argv; memset(&g,0,sizeof g); memset(&cfg,0,sizeof cfg);
    cfg.contrast=true; cfg.animations=true; cfg.text_size=argc>1;
    g.eth_link=g.eth_ip=g.eth_hw=g.eth_full=true;g.eth_speed=100;
    strcpy(g.ip,"192.0.2.10");strcpy(g.mask,"255.255.255.0");strcpy(g.gw,"192.0.2.1");strcpy(g.dns,"192.0.2.1");strcpy(g.mac,"02:00:00:00:00:01");
    g.sw_found=true;strcpy(g.sw_name,"QA fixture switch");strcpy(g.sw_port,"GigabitEthernet1/0/24");strcpy(g.sw_vlan,"30");strcpy(g.sw_voice,"40");strcpy(g.sw_proto,"LLDP");strcpy(g.sw_adv,"10/100/1000");strcpy(g.sw_duplex,"100M Full");strcpy(g.sw_model,"QA fixture");
    g.wifi_conn=true;strcpy(g.w_cfg_ssid,"QA fixture Wi-Fi");strcpy(g.w_ssid,"QA fixture Wi-Fi");strcpy(g.w_bssid,"02:00:00:00:00:02");strcpy(g.w_ip,"198.51.100.10");strcpy(g.w_gw,"198.51.100.1");strcpy(g.w_mask,"255.255.255.0");strcpy(g.w_dns,"198.51.100.1");strcpy(g.w_auth,"WPA2");strcpy(g.w_phy,"11n");strcpy(g.w_mac,"02:00:00:00:00:03");strcpy(g.w_pairwise,"CCMP");strcpy(g.w_group,"CCMP");strcpy(g.w_country,"US");g.w_chan=6;g.w_freq=2437;g.w_width=20;g.w_rssi=-57;
    g.ap_count=MAX_APS;
    for(int i=0;i<MAX_APS;i++) {memset(g.aps[i].ssid,'"',32);strcpy(g.aps[i].bssid,"02:00:00:00:00:04");strcpy(g.aps[i].auth,"WPA2/3");g.aps[i].chan=i%11+1;g.aps[i].rssi=-40-i;g.chan_cnt[g.aps[i].chan]++;}
    g.alert_n=10;for(int i=0;i<10;i++){g.alerts[i].lvl=i%3;memset(g.alerts[i].txt,'"',55);}
    for(int i=0;i<TS_COUNT;i++){g.ts[i].st=T_PASS;strcpy(g.ts[i].val,"OK");}
    g.wifi_speed.down_ok=g.wifi_speed.up_ok=true;g.wifi_speed.down_mbps=12.34;g.wifi_speed.up_mbps=7.89;strcpy(g.wifi_speed.stage,"Complete");strcpy(g.wifi_speed.network,g.w_ssid);g.wifi_speed.first_byte_ms=200;g.wifi_speed.channel=6;g.wifi_speed.down_bytes=5000000;g.wifi_speed.up_bytes=1000000;g.wifi_speed.progress=100;g.wifi_speed.finished_ms=590000;
    poe_reset(&g.poe);uint8_t v[]={0,0x12,0x0f,2,0x0f,1,5,0x12,0,130,1,44};poe_lldp(&g.poe,v,sizeof v);g.poe.seen_ms=590000;
    g.chip_temp=38;g.heap_free=120000;g.heap_min=90000;strcpy(g.node_id,"qa-fixture");strcpy(g.ap_ssid,"NetAlly-QA");strcpy(g.ap_pass,"qa-only-123");strcpy(g.ap_ip,"192.168.4.1");strcpy(g.reset_reason,"power-on");
    for(int i=0;i<48;i++){g.rssi_hist[i]=-50-i%12;g.fps_hist[i]=i*3;}
    char b[16384];int n=build_status_json(b,sizeof b);assert(n>0 && n<(int)sizeof(b)-1);fwrite(b,1,n,stdout);fputc('\n',stdout);
    return 0;
}
