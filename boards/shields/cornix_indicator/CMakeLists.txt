# Attach to the app target (caksoylar style) so widget code shares ZMK's
# include search path — including app/include/drivers/ext_power.h.
# central.c and peripheral.c each guard themselves with #if so it's safe
# to list both unconditionally; the inactive one compiles to nothing.
target_sources(app PRIVATE
    src/widget.c
    src/central.c
    src/peripheral.c
    src/charging.c
)
zephyr_include_directories(include)
