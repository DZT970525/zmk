/* Select the connector wiring before TWIM initializes; no active bus is remapped. */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
PINCTRL_DT_DEV_CONFIG_DECLARE(DT_NODELABEL(i2c0));
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), module_alt_default);
#ifndef PINCTRL_SKIP_SLEEP
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), module_alt_sleep);
#endif
static const struct pinctrl_state alternate[] = {
 PINCTRL_DT_STATE_INIT(module_alt_default, PINCTRL_STATE_DEFAULT),
#ifndef PINCTRL_SKIP_SLEEP
 PINCTRL_DT_STATE_INIT(module_alt_sleep, PINCTRL_STATE_SLEEP),
#endif
};
static const struct device *const port0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
#if defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT)
#define ORIGINAL_SDA 26
#define ALTERNATE_SDA 8
#define EXPECTED_ADDRESS 0x15
#define CLOCK_PIN 4
static const struct device *const clock_port = DEVICE_DT_GET(DT_NODELABEL(gpio0));
#else
#define ORIGINAL_SDA 14
#define ALTERNATE_SDA 7
#define EXPECTED_ADDRESS 0x37
#define CLOCK_PIN 8
static const struct device *const clock_port = DEVICE_DT_GET(DT_NODELABEL(gpio1));
#endif
static void data(int high) { gpio_pin_set(port0, ALTERNATE_SDA, high); k_busy_wait(10); }
static bool clock_line(int high) {
 gpio_pin_set(clock_port, CLOCK_PIN, high);
 for (int i=0;i<100;i++) {
  if (!high || gpio_pin_get(clock_port,CLOCK_PIN)>0) { k_busy_wait(10); return true; }
  k_busy_wait(10);
 }
 return false;
}
static bool address_ack(unsigned address) {
 bool okay=true;
 data(1); okay=clock_line(1);
 if (gpio_pin_get(port0,ALTERNATE_SDA)<=0) okay=false;
 data(0); clock_line(0);
 unsigned value=address<<1;
 for(int bit=7;bit>=0;bit--) {
  data((value>>bit)&1); okay=clock_line(1)&&okay; clock_line(0);
 }
 data(1); okay=clock_line(1)&&okay;
 bool ack=gpio_pin_get(port0,ALTERNATE_SDA)==0;
 clock_line(0); data(0); clock_line(1); data(1);
 return okay&&ack;
}
static int select_bus(void) {
 if (!device_is_ready(port0)||!device_is_ready(clock_port)) return -ENODEV;
 /* Release the original SDA: it is the alternate module's interrupt output. */
 gpio_pin_configure(port0,ORIGINAL_SDA,GPIO_INPUT);
 gpio_pin_configure(port0,ALTERNATE_SDA,GPIO_OUTPUT_HIGH|GPIO_INPUT|GPIO_OPEN_DRAIN|GPIO_PULL_UP);
 gpio_pin_configure(clock_port,CLOCK_PIN,GPIO_OUTPUT_HIGH|GPIO_INPUT|GPIO_OPEN_DRAIN|GPIO_PULL_UP);
 bool found=false;
 for(int retry=0;retry<5&&!found;retry++) {
  k_busy_wait(10000);
  /* A permanently-low MOTION line must never be accepted as an I2C ACK. */
  if (!address_ack(0x7e)) {
   found=address_ack(EXPECTED_ADDRESS)&&address_ack(EXPECTED_ADDRESS);
#if !defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT)
   if(!found) found=address_ack(0x3b)&&address_ack(0x3b);
#endif
  }
 }
 gpio_pin_configure(port0,ALTERNATE_SDA,GPIO_INPUT);
 gpio_pin_configure(clock_port,CLOCK_PIN,GPIO_INPUT);
 if(found) return pinctrl_update_states(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(i2c0)),alternate,ARRAY_SIZE(alternate));
 return 0;
}
SYS_INIT(select_bus,POST_KERNEL,45);
