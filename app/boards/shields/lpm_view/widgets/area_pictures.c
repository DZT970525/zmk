/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include "area_pictures.h"

#define AREA_SLOTS 10
#define AREA_MASK ((1U << AREA_SLOTS) - 1)
#define PIC_BYTES 1372 /* 69*53 pixels, packed RGB111, three bits per pixel */
/* Slot payloads stay in Flash. The display and Studio reader share one cache;
 * an independent staging buffer keeps an upload from changing visible pixels. */
static uint8_t picture_cache[PIC_BYTES], staging[PIC_BYTES];
static int cached_slot = -1;
static uint32_t crc[AREA_SLOTS], revision;
static bool present[AREA_SLOTS], verified[AREA_SLOTS];
static uint8_t playback[3]={0,60,0}; /* random, interval LE16 */
static uint16_t enabled=AREA_MASK;
static uint8_t order[AREA_SLOTS]={0,1,2,3,4,5,6,7,8,9};
static uint16_t received;
static int transfer_slot=-1, selected=-1;
static int64_t last_tick=-1;
/* A completed upload previews its own slot for one playback interval without
 * changing the saved playlist mask or order. */
static int preview_slot=-1;
static int64_t preview_until;
K_MUTEX_DEFINE(picture_lock);
K_MUTEX_DEFINE(picture_store_lock);

static int picture_settings(const char *name,size_t len,settings_read_cb read,void *arg) {
    if(!strcmp(name,"play")) {
        if(len!=3) return -EINVAL;
        uint8_t value[3];int n=read(arg,value,3);
        if(n!=3||value[0]>3||sys_get_le16(value+1)<1||sys_get_le16(value+1)>600)return -EINVAL;
        memcpy(playback,value,3);return 0;
    }
    if(!strcmp(name,"enabled")){uint8_t bytes[2];if(len!=1&&len!=2)return -EINVAL;int n=read(arg,bytes,len);if(n!=(int)len)return -EINVAL;unsigned value=len==1?bytes[0]:sys_get_le16(bytes);if(value>AREA_MASK)return -EINVAL;enabled=len==1?((AREA_MASK&~3U)|value):value;return 0;}
    if(!strcmp(name,"order")){uint8_t value[AREA_SLOTS];if(len!=AREA_SLOTS||read(arg,value,len)!=AREA_SLOTS)return -EINVAL;unsigned seen=0;for(int i=0;i<AREA_SLOTS;i++){if(value[i]>=AREA_SLOTS||seen&(1U<<value[i]))return -EINVAL;seen|=1U<<value[i];}memcpy(order,value,AREA_SLOTS);return 0;}
    /* Exact small keys: avoid scanf's large startup stack footprint. */
    if((name[0]>='0'&&name[0]<='9') && name[1]=='/') {
        unsigned slot=name[0]-'0';
        if(!strcmp(name+2,"f0") || !strcmp(name+2,"f1")) {
            unsigned part=name[3]-'0';size_t size=part?PIC_BYTES-700:700;
            if(len!=size)return -EINVAL;
            /* Boot loads metadata only; payload and CRC are checked on demand. */
            verified[slot]=false;return 0;
        }
        if(!strcmp(name+2,"meta")) {
            if(len==1){uint8_t empty;int n=read(arg,&empty,1);present[slot]=false;verified[slot]=true;return n==1?0:-EINVAL;}
            if(len!=4)return -EINVAL;
            int n=read(arg,&crc[slot],4);present[slot]=n==4;verified[slot]=false;return n==4?0:-EINVAL;
        }
    }
    return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(keypoint_area,"keypoint_area",NULL,picture_settings,NULL,NULL);
struct picture_load { unsigned parts; };
static int picture_read(const char *name,size_t len,settings_read_cb read,void *arg,void *param) {
    struct picture_load *load=param;
    if(strcmp(name,"f0") && strcmp(name,"f1"))return 0;
    unsigned part=name[1]-'0';size_t size=part?PIC_BYTES-700:700;
    if(len!=size || read(arg,picture_cache+part*700,size)!=(int)size)return -EINVAL;
    load->parts|=1U<<part;return 0;
}
/* Caller holds picture_store_lock then picture_lock, including while using
 * the returned cache. Never return a pointer whose lifetime outlasts the lock. */
static int picture_load_slot(unsigned slot) {
    if(slot>=AREA_SLOTS || !present[slot])return -ENOENT;
    if(cached_slot==(int)slot)return 0;
    cached_slot=-1;
    char subtree[32];snprintf(subtree,sizeof(subtree),"keypoint_area/%u",slot);
    struct picture_load load={0};
    int ret=settings_load_subtree_direct(subtree,picture_read,&load);
    if(ret || load.parts!=3 || crc32_ieee(picture_cache,PIC_BYTES)!=crc[slot]) {
        present[slot]=false;verified[slot]=true;
        return ret?ret:-EBADMSG;
    }
    cached_slot=slot;verified[slot]=true;return 0;
}
static uint16_t mask(void) {
    uint16_t value=0;
    for(int i=0;i<AREA_SLOTS;i++) {
        /* Metadata only: do not scan Flash for every slot before the first frame. */
        if(present[i])value|=1<<i;
    }
    return value;
}
uint32_t keypoint_area_picture_revision(void) {
    k_mutex_lock(&picture_lock,K_FOREVER);
    uint32_t value=revision*100003U+k_uptime_get()/1000/sys_get_le16(playback+1);
    k_mutex_unlock(&picture_lock);return value;
}
/* Small 3x5 bitmap text, written directly to the existing canvas. */
static void empty_text(lv_obj_t *canvas,int x,int y,const char *text) {
    static const char chars[]="Adyourpicte";
    static const uint16_t glyphs[]={0x2bed,0x12eb,0xb59,0x56a,0xb6f,0x724,0xd74,0x2092,0x723,0x2e93,0x573};
    for(;*text;text++,x+=8){const char *g=strchr(chars,*text);if(!g)continue;uint16_t bits=glyphs[g-chars];
        for(int row=0;row<5;row++)for(int col=0;col<3;col++)if(bits&(1U<<(14-row*3-col)))
            for(int yy=0;yy<2;yy++)for(int xx=0;xx<2;xx++)lv_canvas_set_px(canvas,x+col*2+xx,y+row*2+yy,lv_color_make(255,255,255),LV_OPA_COVER);
    }
}
void keypoint_area_picture_draw(lv_obj_t *canvas) {
    k_mutex_lock(&picture_store_lock,K_FOREVER);
    k_mutex_lock(&picture_lock,K_FOREVER);
    uint16_t available=mask() & enabled;
    int64_t tick=k_uptime_get()/1000/sys_get_le16(playback+1);
    if(preview_slot>=0 && present[preview_slot] && k_uptime_get()<preview_until) {
        selected=preview_slot;
        last_tick=tick;
    } else {
    if(preview_slot>=0)last_tick=-1;
    preview_slot=-1;
    if(tick!=last_tick||selected<0||!(available&(1<<selected))) {
        int options[AREA_SLOTS],count=0;for(int i=0;i<AREA_SLOTS;i++){int slot=order[(playback[0]&2)?AREA_SLOTS-1-i:i];if(available&(1U<<slot))options[count++]=slot;}
        if(count) {
            if(playback[0]&1)selected=options[sys_rand32_get()%count];
            else {int next=0;for(int i=0;i<count;i++)if(options[i]==selected){next=(i+1)%count;break;}selected=options[next];}
        } else selected=-1;
        last_tick=tick;
    }
    }
    /* Load only the chosen picture. A corrupt slot is removed by the loader;
     * fall through to the next enabled slot so a valid picture still appears. */
    for(int attempt=0;selected>=0 && attempt<AREA_SLOTS;attempt++) {
        if(!picture_load_slot(selected))break;
        available &= ~(1U<<selected);
        selected=-1;
        for(int i=0;i<AREA_SLOTS;i++) {
            int slot=order[(playback[0]&2)?AREA_SLOTS-1-i:i];
            if(available&(1U<<slot)){selected=slot;break;}
        }
    }
    for(int y=0;y<53;y++)for(int x=0;x<69;x++) {
        unsigned pixel=y*69+x,bit=pixel*3,index=0;
        if(selected>=0)for(int b=0;b<3;b++)index=(index<<1)|((picture_cache[(bit+b)/8]>>(7-(bit+b)%8))&1);
        lv_color_t color=lv_color_make(index&4?255:0,index&2?255:0,index&1?255:0);
        lv_canvas_set_px(canvas,x+2,y+18,color,LV_OPA_COVER);
    }
    if(selected<0){empty_text(canvas,5,32,"Add your");empty_text(canvas,9,48,"picture");}
    k_mutex_unlock(&picture_lock);k_mutex_unlock(&picture_store_lock);lv_obj_invalidate(canvas);
}

int keypoint_area_picture_write(uint8_t slot,const uint8_t *data,size_t len) {
    if(slot>=AREA_SLOTS || len!=PIC_BYTES)return -EINVAL;
    k_mutex_lock(&picture_store_lock,K_FOREVER);
    char key[40];uint32_t sum=crc32_ieee(data,len);int ret=0;
    for(int part=0;part<2&&!ret;part++){snprintf(key,sizeof(key),"keypoint_area/%u/f%d",slot,part);ret=settings_save_one(key,data+part*700,part?PIC_BYTES-700:700);}
    if(!ret){snprintf(key,sizeof(key),"keypoint_area/%u/meta",slot);ret=settings_save_one(key,&sum,4);}
    k_mutex_lock(&picture_lock,K_FOREVER);
    if(!ret){memcpy(picture_cache,data,PIC_BYTES);cached_slot=slot;crc[slot]=sum;present[slot]=verified[slot]=true;
        preview_slot=slot;preview_until=k_uptime_get()+(int64_t)sys_get_le16(playback+1)*1000;
        selected=slot;revision++;last_tick=-1;}
    else {cached_slot=-1;verified[slot]=false;revision++;last_tick=-1;}
    k_mutex_unlock(&picture_lock);k_mutex_unlock(&picture_store_lock);return ret;
}

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>
#include <pb_encode.h>
static uint8_t reply[87];
static size_t reply_len=66;
static bool encode(pb_ostream_t *stream,const pb_field_t *field,void *const *arg) {
    ARG_UNUSED(arg);return pb_encode_tag_for_field(stream,field)&&pb_encode_string(stream,reply,reply_len);
}
static bool rpc(const zmk_custom_CallRequest *request,pb_callback_t *output) {
    const uint8_t *p=request->payload.bytes;size_t len=request->payload.size;int ret=-EINVAL;
    reply_len=66;
    k_mutex_lock(&picture_store_lock,K_FOREVER);
    k_mutex_lock(&picture_lock,K_FOREVER);
    if(len==5&&p[0]==1&&p[1]==6&&p[2]<AREA_SLOTS&&sys_get_le16(p+3)<PIC_BYTES&&(mask()&(1<<p[2]))) {
        uint16_t offset=sys_get_le16(p+3);size_t size=PIC_BYTES-offset<20?PIC_BYTES-offset:20;
        ret=picture_load_slot(p[2]);
        if(!ret){memset(reply+67,0,20);memcpy(reply+67,picture_cache+offset,size);reply[66]=size;reply_len=87;}
    } else if(len==2&&p[0]==1&&p[1]==0)ret=0;
    else if(len==5&&p[0]==1&&p[1]==2&&p[2]<=3&&sys_get_le16(p+3)>=1&&sys_get_le16(p+3)<=600) {
        k_mutex_unlock(&picture_lock);
        ret=settings_save_one("keypoint_area/play",p+2,3);
        k_mutex_lock(&picture_lock,K_FOREVER);
        if(!ret){memcpy(playback,p+2,3);revision++;selected=-1;last_tick=-1;}
    } else if((len==3||len==4)&&p[0]==1&&p[1]==5&&(len==3?p[2]:sys_get_le16(p+2))<=AREA_MASK) {
        uint16_t next=len==3?((enabled&~255U)|p[2]):sys_get_le16(p+2);uint8_t value[2];sys_put_le16(next,value);
        k_mutex_unlock(&picture_lock);ret=settings_save_one("keypoint_area/enabled",value,2);k_mutex_lock(&picture_lock,K_FOREVER);
        if(!ret){enabled=next;revision++;last_tick=-1;}
    } else if(len==2+AREA_SLOTS&&p[0]==1&&p[1]==7) {
        unsigned seen=0;bool valid=true;for(int i=0;i<AREA_SLOTS;i++){if(p[i+2]>=AREA_SLOTS||seen&(1U<<p[i+2])){valid=false;break;}seen|=1U<<p[i+2];}
        if(valid){k_mutex_unlock(&picture_lock);ret=settings_save_one("keypoint_area/order",p+2,AREA_SLOTS);k_mutex_lock(&picture_lock,K_FOREVER);if(!ret){memcpy(order,p+2,AREA_SLOTS);revision++;selected=-1;last_tick=-1;}}
    } else if(len==3&&p[0]==1&&p[1]==4&&p[2]<AREA_SLOTS) {
        char key[40];uint8_t empty=0;snprintf(key,sizeof(key),"keypoint_area/%u/meta",p[2]);
        k_mutex_unlock(&picture_lock);
        ret=settings_save_one(key,&empty,1);
        k_mutex_lock(&picture_lock,K_FOREVER);
        if(!ret){present[p[2]]=false;verified[p[2]]=true;if(cached_slot==p[2])cached_slot=-1;revision++;last_tick=-1;}
    } else if(len>5&&len<=25&&p[0]==1&&p[1]==3&&p[2]<AREA_SLOTS) {
        uint16_t offset=sys_get_le16(p+3);size_t size=len-5;
        if(offset==0){transfer_slot=p[2];received=0;}
        if(transfer_slot==p[2]&&offset==received&&size<=PIC_BYTES-received) {
            memcpy(staging+received,p+5,size);received+=size;ret=0;
            if(received==PIC_BYTES) {
                k_mutex_unlock(&picture_lock);
                ret=keypoint_area_picture_write(p[2],staging,PIC_BYTES);
                k_mutex_lock(&picture_lock,K_FOREVER);
                transfer_slot=-1;
            }
        }
    }
    reply[0]=1;sys_put_le32(ret,reply+1);reply[5]=mask();reply[6]=playback[0];
    memcpy(reply+7,playback+1,2);sys_put_le16(received,reply+9);reply[11]=transfer_slot<0?255:transfer_slot;
    reply[20]=enabled;
    for(int i=0;i<2;i++)sys_put_le32(crc[i],reply+12+i*4);
    reply[21]=AREA_SLOTS;reply[22]=mask()>>8;reply[23]=enabled>>8;
    for(int i=2;i<AREA_SLOTS;i++)sys_put_le32(crc[i],reply+24+(i-2)*4);
    memcpy(reply+56,order,AREA_SLOTS);
    k_mutex_unlock(&picture_lock);
    k_mutex_unlock(&picture_store_lock);
    output->funcs.encode=encode;output->arg=NULL;return true;
}
static struct zmk_rpc_custom_subsystem_meta meta={.security=ZMK_STUDIO_RPC_HANDLER_SECURED};
ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech_area_pictures,&meta,rpc);
#endif
