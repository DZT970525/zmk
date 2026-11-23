"""Build isolated per-hand instances of the maintained pointing implementations.

Only SOFLE's opt-in modular build uses these instances. Protocol messages remain
identical to the existing trackpad/TrackPoint protocol; state, settings, events
and RPC identifiers are distinct. No generated files are written into sources.
"""
from pathlib import Path
import re, sys

board, out = (Path(p).resolve() for p in sys.argv[1:3])
keypoint = board.name == 'zitaotech_keypoint'
drivers = board.parent/'zitaotech_sofle'
scroll_position = 49 if keypoint else 61
root = board.parents[3]
features = root / 'zmk-features'
pad = features / 'zmk-feature-zitaotech-trackpad'
tp = features / 'zmk-feature-zitaotech-trackpoint'
out.mkdir(parents=True, exist_ok=True)

def read(p): return p.read_text(encoding='utf-8')
def write(name, text):
    p = out / name
    p.parent.mkdir(parents=True, exist_ok=True)
    if not p.exists() or read(p) != text: p.write_text(text, encoding='utf-8')

headers = { 'trackpad': read(pad/'include/zitaotech/zmk/trackpad.h'),
            'trackpoint': read(tp/'include/zitaotech/zmk/trackpoint.h'),
            'pointer_curve': read(tp/'include/zitaotech/zmk/pointer_curve.h'),
            'pointing_profiles': read(root/'app/include/zmk/pointing_profiles.h') }
mapping = {}
for kind, text in headers.items():
    prefix = {'trackpad':'sofle_right_trackpad', 'trackpoint':'sofle_left_trackpoint',
              'pointer_curve':'sofle_left_pointer_curve', 'pointing_profiles':'sofle_extra_pointing'}[kind]
    old = 'zmk_pointing' if kind == 'pointing_profiles' else 'zitaotech_' + kind
    for token in re.findall(r'\b'+old+r'\w*', text): mapping[token] = token.replace(old, prefix, 1)
for token in re.findall(r'\bzitaotech_pointer_(?:rise_rate|release_decay)\w*', headers['pointer_curve']):
    mapping[token] = token.replace('zitaotech_pointer_', 'sofle_left_pointer_', 1)

sources = {
 'right_trackpad.c': read(pad/'src/trackpad.c'),
 'left_trackpoint.c': read(tp/'src/trackpoint.c'),
 'left_curve.c': read(tp/'src/pointer_curve.c'),
 'extra_profiles.c': read(drivers/'custom_driver/host_pointing_profiles.c'),
 'right_trackpad_rpc.c': read(pad/'src/studio/trackpad_handler.c'),
 'left_trackpoint_rpc.c': read(tp/'src/studio/trackpoint_handler.c'),
}
# Section/linker registrations must be unique, even when their callbacks are static.
for text in list(sources.values()) + [read(board/'custom_driver/input_processor_trackpad.c')]:
    for token in re.findall(r'(?:ZMK_EVENT_(?:DECLARE|IMPL)|ZMK_LISTENER|SETTINGS_STATIC_HANDLER_DEFINE|LOG_MODULE_REGISTER|SYS_INIT|K_WORK_DEFINE|K_WORK_DELAYABLE_DEFINE|K_MUTEX_DEFINE|K_SEM_DEFINE|K_MSGQ_DEFINE)\(\s*(\w+)', text):
        mapping.setdefault(token, 'modular_' + token)
for event in list(mapping):
    for prefix in ('raise_', 'as_', 'new_', 'free_', 'copy_'):
        mapping[prefix+event] = prefix+mapping[event]
mapping.update({name:'modular_'+name for name in ['zmk_input_processor_temp_layer_enabled','zmk_input_processor_temp_layer_timeout_ms','zmk_pointing_smooth_scrolling_enabled']})
mapping.update({'zitaotech__trackpad':'sofle__right_touchpad',
                'zitaotech__trackpoint':'sofle__left_trackpoint',
                'zitaotech_trackpad_rpc_handle_request':'modular_pad_rpc',
                'zitaotech_trackpoint_rpc_handle_request':'modular_tp_rpc',
                'tsk':'lsk','tpm':'lpm','tps':'lps','tpc':'lpc','tpd':'lpd','tpa':'rpa','tld':'rld','spr':'xpr',
                'sofle_pointing_profile_reset':'extra_profile_reset',
                'as_sofle_pointing_profile_reset':'as_extra_profile_reset',
                'raise_sofle_pointing_profile_reset':'raise_extra_profile_reset'})

def transform(text):
    text = re.sub(r'\b[A-Za-z_]\w*\b', lambda m: mapping.get(m[0], m[0]), text)
    for kind in headers:
        path = 'zmk/pointing_profiles.h' if kind=='pointing_profiles' else 'zitaotech/zmk/'+kind+'.h'
        text = text.replace('<'+path+'>', '<sofle_modular/'+kind+'.h>')
    return text.replace('"ztp"','"sofle_right_pad"').replace('"unified_trackpoint"','"sofle_left_tp"').replace('"sofle_pointing','"sofle_extra_pointing').replace('"sofle_rates','"sofle_extra_rates').replace('"tpc"','"lpc"').replace('"zitaotech_tpcurve','"sofle_left_curve')

for name, text in headers.items(): write('sofle_modular/'+name+'.h', transform(text))
for name, text in sources.items():
    text = transform(text)
    if name == 'right_trackpad_rpc.c': text = text.replace('CONFIG_ZITAOTECH_TRACKPAD_DRIVER_NAME', '"Right_hand_Touchpad"')
    if name == 'left_trackpoint_rpc.c': text = text.replace('CONFIG_ZITAOTECH_TRACKPOINT_DRIVER_NAME', '"Left_hand_TrackPoint"')
    if name == 'right_trackpad.c':
        text = text.replace('#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE)) || (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_LEFT))', '#if 1')
        text = text.replace('#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE))', '#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)')
        text += '\n#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)\nvoid sofle_modular_sync_right_pad(void) { k_work_reschedule(&modular_led_dpi_send_work, K_NO_WAIT); }\n#endif\n'
    if name == 'left_trackpoint.c':
        text = text.replace('CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_KEY_POSITION',str(scroll_position))
    if name == 'right_trackpad.c':
        text = text.replace('CONFIG_ZITAOTECH_TRACKPAD_SCROLL_KEY_POSITION',str(scroll_position))
    write(name, text)

# The left TrackPoint processor has its own fractional remainders and settings.
text = transform(read(drivers/'custom_driver/input_processor_trackpoint_curve.c'))
text = text.replace('zmk_input_processor_trackpoint_dpi','zmk_input_processor_sofle_left_trackpoint')
text = text.replace('sofle_trackpoint_dpi_get()', 'sofle_left_trackpoint_dpi_value()')
write('left_tp_processor.c', text)
text = transform(read(drivers/'custom_driver/input_processor_trackpad.c'))
text = text.replace('zmk_input_processor_dongle_trackpad','zmk_input_processor_sofle_right_trackpad')
text = text.replace('indicator_tp_arrow_fired();','sofle_modular_right_arrow();')
text = '#include "modular.h"\n'+text
write('right_pad_processor.c', text)

# Local sensor drivers: the same code runs on each slot's physical pins.
left = sys.argv[3] == 'left'
motion_pin = (8 if left else 14) if keypoint else (5 if left else 14)
sensor_prefix = f'#define MOTION_GPIO_PIN {motion_pin}\n#include "modular.h"\n'
text = read(drivers/'custom_driver_left/a320.c')
text = re.sub(r'\(IS_ENABLED\(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE\) \|\| IS_ENABLED\(CONFIG_ZITAOTECH_KEYPOINT_SCREEN\)\)', '1', text)
text = text.replace('#if ((IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_LEFT)) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN))', '#if 1')
text = text.replace('static int a320_init(const struct device *dev) {', 'static int a320_init(const struct device *dev) {\n    if (sofle_modular_local_kind() != SOFLE_MODULE_TOUCHPAD) return -ENODEV;')
if not left: text = transform(text)
write('local_a320.c', sensor_prefix+text)
text = read(drivers/'custom_driver_right/trackpoint_curve.c').replace('last_packet_time','trackpoint_packet_time').replace('#define TP_WORKQ_PRIORITY 5','#define TP_WORKQ_PRIORITY 9')
text = text.replace('static int trackpoint_init(const struct device *dev) {', 'static int trackpoint_init(const struct device *dev) {\n    if (sofle_modular_local_kind() != SOFLE_MODULE_TRACKPOINT) return -ENODEV;')
if left:
    text = transform(text).replace('"pointer_velocity.h"','"left_pointer_velocity.h"')
write('local_trackpoint.c', (sensor_prefix.replace(f'MOTION_GPIO_PIN {motion_pin}', f'MOTION_GPIO_PIN {26 if left else 7}') if keypoint else sensor_prefix)+text)
write('left_pointer_velocity.h', transform(read(drivers/'custom_driver_right/pointer_velocity.h')))
for name, src, init, kind in [
 ('local_pad_led.c',drivers/'custom_driver_left/trackpad_led.c','indicator_tp_init','TOUCHPAD'),
 ('local_tp_led.c',drivers/'custom_driver_right/custom_led.c','init_led_follow','TRACKPOINT')]:
    text = read(src).replace('static int '+init+'(void) {','static int '+init+'(void) {\n    if (sofle_modular_local_kind() != SOFLE_MODULE_'+kind+') return 0;')
    if (kind=='TRACKPOINT' and left) or (kind=='TOUCHPAD' and not left): text = transform(text)
    # Chosen LED devices select the module's physical LED contact.
    if kind=='TRACKPOINT' and left:
        text = text.replace('sofle_trackpoint_dpi_get()', 'sofle_left_trackpoint_dpi_value()')
    write(name, '#include "modular.h"\n'+text)
