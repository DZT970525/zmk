#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <zmk/event_manager.h>
#include <zmk/split/central.h>
#include <zmk/display.h>
#include "screen_config.h"
#define PREFIX "zitaotech_screen"
struct playback_config { uint32_t ms; uint16_t mask; uint8_t sequential; uint8_t order[5]; };
static const struct playback_config defaults={60000,0x3ff,0,{0x01,0x23,0x45,0x67,0x89}};
static struct playback_config config={60000,0x3ff,0,{0x01,0x23,0x45,0x67,0x89}}, right={0,0,0,{0x01,0x23,0x45,0x67,0x89}};
static struct k_spinlock lock;
static bool v3_loaded,mask_v2_loaded;
static struct playback_config snapshot(bool remote) {k_spinlock_key_t key=k_spin_lock(&lock);struct playback_config result=remote?right:config;k_spin_unlock(&lock,key);return result;}
static void assign(struct playback_config value,bool remote){k_spinlock_key_t key=k_spin_lock(&lock);if(remote)right=value;else config=value;k_spin_unlock(&lock,key);}
static bool valid(struct playback_config value){unsigned seen=0;for(unsigned i=0;i<10;i++){unsigned id=(value.order[i/2]>>(i%2?0:4))&15;if(id>=10||(seen&BIT(id)))return false;seen|=BIT(id);}return value.ms>=100&&value.ms<=600000&&value.ms%100==0&&value.sequential<=1&&value.mask<=0x3ff;}
uint32_t zitaotech_screen_interval_ms(void){return snapshot(false).ms;}
uint32_t zitaotech_screen_interval_s(void){return (zitaotech_screen_interval_ms()+999)/1000;}
uint16_t zitaotech_screen_enabled_mask(void){return snapshot(false).mask;}
bool zitaotech_screen_sequential(void){return snapshot(false).sequential;}
void zitaotech_screen_playback_order(uint8_t out[5]){struct playback_config c=snapshot(false);memcpy(out,c.order,5);}
uint32_t zitaotech_screen_right_interval_ms(void){return snapshot(true).ms;}
uint32_t zitaotech_screen_right_interval_s(void){return (zitaotech_screen_right_interval_ms()+999)/1000;}
uint16_t zitaotech_screen_right_applied_mask(void){return snapshot(true).mask;}
bool zitaotech_screen_right_sequential(void){return snapshot(true).sequential;}
void zitaotech_screen_right_playback_order(uint8_t out[5]){struct playback_config c=snapshot(true);memcpy(out,c.order,5);}
__weak void zitaotech_screen_state_changed(void){}
ZMK_EVENT_IMPL(zitaotech_screen_config_mirror_v3);
ZMK_EVENT_IMPL(zitaotech_screen_config_request_v3);
ZMK_EVENT_IMPL(zitaotech_screen_config_applied_v3);
ZMK_EVENT_IMPL(zitaotech_screen_config_query_v3);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zitaotech_screen_config_mirror_v3,sf3,source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_screen_config_mirror_v3,sf3,source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zitaotech_screen_config_request_v3,sr3,source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_screen_config_request_v3,sr3,source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zitaotech_screen_config_applied_v3,sa3,source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_screen_config_applied_v3,sa3,source);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zitaotech_screen_config_query_v3,sq3,source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_screen_config_query_v3,sq3,source);
static int save(struct playback_config value){return settings_save_one(PREFIX "/playback_v3",&value,sizeof(value));}
static int load_setting(const char *name,size_t len,settings_read_cb read_cb,void *arg){
 struct playback_config value=snapshot(false);int32_t old;const char *next;
 if(settings_name_steq(name,"playback_v3",&next)&&!next){if(len!=sizeof(value)||read_cb(arg,&value,sizeof(value))!=sizeof(value)||!valid(value))return -EINVAL;v3_loaded=true;assign(value,false);return 0;}
 if(v3_loaded)return 0;
 if(strcmp(name,"interval")&&strcmp(name,"mask")&&strcmp(name,"mask_v2"))return -ENOENT;
 if(len!=sizeof(old)||read_cb(arg,&old,sizeof(old))!=sizeof(old))return -EINVAL;
 if(!strcmp(name,"interval"))value.ms=CLAMP(old,5,600)*1000;
 else if(!strcmp(name,"mask_v2")){value.mask=(uint16_t)old&0x3ff;mask_v2_loaded=true;}
 else if(!mask_v2_loaded)value.mask=(old&0xff)|0x300;
 assign(value,false);return 0;
}
#if !IS_ENABLED(CONFIG_ZMK_SPLIT)||IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
bool zitaotech_screen_right_connected(void){return zmk_split_central_peripherals_connected();}
static void push(void){struct playback_config c=snapshot(false);struct zitaotech_screen_config_mirror_v3 event={.interval_ticks=c.ms/100,.enabled_mask=c.mask|(c.sequential?0x8000:0),.source=ZMK_RELAY_EVENT_SOURCE_SELF};memcpy(event.order,c.order,5);raise_zitaotech_screen_config_mirror_v3(event);}
void zitaotech_screen_refresh_right(void){raise_zitaotech_screen_config_query_v3((struct zitaotech_screen_config_query_v3){.source=ZMK_RELAY_EVENT_SOURCE_SELF});}
static int update(struct playback_config value){if(!valid(value))return -EINVAL;struct playback_config old=snapshot(false);if(!memcmp(&old,&value,sizeof(value)))return 0;int err=save(value);if(err)return err;assign(value,false);push();zitaotech_screen_state_changed();return 0;}
int zitaotech_screen_set_playback(uint32_t ms,bool sequential,const uint8_t order[5]){struct playback_config c=snapshot(false);c.ms=ms;c.sequential=sequential;memcpy(c.order,order,5);return update(c);}
int zitaotech_screen_set_interval_s(uint32_t seconds){struct playback_config c=snapshot(false);c.ms=CLAMP(seconds,5,600)*1000;return update(c);}
int zitaotech_screen_set_enabled_mask(uint16_t mask){struct playback_config c=snapshot(false);c.mask=mask&0x3ff;return update(c);}
int zitaotech_screen_reset(void){return update(defaults);}
static int request(const zmk_event_t *eh){push();return ZMK_EV_EVENT_BUBBLE;}
ZMK_LISTENER(screen_v3_request,request);ZMK_SUBSCRIPTION(screen_v3_request,zitaotech_screen_config_request_v3);
static int applied(const zmk_event_t *eh){struct zitaotech_screen_config_applied_v3 *e=as_zitaotech_screen_config_applied_v3(eh);if(!e||e->source==ZMK_RELAY_EVENT_SOURCE_SELF)return ZMK_EV_EVENT_BUBBLE;struct playback_config c={.ms=e->interval_ticks*100,.mask=e->enabled_mask&0x3ff,.sequential=!!(e->enabled_mask&0x8000)};memcpy(c.order,e->order,5);if(valid(c)){assign(c,true);zitaotech_screen_state_changed();}return ZMK_EV_EVENT_BUBBLE;}
ZMK_LISTENER(screen_v3_applied,applied);ZMK_SUBSCRIPTION(screen_v3_applied,zitaotech_screen_config_applied_v3);
static int commit(void){push();zitaotech_screen_state_changed();return 0;}
static struct settings_handler handler={.name=PREFIX,.h_set=load_setting,.h_commit=commit};
#else
bool zitaotech_screen_right_connected(void){return false;}
void zitaotech_screen_refresh_right(void){}
int zitaotech_screen_set_playback(uint32_t ms,bool sequential,const uint8_t order[5]){return -ENOTSUP;}
int zitaotech_screen_set_interval_s(uint32_t seconds){return -ENOTSUP;}
int zitaotech_screen_set_enabled_mask(uint16_t mask){return -ENOTSUP;}
int zitaotech_screen_reset(void){return -ENOTSUP;}
static struct playback_config pending;
static void report(void){struct playback_config c=snapshot(false);struct zitaotech_screen_config_applied_v3 e={.interval_ticks=c.ms/100,.enabled_mask=c.mask|(c.sequential?0x8000:0),.source=ZMK_RELAY_EVENT_SOURCE_SELF};memcpy(e.order,c.order,5);raise_zitaotech_screen_config_applied_v3(e);}
static void apply_work(struct k_work *work){k_spinlock_key_t key=k_spin_lock(&lock);struct playback_config c=pending;k_spin_unlock(&lock,key);if(save(c))return;assign(c,false);zmk_widget_status_carousel_config_changed();report();}
static K_WORK_DEFINE(apply,apply_work);
static int mirror(const zmk_event_t *eh){struct zitaotech_screen_config_mirror_v3 *e=as_zitaotech_screen_config_mirror_v3(eh);if(!e||e->source==ZMK_RELAY_EVENT_SOURCE_SELF)return ZMK_EV_EVENT_BUBBLE;struct playback_config c={.ms=e->interval_ticks*100,.mask=e->enabled_mask&0x3ff,.sequential=!!(e->enabled_mask&0x8000)};memcpy(c.order,e->order,5);struct playback_config old=snapshot(false);if(valid(c)&&memcmp(&old,&c,sizeof(c))){k_spinlock_key_t key=k_spin_lock(&lock);pending=c;k_spin_unlock(&lock,key);k_work_submit_to_queue(zmk_display_work_q(),&apply);}return ZMK_EV_EVENT_BUBBLE;}
ZMK_LISTENER(screen_v3_mirror,mirror);ZMK_SUBSCRIPTION(screen_v3_mirror,zitaotech_screen_config_mirror_v3);
static int query(const zmk_event_t *eh){report();return ZMK_EV_EVENT_BUBBLE;}
ZMK_LISTENER(screen_v3_query,query);ZMK_SUBSCRIPTION(screen_v3_query,zitaotech_screen_config_query_v3);
static struct k_work_delayable poll;
static void poll_work(struct k_work *work){raise_zitaotech_screen_config_request_v3((struct zitaotech_screen_config_request_v3){.source=ZMK_RELAY_EVENT_SOURCE_SELF});k_work_schedule(&poll,K_SECONDS(2));}
static struct settings_handler handler={.name=PREFIX,.h_set=load_setting};
#endif
static int init(void){int err=settings_register(&handler);
#if IS_ENABLED(CONFIG_ZMK_SPLIT)&&!IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
 k_work_init_delayable(&poll,poll_work);k_work_schedule(&poll,K_SECONDS(2));
#endif
 return err;}
SYS_INIT(init,APPLICATION,95);



