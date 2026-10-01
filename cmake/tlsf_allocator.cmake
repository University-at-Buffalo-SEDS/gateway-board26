option(TELEMETRY_USE_TLSF "Experiment: use TLSF for board-owned SEDSNet allocation hooks" OFF)
if(TELEMETRY_USE_TLSF)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../Core/Src/telemetry_tlsf.c
        ${CMAKE_CURRENT_LIST_DIR}/../third_party/tlsf/tlsf.c)
    # This is a measured packet-processing hotspot. Optimize the allocator for
    # speed while keeping the rest of the image size-optimized.
    set_source_files_properties(
        ${CMAKE_CURRENT_LIST_DIR}/../Core/Src/telemetry_tlsf.c
        ${CMAKE_CURRENT_LIST_DIR}/../third_party/tlsf/tlsf.c
        PROPERTIES COMPILE_OPTIONS "-O2")
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../third_party/tlsf)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE TELEMETRY_USE_TLSF=1)
endif()
