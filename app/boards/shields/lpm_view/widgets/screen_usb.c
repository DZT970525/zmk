#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <string.h>
#include <zmk/screen_usb.h>
#include "screen_transfer.h"
#include "screen_image_store.h"
#if (IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN))
#include "area_pictures.h"
#endif

/* USB image v1, independent of Studio protobuf data packets:
 * A5 5A 01 op token slot format 00 offset:u16 total:u16 length:u16
 * payload (max 256), then CRC32 of header+payload. Studio remains the
 * control/status channel. Normal Studio frames can occur between packets. */
#define USB_HEADER 14
#define USB_PACKET_MAX (USB_HEADER + 256 + 4)
struct usb_packet { uint16_t len; uint8_t bytes[USB_PACKET_MAX]; };
K_MSGQ_DEFINE(usb_image_packets, sizeof(struct usb_packet), 4, 4);
static struct usb_packet parsing;
static unsigned expected;
static int64_t last_byte;
static bool rpc_frame, rpc_escape;
static struct k_spinlock parser_lock, status_lock;
static atomic_t active, overflow;
static struct zitaotech_usb_image_status status;
static uint8_t *image;

bool zitaotech_usb_image_active(void) { return atomic_get(&active); }
void zitaotech_usb_image_status(struct zitaotech_usb_image_status *out) {
    k_spinlock_key_t key=k_spin_lock(&status_lock); *out=status; k_spin_unlock(&status_lock,key);
}
static void publish(struct zitaotech_usb_image_status value) {
    k_spinlock_key_t key=k_spin_lock(&status_lock); status=value; k_spin_unlock(&status_lock,key);
}
bool zitaotech_usb_image_feed(uint8_t byte) {
    k_spinlock_key_t key=k_spin_lock(&parser_lock);
    if (!parsing.len) {
        if (rpc_frame) {
            if (rpc_escape) rpc_escape=false;
            else if (byte==0xac) rpc_escape=true;
            else if (byte==0xad) rpc_frame=false;
            k_spin_unlock(&parser_lock,key); return false;
        }
        if (byte==0xab) { rpc_frame=true; rpc_escape=false; }
        if (byte!=0xa5) { k_spin_unlock(&parser_lock,key); return false; }
        expected=USB_HEADER;
    }
    last_byte=k_uptime_get();
    parsing.bytes[parsing.len++]=byte;
    if ((parsing.len==2 && byte!=0x5a) || (parsing.len==3 && byte!=1)) {
        parsing.len=0; k_spin_unlock(&parser_lock,key); return true;
    }
    if (parsing.len==USB_HEADER) {
        unsigned length=sys_get_le16(parsing.bytes+12);
        if (length>256) { parsing.len=0; atomic_set(&overflow,1); }
        else expected=USB_HEADER+length+4;
    } else if (parsing.len==expected) {
        if (k_msgq_put(&usb_image_packets,&parsing,K_NO_WAIT)) atomic_set(&overflow,1);
        parsing.len=0;
    }
    k_spin_unlock(&parser_lock,key); return true;
}
static void usb_image_thread(void *a, void *b, void *c) {
    struct usb_packet packet;
    struct zitaotech_usb_image_status value={0};
    uint8_t format=0;
    int64_t activity=0;
    while (true) {
        if (k_msgq_get(&usb_image_packets,&packet,K_SECONDS(1))) {
            k_spinlock_key_t key=k_spin_lock(&parser_lock);
            if (parsing.len && k_uptime_get()-last_byte>2000) { parsing.len=0; atomic_set(&overflow,1); }
            k_spin_unlock(&parser_lock,key);
            if ((atomic_get(&active) && k_uptime_get()-activity>10000) || atomic_get(&overflow)) {
                atomic_clear(&overflow); atomic_clear(&active);
                zitaotech_screen_image_upload_end(2);
                value.active=false; value.error=-ETIMEDOUT; publish(value);
            }
            continue;
        }
        uint8_t *p=packet.bytes;
        unsigned len=sys_get_le16(p+12), offset=sys_get_le16(p+8), total=sys_get_le16(p+10);
        int error=0;
        if (crc32_ieee(p,USB_HEADER+len)!=sys_get_le32(p+USB_HEADER+len) || p[7]) error=-EBADMSG;
        else if (atomic_get(&overflow)) { atomic_clear(&overflow); error=-ENOBUFS; }
        else if (p[3]==1) {
            if (len!=4 || offset || (p[6]==3 ? p[5]>=10 : p[5]>=ZITAOTECH_SCREEN_IMAGE_SLOTS) || (p[6]>1 && !((IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN)) && p[6]==3)) || total!=(p[6]==3 ? 1372 : p[6] ? 1080 : 4320)) error=-EINVAL;
            else if (zitaotech_screen_transfer_active() || zitaotech_screen_rpc_upload_active()) error=-EBUSY;
            else {
                value=(struct zitaotech_usb_image_status){.active=true,.token=p[4],.slot=p[5],.total=total,.crc=sys_get_le32(p+USB_HEADER)};
                image=zitaotech_screen_image_upload_buffer(2);
                if (!image) error=-EBUSY;
                format=p[6]; if (!error) atomic_set(&active,1);
            }
        } else if (p[3]==3 && value.stored && p[4]==value.token && p[5]==value.slot &&
                   p[6]==format && total==value.total && offset==total && !len) {
            /* A repeated COMMIT confirms the previous write without rewriting NVS. */
        } else if (!atomic_get(&active) || p[4]!=value.token || p[5]!=value.slot ||
                   p[6]!=format || total!=value.total) error=-EINVAL;
        else if (p[3]==2) {
            if (!len || offset+len>total || offset>value.received) error=-EINVAL;
            else if (offset==value.received) { memcpy(image+offset,p+USB_HEADER,len); value.received+=len; }
            else if (offset+len>value.received || memcmp(image+offset,p+USB_HEADER,len)) error=-EBADMSG;
        } else if (p[3]==3) {
            if (len || offset!=total || value.received!=total || crc32_ieee(image,total)!=value.crc) error=-EBADMSG;
            else {
#if (IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN))
                if(format==3)error=keypoint_area_picture_write(value.slot,image,total);
                else
#endif
                error=zitaotech_screen_image_write(value.slot,format,image,total);
                value.stored=!error;
            }
            atomic_clear(&active);
        } else if (p[3]==4 && !len) atomic_clear(&active);
        else error=-EINVAL;
        if (error) { atomic_clear(&active); value.stored=false; }
        if (!atomic_get(&active)) zitaotech_screen_image_upload_end(2);
        value.active=atomic_get(&active); value.op=p[3]; value.error=error;
        if (error) { value.token=p[4]; value.slot=p[5]; }
        activity=k_uptime_get(); publish(value);
    }
}
K_THREAD_DEFINE(usb_image_worker,3072,usb_image_thread,NULL,NULL,NULL,10,0,0);
