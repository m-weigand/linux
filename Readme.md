# Description of new branches and merge order

## Branches

* mw/rk35/pinenote-next-t1 -- Contains all changed merged into one branch that
  can be used on the Pinenote.
* mw/rk35/ebc-drm-v5-modifications-t1 -- Changes to the rockchip_ebc driver
  controlling the Pinenote epd display
* mw/rk35/pinenote-defconfig-changes -- Changes to the Pinenote defconfig tree
* mw/rk35/pinenote_dts_changes -- Changes to the Pinenote device tree
* mw/rk35/rk356x-rga -- Changes to the RGA V4L2 driver for the rockchip-rga of
  the Pinenote. The RGA can be used to convert RGB images to Y4 grayscale for
  the EPD display. It also supports dithering to monochrome colors.
* master: this is the main branch forked from torvalds/linux

