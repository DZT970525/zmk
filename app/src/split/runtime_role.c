/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zmk/split/role.h>

/* Boot in dongle-peripheral mode unless a previous transition was committed.
 * Role changes take effect after reboot, avoiding mid-report transport changes.
 * Neither the host nor split bond namespaces are erased during a transition. */
static atomic_t boot_central;
static atomic_t manual_switch_enabled;
static atomic_t standalone_selected;
static atomic_t transition_pending;
static bool settings_committed;
static uint8_t saved_role;
static uint8_t requested_role;
static bool handoff_owned;
static bool manual_transition;

bool zmk_split_role_is_central(void) { return atomic_get(&boot_central) != 0; }
bool zmk_split_role_manual_switch_enabled(void) {
    return atomic_get(&manual_switch_enabled) != 0;
}

bool zmk_split_role_standalone_selected(void) {
    return atomic_get(&standalone_selected)!=0;
}

int zmk_split_role_set_manual_switch_enabled(bool enabled) {
    const uint8_t value = enabled ? 1 : 0;
    int ret = settings_save_one("keypoint_role/manual_switch", &value, sizeof(value));
    if (ret == 0) {
        atomic_set(&manual_switch_enabled, value);
    }
    return ret;
}

static void transition_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    /* Preparation must not change the durable boot role. A power failure
     * before peer/configuration ACKs must retain the current topology. */
    int ret = zmk_keypoint_config_prepare_transition();
    handoff_owned=false;
    atomic_set(&transition_pending, ret ? 0 : 2);
}
static K_WORK_DEFINE(transition_work, transition_work_cb);

bool zmk_split_role_transition_ready(void) {
    int phase=atomic_get(&transition_pending);
    return phase==2 || phase==5;
}
bool zmk_split_role_transition_is_manual(void) {
    return atomic_get(&transition_pending)!=0 && manual_transition;
}

int zmk_split_role_begin_handoff(void) {
    /* Retry an owned handoff after a lost/uncertain remote ACK. It must not
     * become cancellable while the right may already have changed masters. */
    if (atomic_get(&transition_pending)==5 && handoff_owned) { return 0; }
    if (!atomic_cas(&transition_pending,2,5)) { return -EAGAIN; }
    handoff_owned=true;
    return 0;
}

int zmk_split_role_abort_handoff(void) {
    if (!atomic_cas(&transition_pending,5,2)) { return -EAGAIN; }
    handoff_owned=false;
    return 0;
}

int zmk_split_role_transition_target(bool *central) {
    if (!central) { return -EINVAL; }
    if (!zmk_split_role_transition_ready()) { return -EAGAIN; }
    *central=requested_role!=0;
    return 0;
}

static void reboot_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    const uint8_t standalone=manual_transition && requested_role;
    int ret=settings_save_one("keypoint_role/standalone_selected",&standalone,sizeof(standalone));
    if (!ret) ret = settings_save_one("keypoint_role/boot_role", &requested_role,
                                sizeof(requested_role));
    if (ret != 0) {
        atomic_set(&transition_pending, handoff_owned ? 5 : 2);
        return;
    }
    sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DEFINE(reboot_work, reboot_work_cb);

int zmk_split_role_cancel_transition(void) {
    /* Confirmation transfers ownership to the persistence/reboot worker. */
    if (!atomic_cas(&transition_pending, 2, 4)) { return -EAGAIN; }
    zmk_keypoint_config_cancel_transition();
    atomic_clear(&transition_pending);
    return 0;
}

int zmk_split_role_confirm_transition(void) {
    int phase=atomic_get(&transition_pending);
    if ((phase!=2 && phase!=5) || !atomic_cas(&transition_pending, phase, 3)) {
        return -EAGAIN;
    }
    int ret = k_work_submit(&reboot_work);
    if (ret < 0) {
        atomic_set(&transition_pending, handoff_owned ? 5 : 2);
        return ret;
    }
    return 0;
}

static int request_role(bool central,bool manual) {
    if (!settings_committed) {
        return -EAGAIN;
    }
    if (central == zmk_split_role_is_central()) {
        return 0;
    }
    if (!atomic_cas(&transition_pending, 0, 1)) {
        return -EBUSY;
    }
    requested_role = central ? 1 : 0;
    manual_transition=manual;
    int ret = k_work_submit(&transition_work);
    if (ret < 0) {
        atomic_clear(&transition_pending);
        return ret;
    }
    return 0;
}
int zmk_split_role_request(bool central) { return request_role(central,false); }

int zmk_split_role_toggle(void) {
    if (!zmk_split_role_manual_switch_enabled()) {
        return -EPERM;
    }
    return request_role(!zmk_split_role_is_central(),true);
}

static int role_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                             void *cb_arg) {
    bool is_role = strcmp(name, "boot_role") == 0;
    bool is_switch = strcmp(name, "manual_switch") == 0;
    bool is_standalone=strcmp(name,"standalone_selected")==0;
    if (!is_role && !is_switch && !is_standalone) {
        return -ENOENT;
    }
    if (len != 1) {
        return -EINVAL;
    }
    uint8_t value;
    int ret = read_cb(cb_arg, &value, sizeof(value));
    if (ret < 0) {
        return ret;
    }
    if (ret != 1 || value > 1) {
        return -EINVAL;
    }
    if (is_role) {
        saved_role = value;
        if (!settings_committed) {
            atomic_set(&boot_central, value);
        }
    } else if (is_standalone) {
        atomic_set(&standalone_selected,value);
    } else {
        atomic_set(&manual_switch_enabled, value);
    }
    return 0;
}

static int role_settings_commit(void) {
    if (!settings_committed) {
        atomic_set(&boot_central, saved_role);
        settings_committed = true;
    }
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(keypoint_role, "keypoint_role", NULL, role_settings_set,
                               role_settings_commit, NULL);
