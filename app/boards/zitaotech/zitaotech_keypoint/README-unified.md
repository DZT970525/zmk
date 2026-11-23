# KEYPOINT unified firmware V1.09

56-key KEYPOINT hardware/layout is retained; there is no extra receiver key0. The same left/right firmware runs standalone or with Macintosch.

- Left/right default Bootmagic48/49, space keys47/50, layer1 B31, layer1 C29.
- Macintosch discovery: left shows Press space to connect; either physical space approves. Encrypted READY completes pairing. Bonded reconnects and switching between remembered keyboards follow the unified SOFLE flow.
- Optional layer1 B route switching uses the saved permission and consumes press/release independently of saved bindings.
- KEYPOINT split service uses a separate family UUID from SOFLE; update both KEYPOINT halves together. Macintosch HID relay service stays shared.
- Left device name KEYPOINT; no distinct _M firmware. Preserves the ordinary56-key keymap and zitaotech_bootmagic settings namespaces.
- Mac fine-motion fractional TrackPoint transport, velocity curve and four BLE pointing profiles remain. USB shares current BLE slot. Left10/right20 images, image transfer lock, Memory LCD bootloader image retention, Studio USB reconnect and +4dBm are retained from current KEYPOINT.
- Host Just Works re-pair and replacement of a disconnected selected BLE profile are enabled. Other host and receiver bonds are preserved.
- New KEYPOINT adds an optional product byte to approval response. Macintosch accepts this plus legacy6-byte SOFLE approval; title stays empty until READY and is cleared on disconnect.

Build recipe: D:/CH582/matrix-tester-disabled/build-keypoint-unified.py left|right|receiver under the existing WSL toolchain. KEYPOINT boards use lpm_view + left_bbtrackpad_keypoint/right_trackpoint_keypoint, original pin definitions and original keymap. Additional configs live here in configs/unified-left.conf and unified-right.conf.

No reset is requested from ordinary56-key KEYPOINT. Back up/check the keymap when migrating from the legacy57-key KEYPOINT_M. Allthree new images require hardware validation; existing unified SOFLE does not need reflashing for the new receiver.
