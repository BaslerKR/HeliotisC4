add_library(heliotis_c4_core STATIC
    HeliotisC4.cpp
    Internal/HeliotisAcquisitionPolicy.cpp
    Internal/HeliotisAcquisitionPolicy.h
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
option(HELIOTISC4_BUILD_QT_UI "Build the optional Qt HeliotisC4 feature-tree widget" OFF)
option(HELIOTISC4_BUILD_PLAYGROUND_ADAPTER
    "Build the optional HeliotisC4 Playground adapter target." OFF)
option(HELIOTISC4_BUILD_PLAYGROUND_PLUGIN
    "Build the HeliotisC4 Playground device plugin MODULE." OFF)
option(HELIOTISC4_BUILD_TESTS "Build the standalone HeliotisC4 module tests" OFF)

if(HELIOTISC4_BUILD_QT_UI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)

    add_library(HeliotisC4QtWidget STATIC
        Utility/Qt/QHeliotisC4Widget.cpp
        Utility/Qt/QHeliotisC4Widget.h
    )
    add_library(HeliotisC4::QtWidget ALIAS HeliotisC4QtWidget)
    set_target_properties(HeliotisC4QtWidget PROPERTIES AUTOMOC ON)
    target_compile_features(HeliotisC4QtWidget PUBLIC cxx_std_17)
    target_compile_definitions(HeliotisC4QtWidget PUBLIC HELIOTISC4_HAS_QT_UI)
    target_link_libraries(HeliotisC4QtWidget
        PUBLIC
            HeliotisC4::Core
            Qt6::Widgets
    )
    message(STATUS "[HeliotisC4] HeliotisC4::QtWidget enabled.")
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
    if(NOT HELIOTISC4_BUILD_QT_UI)
        message(FATAL_ERROR
            "[HeliotisC4] HELIOTISC4_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "HELIOTISC4_BUILD_QT_UI."
        )
    endif()
    if(NOT HELIOTISC4_ENABLE_SDK)
        message(FATAL_ERROR
            "[HeliotisC4] HELIOTISC4_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "HELIOTISC4_ENABLE_SDK."
        )
    endif()
    if(NOT TARGET Playground::DevicePlugin)
        message(FATAL_ERROR
            "[HeliotisC4] HELIOTISC4_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "Playground::DevicePlugin."
        )
    endif()
    if(NOT TARGET Qt6::Widgets)
        message(FATAL_ERROR
            "[HeliotisC4] HELIOTISC4_BUILD_PLAYGROUND_PLUGIN=ON requires Qt 6."
        )
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Concurrent)

    file(TO_CMAKE_PATH "${HELIOTISC4_C4UTILITY_ROOT}" heliotis_c4_plugin_sdk_root)
    add_library(HeliotisC4Plugin MODULE
        Utility/PlaygroundAdapter/Plugin/HeliotisC4Plugin.cpp
        Utility/PlaygroundAdapter/Source/HeliotisC4SourceController.h
        Utility/PlaygroundAdapter/Source/HeliotisC4SourceController.cpp
    )
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
        Playground::DevicePlugin
        HeliotisC4::QtWidget
        HeliotisC4::PlaygroundAdapter
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        Qt6::Concurrent
    )
    if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_link_libraries(HeliotisC4Plugin PRIVATE delayimp)
        target_link_options(HeliotisC4Plugin PRIVATE "/DELAYLOAD:C4HdlC.dll")
    endif()
    include("${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/RuntimeStaging.cmake")
    heliotis_c4_prepare_playground_plugin_runtime(HeliotisC4Plugin)
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

    if(HELIOTISC4_BUILD_QT_UI)
        add_executable(heliotis_c4_widget_test
            Tests/HeliotisC4WidgetTest.cpp
        )
        target_link_libraries(heliotis_c4_widget_test
            PRIVATE
                HeliotisC4::QtWidget
        )
        if(WIN32)
            add_test(
                NAME heliotis_c4_widget
                COMMAND ${CMAKE_COMMAND} -E env
                    "QT_QPA_PLATFORM=offscreen"
                    "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}"
                    $<TARGET_FILE:heliotis_c4_widget_test>
            )
        else()
            add_test(
                NAME heliotis_c4_widget
                COMMAND heliotis_c4_widget_test
            )
            set_tests_properties(heliotis_c4_widget PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            )
        endif()
    endif()
endif()
