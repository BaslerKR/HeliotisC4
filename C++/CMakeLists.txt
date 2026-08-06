add_library(heliotis_c4_core STATIC
    HeliotisC4.cpp
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
    "C4Utility installation root used by the future HeliotisC4 implementation")
option(HELIOTISC4_ENABLE_SDK "Resolve C4HdlC for implementation work" OFF)
option(HELIOTISC4_BUILD_QT_UI "Build the optional Qt HeliotisC4 feature-tree widget" ON)
option(HELIOTISC4_BUILD_GRAPHICSENGINE_ADAPTER
    "Build the optional HeliotisC4-to-GraphicsEngine adapter target." OFF)
option(HELIOTISC4_BUILD_TESTS "Build the standalone HeliotisC4 module tests" OFF)

if(HELIOTISC4_BUILD_QT_UI)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(Qt6_FOUND)
        set_target_properties(heliotis_c4_core PROPERTIES AUTOMOC ON)
        target_sources(heliotis_c4_core PRIVATE
            Utility/Qt/QHeliotisC4Widget.cpp
            Utility/Qt/QHeliotisC4Widget.h
        )
        target_link_libraries(heliotis_c4_core PUBLIC Qt6::Widgets)
        target_compile_definitions(heliotis_c4_core PUBLIC HELIOTISC4_HAS_QT_UI)
    endif()
endif()

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
    set_property(TARGET C4Hdl::C4HdlC PROPERTY IMPORTED_GLOBAL TRUE)
    target_sources(heliotis_c4_core PRIVATE
        HeliotisC4System.cpp
        HeliotisC4System.h
    )
    target_compile_definitions(heliotis_c4_core PUBLIC HELIOTISC4_HAS_SDK)
    target_link_libraries(heliotis_c4_core PUBLIC C4Hdl::C4HdlC)
endif()

if(HELIOTISC4_BUILD_GRAPHICSENGINE_ADAPTER)
    if(NOT TARGET GraphicsEngine::GraphicsEngine)
        message(FATAL_ERROR
            "[HeliotisC4] HELIOTISC4_BUILD_GRAPHICSENGINE_ADAPTER=ON requires "
            "GraphicsEngine::GraphicsEngine. Add GraphicsEngine first or disable the adapter."
        )
    endif()

    add_library(HeliotisC4GraphicsEngineAdapter STATIC
        Utility/GraphicsEngine/HeliotisC4GraphicsSceneAdapter.h
        Utility/GraphicsEngine/HeliotisC4GraphicsSceneAdapter.cpp
    )
    add_library(HeliotisC4::GraphicsEngineAdapter ALIAS HeliotisC4GraphicsEngineAdapter)
    target_compile_features(HeliotisC4GraphicsEngineAdapter PUBLIC cxx_std_17)
    target_include_directories(HeliotisC4GraphicsEngineAdapter
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/GraphicsEngine"
    )
    target_link_libraries(HeliotisC4GraphicsEngineAdapter
        PUBLIC
            HeliotisC4::Core
            GraphicsEngine::GraphicsEngine
    )
    message(STATUS "[HeliotisC4] HeliotisC4::GraphicsEngineAdapter enabled.")
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

    add_executable(heliotis_c4_frame_contract_test
        Tests/FrameContractTest.cpp
    )
    target_link_libraries(heliotis_c4_frame_contract_test PRIVATE HeliotisC4::Core)
    add_test(
        NAME heliotis_c4_frame_contract
        COMMAND heliotis_c4_frame_contract_test
    )

    if(HELIOTISC4_BUILD_GRAPHICSENGINE_ADAPTER)
        add_executable(heliotis_c4_graphics_adapter_test
            Tests/GraphicsSceneAdapterTest.cpp
        )
        target_link_libraries(heliotis_c4_graphics_adapter_test
            PRIVATE
                HeliotisC4::GraphicsEngineAdapter
        )
        add_test(
            NAME heliotis_c4_graphics_adapter
            COMMAND heliotis_c4_graphics_adapter_test
        )
    endif()

    if(HELIOTISC4_BUILD_QT_UI AND TARGET Qt6::Widgets)
        add_executable(heliotis_c4_widget_test
            Tests/HeliotisC4WidgetTest.cpp
        )
        target_link_libraries(heliotis_c4_widget_test
            PRIVATE
                HeliotisC4::Core
                Qt6::Widgets
        )
        add_test(
            NAME heliotis_c4_widget
            COMMAND heliotis_c4_widget_test
        )
        set_tests_properties(heliotis_c4_widget PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endif()
endif()
