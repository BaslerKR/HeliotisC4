add_library(heliotis_c4_core STATIC
    HeliotisC4.cpp
    Internal/HeliotisAcquisitionPolicy.cpp
    Internal/HeliotisAcquisitionPolicy.h
    Internal/HeliotisPartFit.h
    HeliotisFrameAnomalies.h
    Internal/C4UtilitySdkRuntime.cpp
    Internal/C4UtilitySdkRuntime.h
)
add_library(HeliotisC4::Core ALIAS heliotis_c4_core)

target_compile_features(heliotis_c4_core PUBLIC cxx_std_17)
target_include_directories(heliotis_c4_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

set(HELIOTISC4_C4UTILITY_ROOT "C:/Program Files/C4Utility" CACHE PATH
    "C4Utility installation root used by the SDK-enabled implementation")
option(HELIOTISC4_ENABLE_SDK "Resolve C4HdlC for implementation work" OFF)
option(HELIOTISC4_BUILD_PLAYGROUND_ADAPTER
    "Build the optional HeliotisC4 Playground adapter target." OFF)
option(HELIOTISC4_BUILD_PLAYGROUND_PLUGIN
    "Build the HeliotisC4 Playground device plugin MODULE." OFF)
option(HELIOTISC4_BUILD_TESTS "Build the standalone HeliotisC4 module tests" OFF)

if(HELIOTISC4_BUILD_PLAYGROUND_PLUGIN)
    set(HELIOTISC4_BUILD_PLAYGROUND_ADAPTER ON)
    set(HELIOTISC4_ENABLE_SDK ON)
endif()
if(PROJECT_NAME STREQUAL "HeliotisC4" AND PROJECT_IS_TOP_LEVEL AND
   (HELIOTISC4_BUILD_PLAYGROUND_ADAPTER OR HELIOTISC4_BUILD_PLAYGROUND_PLUGIN))
    find_package(PlaygroundPluginSDK 0.3 CONFIG REQUIRED)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# The module does not distribute the proprietary SDK. Enable SDK discovery only
# when the consumer provides a compatible C4Utility installation.
if(HELIOTISC4_ENABLE_SDK)
    if(NOT WIN32)
        message(FATAL_ERROR "[HeliotisC4] C4Utility SDK discovery is currently supported only on Windows.")
    endif()

    find_package(C4HdlC CONFIG REQUIRED
        PATHS "${HELIOTISC4_C4UTILITY_ROOT}/c4hdl/win64-x64/c"
        NO_DEFAULT_PATH
    )
    set_property(TARGET C4Hdl::C4HdlC PROPERTY IMPORTED_GLOBAL TRUE)
    target_sources(heliotis_c4_core PRIVATE
        HeliotisC4System.cpp
        HeliotisC4System.h
    )
    target_compile_definitions(heliotis_c4_core PUBLIC HELIOTISC4_HAS_SDK)
    target_link_libraries(heliotis_c4_core PUBLIC C4Hdl::C4HdlC)
endif()

if(HELIOTISC4_BUILD_PLAYGROUND_ADAPTER)
    if(NOT TARGET GraphicsEngine::Contracts)
        message(FATAL_ERROR
            "[HeliotisC4] HELIOTISC4_BUILD_PLAYGROUND_ADAPTER=ON requires "
            "GraphicsEngine::Contracts. Add GraphicsEngine first or disable the adapter."
        )
    endif()

    if(NOT TARGET Playground::DeviceRuntimeContract)
        message(FATAL_ERROR
            "[HeliotisC4] HELIOTISC4_BUILD_PLAYGROUND_ADAPTER=ON requires "
            "Playground::DeviceRuntimeContract for the frame storage plan."
        )
    endif()

    add_library(HeliotisC4PlaygroundAdapter STATIC
        Utility/PlaygroundAdapter/HeliotisC4GraphicsFrameAdapter.h
        Utility/PlaygroundAdapter/HeliotisC4GraphicsFrameAdapter.cpp
        Utility/PlaygroundAdapter/HeliotisGraphicsFrameStream.h
        Utility/PlaygroundAdapter/HeliotisGraphicsFrameStream.cpp
    )
    add_library(HeliotisC4::PlaygroundAdapter ALIAS HeliotisC4PlaygroundAdapter)
    target_compile_features(HeliotisC4PlaygroundAdapter PUBLIC cxx_std_17)
    target_include_directories(HeliotisC4PlaygroundAdapter
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter"
    )
    target_link_libraries(HeliotisC4PlaygroundAdapter
        PUBLIC
            HeliotisC4::Core
            GraphicsEngine::Contracts
            Playground::DeviceRuntimeContract
    )
    message(STATUS "[HeliotisC4] HeliotisC4::PlaygroundAdapter enabled.")
endif()

if(HELIOTISC4_BUILD_PLAYGROUND_PLUGIN)
    if(NOT HELIOTISC4_BUILD_PLAYGROUND_ADAPTER)
        message(FATAL_ERROR
            "[HeliotisC4] HELIOTISC4_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "HELIOTISC4_BUILD_PLAYGROUND_ADAPTER."
        )
    endif()
    if(NOT HELIOTISC4_ENABLE_SDK)
        message(FATAL_ERROR
            "[HeliotisC4] HELIOTISC4_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "HELIOTISC4_ENABLE_SDK."
        )
    endif()
    if(NOT TARGET Playground::DeviceRuntimeContract)
        message(FATAL_ERROR
            "[HeliotisC4] HELIOTISC4_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "Playground::DeviceRuntimeContract."
        )
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Core Concurrent)

    file(TO_CMAKE_PATH "${HELIOTISC4_C4UTILITY_ROOT}" heliotis_c4_plugin_sdk_root)
    add_library(HeliotisC4Plugin MODULE
        Utility/PlaygroundAdapter/Plugin/HeliotisC4Plugin.cpp
        Utility/PlaygroundAdapter/Plugin/HeliotisC4PluginSession.h
        Utility/PlaygroundAdapter/Plugin/HeliotisC4PluginSession.cpp
        Utility/PlaygroundAdapter/Source/HeliotisC4SourceController.h
        Utility/PlaygroundAdapter/Source/HeliotisC4SourceController.cpp
    )
    playground_sdk_add_runtime_contract(HeliotisC4Plugin)
    set_target_properties(HeliotisC4Plugin PROPERTIES
        PREFIX ""
        OUTPUT_NAME HeliotisC4Plugin
        AUTOMOC ON
        PLAYGROUND_PLUGIN_PACKAGE
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/Package.cmake"
    )
    target_compile_features(HeliotisC4Plugin PRIVATE cxx_std_17)
    target_include_directories(HeliotisC4Plugin PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source"
        "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Plugin"
    )
    target_compile_definitions(HeliotisC4Plugin PRIVATE
        HELIOTISC4_SDK_ROOT="${heliotis_c4_plugin_sdk_root}"
    )
    target_link_libraries(HeliotisC4Plugin PRIVATE
        Playground::DeviceRuntimeContract
        HeliotisC4::PlaygroundAdapter
        Qt6::Core
        Qt6::Concurrent
    )
    if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_link_libraries(HeliotisC4Plugin PRIVATE delayimp)
        target_link_options(HeliotisC4Plugin PRIVATE "/DELAYLOAD:C4HdlC.dll")
    endif()
    include("${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/RuntimeStaging.cmake")
    heliotis_c4_prepare_playground_plugin_runtime(HeliotisC4Plugin)
    if(PROJECT_NAME STREQUAL "HeliotisC4" AND PROJECT_IS_TOP_LEVEL)
        if(APPLE)
            set_target_properties(HeliotisC4Plugin PROPERTIES BUILD_WITH_INSTALL_RPATH ON
                INSTALL_RPATH "@loader_path/runtime;@executable_path/../Frameworks")
        elseif(UNIX)
            set_target_properties(HeliotisC4Plugin PROPERTIES BUILD_WITH_INSTALL_RPATH ON
                INSTALL_RPATH "$ORIGIN/runtime")
        endif()
        playground_sdk_package_plugin(HeliotisC4Plugin
            PACKAGE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/Package.cmake"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/icon.png")
    endif()
    message(STATUS "[HeliotisC4] HeliotisC4Plugin enabled.")
endif()

if(HELIOTISC4_BUILD_TESTS)
    add_executable(heliotis_c4_sdk_layout_test
        Tests/C4UtilitySdkRuntimeTest.cpp
    )
    target_link_libraries(heliotis_c4_sdk_layout_test PRIVATE HeliotisC4::Core)
    add_test(
        NAME heliotis_c4_sdk_layout
        COMMAND heliotis_c4_sdk_layout_test "${HELIOTISC4_C4UTILITY_ROOT}"
    )

    if(HELIOTISC4_ENABLE_SDK)
        add_executable(heliotis_c4_runtime_open_test
            Tests/C4UtilityRuntimeOpenTest.cpp
        )
        target_link_libraries(heliotis_c4_runtime_open_test PRIVATE HeliotisC4::Core)
        add_test(
            NAME heliotis_c4_runtime_open
            COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${HELIOTISC4_C4UTILITY_ROOT}/c4hdl/win64-x64/c/bin;${HELIOTISC4_C4UTILITY_ROOT}/c4hdl/win64-x64/genicam/bin;$ENV{PATH}"
                $<TARGET_FILE:heliotis_c4_runtime_open_test>
                "${HELIOTISC4_C4UTILITY_ROOT}"
        )
    endif()

    add_executable(heliotis_c4_frame_contract_test
        Tests/FrameContractTest.cpp
    )
    target_link_libraries(heliotis_c4_frame_contract_test PRIVATE HeliotisC4::Core)
    add_test(
        NAME heliotis_c4_frame_contract
        COMMAND heliotis_c4_frame_contract_test
    )

    add_executable(heliotis_c4_part_fit_test
        Tests/PartFitTest.cpp
    )
    target_link_libraries(heliotis_c4_part_fit_test PRIVATE HeliotisC4::Core)
    add_test(
        NAME heliotis_c4_part_fit
        COMMAND heliotis_c4_part_fit_test
    )

    add_executable(heliotis_c4_acquisition_policy_test
        Tests/HeliotisAcquisitionPolicyTest.cpp
    )
    target_link_libraries(heliotis_c4_acquisition_policy_test PRIVATE HeliotisC4::Core)
    add_test(
        NAME heliotis_c4_acquisition_policy
        COMMAND heliotis_c4_acquisition_policy_test
    )

    if(HELIOTISC4_BUILD_PLAYGROUND_ADAPTER)
        add_executable(heliotis_c4_graphics_adapter_test
            Utility/PlaygroundAdapter/Tests/GraphicsFrameAdapterTest.cpp
        )
        target_link_libraries(heliotis_c4_graphics_adapter_test
            PRIVATE
                HeliotisC4::PlaygroundAdapter
        )
        add_test(
            NAME heliotis_c4_graphics_adapter
            COMMAND heliotis_c4_graphics_adapter_test
        )
        add_executable(heliotis_c4_graphics_stream_test
            Utility/PlaygroundAdapter/Tests/GraphicsFrameStreamTest.cpp)
        target_link_libraries(heliotis_c4_graphics_stream_test PRIVATE HeliotisC4::PlaygroundAdapter)
        add_test(NAME heliotis_c4_graphics_stream COMMAND heliotis_c4_graphics_stream_test)
    endif()

    if(HELIOTISC4_BUILD_PLAYGROUND_PLUGIN)
        # The controller runs against a link-time device fake; no hardware is required.
        add_executable(heliotis_c4_source_control_test
            Utility/PlaygroundAdapter/Tests/SourceControlTest.cpp
            Utility/PlaygroundAdapter/Source/HeliotisC4SourceController.cpp
            Utility/PlaygroundAdapter/Source/HeliotisC4SourceController.h)
        playground_sdk_add_runtime_contract(heliotis_c4_source_control_test)
        set_target_properties(heliotis_c4_source_control_test PROPERTIES AUTOMOC ON)
        target_include_directories(heliotis_c4_source_control_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source")
        target_link_libraries(heliotis_c4_source_control_test PRIVATE
            Playground::DeviceRuntimeContract HeliotisC4::PlaygroundAdapter Qt6::Concurrent)
        if(WIN32)
            add_test(NAME heliotis_c4_source_control
                COMMAND ${CMAKE_COMMAND} -E env
                    "QT_QPA_PLATFORM=offscreen"
                    "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}"
                    $<TARGET_FILE:heliotis_c4_source_control_test>)
        else()
            add_test(NAME heliotis_c4_source_control COMMAND heliotis_c4_source_control_test)
            set_tests_properties(heliotis_c4_source_control PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endif()

        # The session drives discovery, connect, Single, stop and disconnect without a view,
        # as a device worker and an API-only session do; the link-time device fake needs no SDK.
        add_executable(heliotis_c4_plugin_session_test
            Utility/PlaygroundAdapter/Tests/PluginSessionHeadlessTest.cpp
            Utility/PlaygroundAdapter/Plugin/HeliotisC4PluginSession.cpp
            Utility/PlaygroundAdapter/Plugin/HeliotisC4PluginSession.h
            Utility/PlaygroundAdapter/Source/HeliotisC4SourceController.cpp
            Utility/PlaygroundAdapter/Source/HeliotisC4SourceController.h)
        playground_sdk_add_runtime_contract(heliotis_c4_plugin_session_test)
        set_target_properties(heliotis_c4_plugin_session_test PROPERTIES AUTOMOC ON)
        target_include_directories(heliotis_c4_plugin_session_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source"
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Plugin")
        target_link_libraries(heliotis_c4_plugin_session_test PRIVATE
            Playground::DeviceRuntimeContract HeliotisC4::PlaygroundAdapter
            Qt6::Concurrent)
        if(WIN32)
            add_test(NAME heliotis_c4_plugin_session
                COMMAND ${CMAKE_COMMAND} -E env
                    "QT_QPA_PLATFORM=offscreen"
                    "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}"
                    $<TARGET_FILE:heliotis_c4_plugin_session_test>)
        else()
            add_test(NAME heliotis_c4_plugin_session COMMAND heliotis_c4_plugin_session_test)
            set_tests_properties(heliotis_c4_plugin_session PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endif()
    endif()

endif()

# Keep contract assertions active in Release validation.
foreach(module_test IN ITEMS heliotis_c4_sdk_layout_test heliotis_c4_runtime_open_test heliotis_c4_frame_contract_test heliotis_c4_part_fit_test heliotis_c4_acquisition_policy_test heliotis_c4_graphics_adapter_test heliotis_c4_graphics_stream_test heliotis_c4_source_control_test heliotis_c4_plugin_session_test)
    if(TARGET ${module_test})
        target_compile_options(${module_test} PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
    endif()
endforeach()
