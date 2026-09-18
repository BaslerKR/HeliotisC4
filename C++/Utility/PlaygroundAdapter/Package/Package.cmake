# Device-owned Playground plugin identity.
# Allowed commands are quoted `set(PLAYGROUND_PLUGIN_* ...)` assignments.
# Host schema, ABI, and icon names come from DevicePluginPackage.h.
# Repeat NAME=value to declare multiple environment paths for one variable.
set(PLAYGROUND_PLUGIN_ID "heliotis-c4")
set(PLAYGROUND_PLUGIN_VERSION "0.1.4")
set(PLAYGROUND_PLUGIN_DISPLAY_NAME "Heliotis C4")
set(PLAYGROUND_PLUGIN_ADD_ACTION_TEXT "Heliotis C4")
set(PLAYGROUND_PLUGIN_SESSION_TYPE "Heliotis C4")
set(PLAYGROUND_PLUGIN_MENU_ORDER 400)
set(PLAYGROUND_PLUGIN_LIBRARY_WINDOWS "HeliotisC4Plugin.dll")
set(PLAYGROUND_PLUGIN_ENVIRONMENT_PATHS
    "PLAYGROUND_HELIOTISC4_RUNTIME_ROOT=runtime")
