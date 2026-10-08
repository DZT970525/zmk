#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <string.h>
#include <zmk/endpoints.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include "screen_transfer.h"
#include "screen_image_store.h"
#include "screen_config.h"

/* Version 1: token/op/slot/format/u16 offset; BEGIN adds size+CRC.
 * DATA payload follows offset. ACK echoes token/op/slot, result, offset, CRC.
 * Stop-and-wait with idempotent retries; all flash work happens here, never
 * inside the Bluetooth receive callback. MTU 23 works without negotiation. */
#define MAX_PACKET 244
#define BEGIN 1
#define DATA 2
#define COMMIT 3
#define ABORT 4
#define DELETE 5
#define DELETE_JOB BIT(31)
static atomic_t busy;
static struct k_spinlock status_lock;
static struct zitaotech_image_transfer_status status;
static int preferred = -1;
static uint8_t token;
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static uint8_t staging[4320];
#endif

struct packet { uint16_t len; uint8_t bytes[MAX_PACKET]; };
K_MSGQ_DEFINE(rx_queue, sizeof(struct packet), 4, 4);
K_MSGQ_DEFINE(ack_queue, 10, 4, 4);
K_MSGQ_DEFINE(job_queue, sizeof(uint32_t), 1, 4);

bool zitaotech_screen_transfer_active(void) { return atomic_get(&busy) != 0; }
int zitaotech_screen_preferred_image(void) {
    int slot = preferred >= 0 ? preferred : zitaotech_screen_image_last_slot();
    return slot >= 0 && zitaotech_screen_image_data(slot) ? slot : -1;
}
void zitaotech_screen_image_transfer_status(struct zitaotech_image_transfer_status *out) {
    k_spinlock_key_t key = k_spin_lock(&status_lock);
    *out = status;
    k_spin_unlock(&status_lock, key);
}
static void update(uint32_t slot, uint32_t sent, uint32_t total, uint32_t crc, int error) {
    k_spinlock_key_t key = k_spin_lock(&status_lock);
    status = (struct zitaotech_image_transfer_status){ .active = atomic_get(&busy),
        .slot = slot, .sent = sent, .total = total, .crc = crc, .error = error };
    k_spin_unlock(&status_lock, key);
}
static void set_busy(bool value) {
    atomic_set(&busy, value);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zmk_endpoint_clear_reports();
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    zmk_endpoint_send_mouse_report();
#endif
#endif
    zmk_widget_status_transfer_changed();
}
int zitaotech_image_receive(const void *data, size_t len) {
    if (len < 6 || len > MAX_PACKET) return -EINVAL;
    struct packet packet = { .len = len };
    memcpy(packet.bytes, data, len);
    return k_msgq_put(&rx_queue, &packet, K_NO_WAIT);
}
void zitaotech_image_ack_receive(const void *data, size_t len) {
    if (len == 10) k_msgq_put(&ack_queue, data, K_NO_WAIT);
}
int zitaotech_screen_image_sync(uint32_t slot) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    struct zitaotech_screen_image_info info;
    if (!zitaotech_screen_image_info(slot, &info)) return -ENOENT;
    if (zitaotech_image_link_mtu() < 23) return -ENOTCONN;
    if (!atomic_cas(&busy, 0, 1)) return -EBUSY;
    update(slot, 0, info.size, info.crc32, 0);
    if (k_msgq_put(&job_queue, &slot, K_NO_WAIT)) {
        atomic_clear(&busy); return -EBUSY;
    }
    return 0;
#else
    return -ENOTSUP;
#endif
}
int zitaotech_screen_image_delete(uint32_t slot) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (slot >= 2) return -EINVAL;
    if (zitaotech_image_link_mtu() < 23) return -ENOTCONN;
    if (!atomic_cas(&busy, 0, 1)) return -EBUSY;
    update(slot, 0, 1, 0, 0);
    uint32_t job = slot | DELETE_JOB;
    if (k_msgq_put(&job_queue, &job, K_NO_WAIT)) { atomic_clear(&busy); return -EBUSY; }
    return 0;
#else
    return -ENOTSUP;
#endif
}
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static int exchange(uint8_t *packet, size_t len, uint16_t expected, uint32_t crc) {
    uint8_t ack[10];
    for (int retry = 0; retry < 4; retry++) {
        int ret = zitaotech_image_link_send(packet, len);
        if (ret == -ENOTCONN) return ret;
        int64_t deadline = k_uptime_get() + (packet[1] == COMMIT ? 5000 : 1000);
        while (ret == 0 && k_uptime_get() < deadline) {
            if (k_msgq_get(&ack_queue, ack, K_MSEC(MAX(1, deadline-k_uptime_get())))) break;
            if (ack[0] != packet[0] || ack[1] != packet[1] || ack[2] != packet[2]) continue;
            if (ack[3]) return -EIO;
            if (sys_get_le16(ack+4) != expected) continue;
            if (packet[1] == COMMIT && sys_get_le32(ack+6) != crc) return -EBADMSG;
            return 0;
        }
        k_sleep(K_MSEC(30));
    }
    return -ETIMEDOUT;
}
static void transfer_thread(void *a, void *b, void *c) {
    uint32_t slot;
    while (true) {
        k_msgq_get(&job_queue, &slot, K_FOREVER);
        if (slot & DELETE_JOB) {
            slot &= ~DELETE_JOB;
            set_busy(true);
            k_msgq_purge(&ack_queue);
            uint8_t erase[6] = {++token, DELETE, slot, 0, 0, 0};
            int ret = exchange(erase, sizeof(erase), 0, 0);
            if (!ret) ret = zitaotech_screen_image_erase(slot);
            if (!ret) {
                uint16_t mask = zitaotech_screen_enabled_mask() & ~BIT(8 + slot);
                bool any = (mask & 0xff) != 0;
                for (unsigned i = 0; i < 2; i++)
                    any |= (mask & BIT(8 + i)) && zitaotech_screen_image_data(i);
                if (!any) mask |= BIT(0);
                zitaotech_screen_set_enabled_mask(mask);
            }
            set_busy(false);
            update(slot, ret ? 0 : 1, 1, 0, ret);
            continue;
        }
        struct zitaotech_screen_image_info info;
        if (!zitaotech_screen_image_info(slot, &info)) { set_busy(false); continue; }
        set_busy(true);
        k_msgq_purge(&ack_queue);
        uint8_t packet[MAX_PACKET] = { ++token, BEGIN, slot, info.format };
        sys_put_le16(0, packet+4);
        sys_put_le16(info.size, packet+6);
        sys_put_le32(info.crc32, packet+8);
        int ret = exchange(packet, 12, 0, info.crc32);
        uint32_t sent = 0;
        const uint8_t *image = zitaotech_screen_image_data(slot);
        while (!ret && sent < info.size) {
            int mtu = zitaotech_image_link_mtu();
            if (mtu < 23) { ret = -ENOTCONN; break; }
            size_t size = MIN(info.size-sent, MIN(MAX_PACKET, mtu-3)-6);
            packet[1] = DATA;
            sys_put_le16(sent, packet+4);
            memcpy(packet+6, image+sent, size);
            ret = exchange(packet, 6+size, sent+size, info.crc32);
            if (!ret) { sent += size; update(slot, sent, info.size, info.crc32, 0); }
        }
        if (!ret) {
            packet[1] = COMMIT;
            sys_put_le16(sent, packet+4);
            ret = exchange(packet, 6, sent, info.crc32);
        }
        if (ret) { packet[1] = ABORT; zitaotech_image_link_send(packet, 6); }
        set_busy(false);
        update(slot, sent, info.size, info.crc32, ret);
    }
}
#else
static void transfer_thread(void *a, void *b, void *c) {
    struct packet packet;
    uint8_t session = 0, slot = 0, format = 0;
    uint16_t total = 0, received = 0;
    uint32_t crc = 0;
    uint8_t committed_token = 0, committed_slot = 0;
    uint16_t committed_size = 0;
    uint32_t committed_crc = 0;
    bool committed = false;
    while (true) {
        if (k_msgq_get(&rx_queue, &packet, K_SECONDS(10))) {
            if (atomic_get(&busy)) { set_busy(false); update(slot, received, total, crc, -ETIMEDOUT); }
            continue;
        }
        uint8_t *p = packet.bytes;
        uint16_t offset = sys_get_le16(p+4);
        int error = 0;
        if (p[1] == DELETE && packet.len == 6 && p[2] < 2 && offset == 0) {
            if (atomic_get(&busy)) error = 2;
            else {
                error = zitaotech_screen_image_erase(p[2]) ? 5 : 0;
                if (!error) { preferred = -1; committed = false; zmk_widget_status_transfer_changed(); }
            }
            uint8_t ack[10] = {p[0], p[1], p[2], error};
            zitaotech_image_link_ack(ack, sizeof(ack));
            continue;
        }
        if (p[1] == BEGIN && packet.len == 12) {
            uint16_t size = sys_get_le16(p+6);
            if (p[2] >= 2 || p[3] > 1 || size != (p[3] ? 1080 : 4320)) error = 1;
            else {
                session=p[0]; slot=p[2]; format=p[3]; total=size;
                crc=sys_get_le32(p+8); received=0; committed=false; set_busy(true);
            }
        } else if (p[1] == COMMIT && committed && p[0] == committed_token &&
                   p[2] == committed_slot && offset == committed_size) {
            uint8_t ack[10] = {p[0], p[1], p[2], 0};
            sys_put_le16(committed_size, ack+4); sys_put_le32(committed_crc, ack+6);
            zitaotech_image_link_ack(ack, sizeof(ack)); continue;
        } else if (!atomic_get(&busy) || p[0] != session || p[2] != slot) error = 2;
        else if (p[1] == DATA) {
            size_t size = packet.len-6;
            if (!size || offset+size > total || offset > received) error=3;
            else if (offset == received) { memcpy(staging+offset, p+6, size); received += size; }
            else if (offset+size > received || memcmp(staging+offset,p+6,size)) error=3;
        } else if (p[1] == COMMIT) {
            if (offset != total || received != total || crc32_ieee(staging,total) != crc) error=4;
            else if (zitaotech_screen_image_write(slot,format,staging,total)) error=5;
            else {
                preferred=slot; committed=true; committed_token=session;
                committed_slot=slot; committed_size=total; committed_crc=crc;
            }
            set_busy(false);
        } else if (p[1] == ABORT) set_busy(false);
        else error=6;
        uint8_t ack[10] = {p[0], p[1], p[2], error};
        sys_put_le16(received, ack+4); sys_put_le32(crc, ack+6);
        zitaotech_image_link_ack(ack, sizeof(ack));
        if (error && atomic_get(&busy)) set_busy(false);
        update(slot,received,total,crc,error ? -EIO : 0);
    }
}
#endif
K_THREAD_DEFINE(image_transfer_thread, 3072, transfer_thread, NULL, NULL, NULL, 10, 0, 0);




