# Device-owned Playground plugin identity.
# Allowed commands are quoted `set(PLAYGROUND_PLUGIN_* ...)` assignments.
# Host schema, ABI, and icon names come from DeviceRuntime.h.
# Repeat NAME=value to declare multiple environment paths for one variable.
set(PLAYGROUND_PLUGIN_ID "heliotis-c4")
set(PLAYGROUND_PLUGIN_VERSION "0.4.0")
# Host-provided frame storage (IFrameContextClient) and the image writer first ship in Core 0.7.0.
set(PLAYGROUND_PLUGIN_MINIMUM_CORE_VERSION "0.7.0")
set(PLAYGROUND_PLUGIN_DISPLAY_NAME "Heliotis C4")
# Optional presentation metadata; consumers must export it to manifests/catalogs.
set(PLAYGROUND_PLUGIN_SUBTITLE "C4Utility")
set(PLAYGROUND_PLUGIN_DESCRIPTION "Configure heliInspect H8 devices and acquire multipart measurement data through C4Utility.")
set(PLAYGROUND_PLUGIN_ADD_ACTION_TEXT "Heliotis C4")
set(PLAYGROUND_PLUGIN_SESSION_TYPE "Heliotis C4")
set(PLAYGROUND_PLUGIN_MENU_ORDER 400)
set(PLAYGROUND_PLUGIN_INSTANCING "package")
set(PLAYGROUND_PLUGIN_SESSION_SERVICES "graphicsEngine" "scriptEditor" "threeD")
set(PLAYGROUND_PLUGIN_MODULE_WINDOWS "HeliotisC4Plugin.dll")
set(PLAYGROUND_PLUGIN_ENVIRONMENT_PATHS
    "PLAYGROUND_HELIOTISC4_RUNTIME_ROOT=runtime")

set(PLAYGROUND_PLUGIN_PANEL_REDISCOVER_FIRST ON)
