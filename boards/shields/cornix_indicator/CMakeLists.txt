# Attach to the app target (caksoylar style) so the widget shares ZMK's
# include search path — including app/include/drivers/ext_power.h.
target_sources(app PRIVATE src/widget.c)
