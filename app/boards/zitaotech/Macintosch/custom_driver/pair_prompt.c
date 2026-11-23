/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
static const char chars[]="ab cdeknoprstuy";
static const unsigned char glyphs[][5]={
 {0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},{0,0,0,0,0},
 {0x38,0x44,0x44,0x44,0x28},{0x38,0x44,0x44,0x48,0x7f},
 {0x38,0x54,0x54,0x54,0x18},{0x7f,0x10,0x28,0x44,0},
 {0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
 {0x7c,0x14,0x14,0x14,0x08},{0x7c,0x08,0x04,0x04,0x08},
 {0x48,0x54,0x54,0x54,0x24},{0x04,0x3f,0x44,0x40,0x20},
 {0x3c,0x40,0x40,0x20,0x7c},{0x0c,0x50,0x50,0x50,0x3c}};
static int lit(char c,int x,int y) {
 const char *p=strchr(chars,c);return p && x<5 && (glyphs[p-chars][x] & (1u<<y));
}
void mac_test_draw_receiver_prompt(void) {
 const struct device *dev=DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
 if (!device_is_ready(dev)) return;
 const char *lines[]={"press space key","on your keyboard","to connect"};
 uint16_t row[240];
 struct display_buffer_descriptor desc={.buf_size=sizeof(row),.width=240,.height=1,.pitch=240};
 for(int y=0;y<240;y++) {
  memset(row,0,sizeof(row));
  for(int line=0;line<3;line++) {
   int py=y-(88+line*24);if(py<0 || py>=14)continue;
   int length=strlen(lines[line]),left=(240-length*12)/2;
   for(int x=left;x<left+length*12;x++) {
    int px=x-left;if(lit(lines[line][px/12],(px%12)/2,py/2))row[x]=0xffff;
   }
  }
  if(display_write(dev,0,y,&desc,row)<0)break;
 }
 display_blanking_off(dev);
}
