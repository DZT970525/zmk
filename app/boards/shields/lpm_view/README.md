# lpm_view

The lpm_view shield drives a JDI LPM009M360A 0.85" reflective memory LCD, a low-power, high
refresh rate display meant to replace I2C OLEDs traditionally used.

This shield requires that an `&lpm_view_spi` labeled SPI bus is provided with _at least_ MOSI,
SCK, and CS pins defined.

## Color mode

The panel is an 8 color display (black, red, green, blue, cyan, magenta, yellow and white): every
dot is made of a red, a green and a blue sub pixel, each of them storing one bit. This shield uses
that 3 bit data mode (`color_mode = [00]` in `lpm_view.overlay`) together with a 16 bit
(RGB565) LVGL color depth, the driver reduces the colors of the rendered image to the 8 colors the
panel is able to show.

The status screen is drawn light on dark, which is the appearance the previous monochrome
configuration had: it used `reverse = <1>;` to invert the image, with color support this is
achieved with `CONFIG_NICE_VIEW_WIDGET_INVERTED=y` so that the accent colors (for example the green
battery icon) keep their hue.

### Switching back to monochrome

To get the old black and white behavior back, set the data mode to 1 bit data mode and the color
depth to 1 bit in `lpm_view.overlay` and `Kconfig.defconfig`:

```
color_mode = [02];
reverse = <1>;
```

```
config LV_Z_BITS_PER_PIXEL
    default 1

choice LV_COLOR_DEPTH
    default LV_COLOR_DEPTH_1
endchoice
```

## Disable custom widget

The nice!view shield includes a custom vertical widget. To use the built-in ZMK one, add the following item to your `.conf` file:

```
CONFIG_ZMK_DISPLAY_STATUS_SCREEN_BUILT_IN=y
CONFIG_ZMK_LV_FONT_DEFAULT_SMALL_MONTSERRAT_26=y
CONFIG_LV_FONT_DEFAULT_MONTSERRAT_26=y
```
