How to compile the firmware for the trackpoint on the right side of the keyboard using shield

west build -p -b zitaotech_keypoint_right -- -DSHIELD="lpm_view;right_trackpoint_keypoint"
