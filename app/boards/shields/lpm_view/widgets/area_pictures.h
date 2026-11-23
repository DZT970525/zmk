#pragma once
#include <lvgl.h>
#include <stdint.h>
void keypoint_area_picture_draw(lv_obj_t *canvas);
uint32_t keypoint_area_picture_revision(void);

#include <stddef.h>
int keypoint_area_picture_write(uint8_t slot,const uint8_t *data,size_t len);
