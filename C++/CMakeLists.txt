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
option(HELIOTISC4_BUILD_GRAPHICSENGINE_ADAPTER
    "Build the optional HeliotisC4-to-GraphicsEngine adapter target." OFF)
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

if(HELIOTISC4_BUILD_GRAPHICSENGINE_ADAPTER)
    if(NOT TARGET GraphicsEngine::Contracts)
        message(FATAL_ERROR
            "[HeliotisC4] HELIOTISC4_BUILD_GRAPHICSENGINE_ADAPTER=ON requires "
            "GraphicsEngine::Contracts. Add GraphicsEngine first or disable the adapter."
        )
    endif()

    add_library(HeliotisC4GraphicsEngineAdapter STATIC
        Utility/GraphicsEngine/HeliotisC4GraphicsFrameAdapter.h
        Utility/GraphicsEngine/HeliotisC4GraphicsFrameAdapter.cpp
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
            GraphicsEngine::Contracts
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

    if(HELIOTISC4_BUILD_GRAPHICSENGINE_ADAPTER)
        add_executable(heliotis_c4_graphics_adapter_test
            Tests/GraphicsFrameAdapterTest.cpp
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
