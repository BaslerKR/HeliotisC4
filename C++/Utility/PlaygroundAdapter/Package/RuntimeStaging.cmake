# Heliotis C4-owned Playground plugin runtime payload. The host copies
# PLAYGROUND_PLUGIN_RUNTIME_PAYLOAD_DIR into plugins/heliotis-c4/current/runtime.

function(heliotis_c4_prepare_playground_plugin_runtime target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR
            "[HeliotisC4] ${target_name} must exist before its plugin runtime is prepared.")
    endif()
    if(NOT WIN32)
        return()
    endif()
    if(NOT HELIOTISC4_C4UTILITY_ROOT OR NOT IS_DIRECTORY "${HELIOTISC4_C4UTILITY_ROOT}")
        message(FATAL_ERROR
            "[HeliotisC4] HELIOTISC4_C4UTILITY_ROOT must point at a C4Utility installation.")
    endif()

    file(TO_CMAKE_PATH "${HELIOTISC4_C4UTILITY_ROOT}" heliotis_c4_runtime_source_root)
    set(payload_dir "${CMAKE_CURRENT_BINARY_DIR}/plugin-runtime")
    file(REMOVE_RECURSE "${payload_dir}")
    file(MAKE_DIRECTORY "${payload_dir}/c4hdl/win64-x64/c/bin")
    file(MAKE_DIRECTORY "${payload_dir}/c4hdl/win64-x64/genicam/bin")
    file(MAKE_DIRECTORY "${payload_dir}/diaphus/win64-x64")

    set(heliotis_c4_hdl_dll
        "${heliotis_c4_runtime_source_root}/c4hdl/win64-x64/c/bin/C4HdlC.dll")
    if(NOT EXISTS "${heliotis_c4_hdl_dll}")
        message(FATAL_ERROR
            "[HeliotisC4] Required C4HdlC.dll is missing: ${heliotis_c4_hdl_dll}")
    endif()
    file(COPY "${heliotis_c4_hdl_dll}"
        DESTINATION "${payload_dir}/c4hdl/win64-x64/c/bin")

    foreach(genicam_dll
            GenApi_MD_VC141_v3_2.dll
            GCBase_MD_VC141_v3_2.dll
            MathParser_MD_VC141_v3_2.dll
            XmlParser_MD_VC141_v3_2.dll
            Log_MD_VC141_v3_2.dll
            log4cpp_MD_VC141_v3_2.dll
            NodeMapData_MD_VC141_v3_2.dll)
        set(genicam_dll_path
            "${heliotis_c4_runtime_source_root}/c4hdl/win64-x64/genicam/bin/${genicam_dll}")
        if(NOT EXISTS "${genicam_dll_path}")
            message(FATAL_ERROR
                "[HeliotisC4] Required GenICam runtime is missing: ${genicam_dll_path}")
        endif()
        file(COPY "${genicam_dll_path}"
            DESTINATION "${payload_dir}/c4hdl/win64-x64/genicam/bin")
    endforeach()

    set(diaphus_cti
        "${heliotis_c4_runtime_source_root}/diaphus/win64-x64/diaphus.cti")
    if(NOT EXISTS "${diaphus_cti}")
        message(FATAL_ERROR
            "[HeliotisC4] Required Diaphus producer is missing: ${diaphus_cti}")
    endif()
    file(COPY "${diaphus_cti}" DESTINATION "${payload_dir}/diaphus/win64-x64")

    set_target_properties(${target_name} PROPERTIES
        PLAYGROUND_PLUGIN_RUNTIME_PAYLOAD_DIR "${payload_dir}"
        PLAYGROUND_PLUGIN_RUNTIME_DEPENDENCY_DEST "runtime"
        PLAYGROUND_PLUGIN_RUNTIME_SEARCH_PATHS "${heliotis_c4_runtime_source_root}"
    )
endfunction()
