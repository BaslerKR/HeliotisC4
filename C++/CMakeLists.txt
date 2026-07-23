add_library(HeliotisC4 INTERFACE)
add_library(HeliotisC4::HeliotisC4 ALIAS HeliotisC4)

target_compile_features(HeliotisC4 INTERFACE cxx_std_17)

set(HELIOTISC4_C4UTILITY_ROOT "C:/Program Files/C4Utility" CACHE PATH
    "C4Utility installation root used by the future HeliotisC4 implementation")
option(HELIOTISC4_ENABLE_SDK "Resolve C4HdlC for implementation work" OFF)

# This skeleton deliberately does not link or distribute the proprietary SDK.
# Enable discovery only when an implementation target consumes C4HdlC.
if(HELIOTISC4_ENABLE_SDK)
    if(NOT WIN32)
        message(FATAL_ERROR "[HeliotisC4] C4Utility SDK discovery is currently supported only on Windows.")
    endif()

    find_package(C4HdlC CONFIG REQUIRED
        PATHS "${HELIOTISC4_C4UTILITY_ROOT}/c4hdl/win64-x64/c"
        NO_DEFAULT_PATH
    )
    target_link_libraries(HeliotisC4 INTERFACE C4Hdl::C4HdlC)
endif()
