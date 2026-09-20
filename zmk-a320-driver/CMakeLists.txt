zephyr_library()

zephyr_library_sources_ifdef(CONFIG_A320 drivers/a320.c)
zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)

