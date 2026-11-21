foreach(mode --surface --surface-check)
    execute_process(
        COMMAND "${PROBE}" "${mode}" -platform offscreen
        RESULT_VARIABLE result
        OUTPUT_VARIABLE report
        ERROR_VARIABLE diagnostic)
    if(NOT result STREQUAL "77")
        message(FATAL_ERROR "Expected offscreen surface fallback, got ${result}\n${report}${diagnostic}")
    endif()

    string(JSON status GET "${report}" status)
    string(JSON reason GET "${report}" reason)
    string(JSON verified GET "${report}" physical_hdr_verified)
    if(NOT status STREQUAL "sdr_fallback" OR NOT reason STREQUAL "not_wayland" OR verified)
        message(FATAL_ERROR "Unexpected offscreen surface report\n${report}")
    endif()
endforeach()
