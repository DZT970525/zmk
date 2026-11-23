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
#define INITIAL_ORDER {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27}
struct playback_config { uint32_t ms; uint32_t mask; uint8_t sequential; uint8_t order[28]; };
static const struct playback_config defaults={60000,ZITAOTECH_SCREEN_PICTURES_DEFAULT,0,INITIAL_ORDER};
static struct playback_config config={60000,ZITAOTECH_SCREEN_PICTURES_DEFAULT,0,INITIAL_ORDER}, right;
static struct k_spinlock lock;
static bool loaded, v3_loaded, mask_v2_loaded;
static struct playback_config snapshot(bool remote){k_spinlock_key_t key=k_spin_lock(&lock);struct playback_config c=remote?right:config;k_spin_unlock(&lock,key);return c;}
static void assign(struct playback_config c,bool remote){k_spinlock_key_t key=k_spin_lock(&lock);if(remote)right=c;else config=c;k_spin_unlock(&lock,key);}
static bool valid(struct playback_config c){uint32_t seen=0;for(unsigned i=0;i<28;i++){unsigned id=c.order[i];if(id>=28||(seen&BIT(id)))return false;seen|=BIT(id);}return c.ms>=100&&c.ms<=600000&&c.ms%100==0&&c.sequential<=1&&c.mask<=ZITAOTECH_SCREEN_PICTURES_DEFAULT;}
uint32_t zitaotech_screen_interval_ms(void){return snapshot(false).ms;}
uint32_t zitaotech_screen_interval_s(void){return (zitaotech_screen_interval_ms()+999)/1000;}
uint32_t zitaotech_screen_enabled_mask(void){return snapshot(false).mask;}
bool zitaotech_screen_sequential(void){return snapshot(false).sequential;}
void zitaotech_screen_playback_order(uint8_t out[28]){struct playback_config c=snapshot(false);memcpy(out,c.order,28);}
uint32_t zitaotech_screen_right_interval_ms(void){return snapshot(true).ms;}
uint32_t zitaotech_screen_right_interval_s(void){return (zitaotech_screen_right_interval_ms()+999)/1000;}
uint32_t zitaotech_screen_right_applied_mask(void){return snapshot(true).mask;}
bool zitaotech_screen_right_sequential(void){return snapshot(true).sequential;}
void zitaotech_screen_right_playback_order(uint8_t out[28]){struct playback_config c=snapshot(true);memcpy(out,c.order,28);}
__weak void zitaotech_screen_state_changed(void){}
ZMK_EVENT_IMPL(zitaotech_screen_config_mirror_v4);
ZMK_EVENT_IMPL(zitaotech_screen_config_request_v4);
ZMK_EVENT_IMPL(zitaotech_screen_config_applied_v4);
ZMK_EVENT_IMPL(zitaotech_screen_config_query_v4);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zitaotech_screen_config_mirror_v4,sf4,source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_screen_config_mirror_v4,sf4,source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zitaotech_screen_config_request_v4,sr4,source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_screen_config_request_v4,sr4,source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zitaotech_screen_config_applied_v4,sa4,source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_screen_config_applied_v4,sa4,source);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zitaotech_screen_config_query_v4,sq4,source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_screen_config_query_v4,sq4,source);
BUILD_ASSERT(sizeof(struct zitaotech_screen_config_mirror_v4)<=12,"Carousel chunk must fit default ATT MTU");
static int save(struct playback_config c){return settings_save_one(PREFIX "/playback_v4",&c,sizeof(c));}
static int load_setting(const char *name,size_t len,settings_read_cb cb,void *arg){
 struct playback_config c=snapshot(false);int32_t old;
 if(!strcmp(name,"playback_v4")){if(len!=sizeof(c)||cb(arg,&c,sizeof(c))!=sizeof(c)||!valid(c))return -EINVAL;loaded=true;assign(c,false);return 0;}
 if(loaded)return 0;
 if(!strcmp(name,"playback_v3")){
  struct {uint32_t ms;uint16_t mask;uint8_t sequential;uint8_t order[5];} legacy;
  if(len!=sizeof(legacy)||cb(arg,&legacy,sizeof(legacy))!=sizeof(legacy))return -EINVAL;
  c.ms=legacy.ms;c.mask=legacy.mask|0x0ffffc00;c.sequential=legacy.sequential;
  for(unsigned i=0;i<10;i++)c.order[i]=(legacy.order[i/2]>>(i%2?0:4))&15;
  if(!valid(c))return -EINVAL;v3_loaded=true;assign(c,false);return 0;
 }
 if(v3_loaded)return 0;
 if(strcmp(name,"interval")&&strcmp(name,"mask")&&strcmp(name,"mask_v2"))return -ENOENT;
 if(len!=sizeof(old)||cb(arg,&old,sizeof(old))!=sizeof(old))return -EINVAL;
 if(!strcmp(name,"interval"))c.ms=CLAMP(old,5,600)*1000;
 else if(!strcmp(name,"mask_v2")){c.mask=((uint32_t)old&0x3ff)|0x0ffffc00;mask_v2_loaded=true;}
 else if(!mask_v2_loaded)c.mask=(old&0xff)|0x0fffff00;
 assign(c,false);return 0;
}
/* Seven independently MTU-safe fragments. Incomplete configurations never apply. */
static struct {struct playback_config c;uint8_t generation;uint8_t parts;} assembly;
static bool collect(uint16_t ticks,uint32_t mask,bool sequential,uint8_t generation,uint8_t offset,const uint8_t order[4],struct playback_config *out){
 if(offset>=28||offset%4||mask>ZITAOTECH_SCREEN_PICTURES_DEFAULT)return false;
 if(!assembly.parts||assembly.generation!=generation||assembly.c.ms!=ticks*100u||assembly.c.mask!=mask||assembly.c.sequential!=sequential){memset(&assembly,0,sizeof(assembly));assembly.generation=generation;assembly.c.ms=ticks*100u;assembly.c.mask=mask;assembly.c.sequential=sequential;}
 memcpy(assembly.c.order+offset,order,MIN(4,28-offset));assembly.parts|=BIT(offset/4);
 if(assembly.parts!=0x7f)return false;
 *out=assembly.c;assembly.parts=0;return valid(*out);
}
#if !IS_ENABLED(CONFIG_ZMK_SPLIT)||IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
bool zitaotech_screen_right_connected(void){return zmk_split_central_peripherals_connected();}
static struct playback_config outbound;
static uint8_t send_generation, send_offset;
static void push_part(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(push_work,push_part);
static void push_part(struct k_work *work){
 k_spinlock_key_t key=k_spin_lock(&lock);struct playback_config c=outbound;unsigned offset=send_offset;unsigned generation=send_generation;if(offset<28)send_offset+=4;k_spin_unlock(&lock,key);
 if(offset>=28)return;
 struct zitaotech_screen_config_mirror_v4 e={.interval_ticks=c.ms/100,.enabled_mask=c.mask|(c.sequential?BIT(31):0),.part=(generation<<3)|(offset/4),.source=ZMK_RELAY_EVENT_SOURCE_SELF};memcpy(e.order,c.order+offset,4);raise_zitaotech_screen_config_mirror_v4(e);
 k_work_reschedule_for_queue(zmk_display_work_q(),&push_work,K_MSEC(30));
}
static void push(void){struct playback_config c=snapshot(false);k_spinlock_key_t key=k_spin_lock(&lock);outbound=c;send_generation=(send_generation+1)&31;send_offset=0;k_spin_unlock(&lock,key);k_work_reschedule_for_queue(zmk_display_work_q(),&push_work,K_NO_WAIT);}
void zitaotech_screen_refresh_right(void){raise_zitaotech_screen_config_query_v4((struct zitaotech_screen_config_query_v4){.source=ZMK_RELAY_EVENT_SOURCE_SELF});}
static int update(struct playback_config c){if(!valid(c))return -EINVAL;struct playback_config old=snapshot(false);if(!memcmp(&old,&c,sizeof(c)))return 0;int err=save(c);if(err)return err;assign(c,false);push();zitaotech_screen_state_changed();return 0;}
int zitaotech_screen_set_playback(uint32_t ms,bool sequential,const uint8_t order[28]){struct playback_config c=snapshot(false);c.ms=ms;c.sequential=sequential;memcpy(c.order,order,28);return update(c);}
int zitaotech_screen_set_interval_s(uint32_t seconds){struct playback_config c=snapshot(false);c.ms=CLAMP(seconds,5,600)*1000;return update(c);}
int zitaotech_screen_set_enabled_mask(uint32_t mask){struct playback_config c=snapshot(false);c.mask=mask;return update(c);}
int zitaotech_screen_reset(void){return update(defaults);}
static int request(const zmk_event_t *eh){push();return ZMK_EV_EVENT_BUBBLE;}
ZMK_LISTENER(screen_v4_request,request);ZMK_SUBSCRIPTION(screen_v4_request,zitaotech_screen_config_request_v4);
static int applied(const zmk_event_t *eh){struct zitaotech_screen_config_applied_v4 *e=as_zitaotech_screen_config_applied_v4(eh);struct playback_config c;if(e&&e->source!=ZMK_RELAY_EVENT_SOURCE_SELF&&collect(e->interval_ticks,e->enabled_mask&ZITAOTECH_SCREEN_PICTURES_DEFAULT,!!(e->enabled_mask&BIT(31)),e->part>>3,(e->part&7)*4,e->order,&c)){assign(c,true);zitaotech_screen_state_changed();}return ZMK_EV_EVENT_BUBBLE;}
ZMK_LISTENER(screen_v4_applied,applied);ZMK_SUBSCRIPTION(screen_v4_applied,zitaotech_screen_config_applied_v4);
static int commit(void){push();zitaotech_screen_state_changed();return 0;}
static struct settings_handler handler={.name=PREFIX,.h_set=load_setting,.h_commit=commit};
#else
bool zitaotech_screen_right_connected(void){return false;}
void zitaotech_screen_refresh_right(void){}
int zitaotech_screen_set_playback(uint32_t ms,bool sequential,const uint8_t order[28]){return -ENOTSUP;}
int zitaotech_screen_set_interval_s(uint32_t seconds){return -ENOTSUP;}
int zitaotech_screen_set_enabled_mask(uint32_t mask){return -ENOTSUP;}
int zitaotech_screen_reset(void){return -ENOTSUP;}
static struct playback_config pending;
static struct playback_config outbound;
static uint8_t send_generation,send_offset;
static void report_part(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(report_work,report_part);
static void report_part(struct k_work *work){
 k_spinlock_key_t key=k_spin_lock(&lock);struct playback_config c=outbound;unsigned offset=send_offset;unsigned generation=send_generation;if(offset<28)send_offset+=4;k_spin_unlock(&lock,key);
 if(offset>=28)return;
 struct zitaotech_screen_config_applied_v4 e={.interval_ticks=c.ms/100,.enabled_mask=c.mask|(c.sequential?BIT(31):0),.part=(generation<<3)|(offset/4),.source=ZMK_RELAY_EVENT_SOURCE_SELF};memcpy(e.order,c.order+offset,4);raise_zitaotech_screen_config_applied_v4(e);
 k_work_reschedule_for_queue(zmk_display_work_q(),&report_work,K_MSEC(30));
}
static void report(void){struct playback_config c=snapshot(false);k_spinlock_key_t key=k_spin_lock(&lock);outbound=c;send_generation=(send_generation+1)&31;send_offset=0;k_spin_unlock(&lock,key);k_work_reschedule_for_queue(zmk_display_work_q(),&report_work,K_NO_WAIT);}
static void apply_work(struct k_work *work){k_spinlock_key_t key=k_spin_lock(&lock);struct playback_config c=pending;k_spin_unlock(&lock,key);if(save(c))return;assign(c,false);zmk_widget_status_carousel_config_changed();report();}
static K_WORK_DEFINE(apply,apply_work);
static int mirror(const zmk_event_t *eh){struct zitaotech_screen_config_mirror_v4 *e=as_zitaotech_screen_config_mirror_v4(eh);struct playback_config c;if(!e||e->source==ZMK_RELAY_EVENT_SOURCE_SELF||!collect(e->interval_ticks,e->enabled_mask&ZITAOTECH_SCREEN_PICTURES_DEFAULT,!!(e->enabled_mask&BIT(31)),e->part>>3,(e->part&7)*4,e->order,&c))return ZMK_EV_EVENT_BUBBLE;struct playback_config old=snapshot(false);if(memcmp(&old,&c,sizeof(c))){k_spinlock_key_t key=k_spin_lock(&lock);pending=c;k_spin_unlock(&lock,key);k_work_submit_to_queue(zmk_display_work_q(),&apply);}return ZMK_EV_EVENT_BUBBLE;}
ZMK_LISTENER(screen_v4_mirror,mirror);ZMK_SUBSCRIPTION(screen_v4_mirror,zitaotech_screen_config_mirror_v4);
static int query(const zmk_event_t *eh){report();return ZMK_EV_EVENT_BUBBLE;}
ZMK_LISTENER(screen_v4_query,query);ZMK_SUBSCRIPTION(screen_v4_query,zitaotech_screen_config_query_v4);
static struct k_work_delayable poll;
static void poll_work(struct k_work *work){raise_zitaotech_screen_config_request_v4((struct zitaotech_screen_config_request_v4){.source=ZMK_RELAY_EVENT_SOURCE_SELF});k_work_schedule(&poll,K_SECONDS(2));}
static struct settings_handler handler={.name=PREFIX,.h_set=load_setting};
#endif
static int init(void){int err=settings_register(&handler);
#if IS_ENABLED(CONFIG_ZMK_SPLIT)&&!IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
 k_work_init_delayable(&poll,poll_work);k_work_schedule(&poll,K_SECONDS(2));
#endif
 return err;}
SYS_INIT(init,APPLICATION,95);
