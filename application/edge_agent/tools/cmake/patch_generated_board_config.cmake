function(edge_agent_remove_generated_field_if_header_lacks HEADER_PATH FIELD_NAME SOURCE_PATH PATCH_NAME)
    if(NOT EXISTS "${HEADER_PATH}" OR NOT EXISTS "${SOURCE_PATH}")
        return()
    endif()

    file(READ "${HEADER_PATH}" EDGE_AGENT_HEADER_CONTENT)
    string(FIND "${EDGE_AGENT_HEADER_CONTENT}" "${FIELD_NAME}" EDGE_AGENT_FIELD_OFFSET)
    if(NOT EDGE_AGENT_FIELD_OFFSET EQUAL -1)
        return()
    endif()

    file(READ "${SOURCE_PATH}" EDGE_AGENT_SOURCE_CONTENT)
    string(REGEX REPLACE "[ \t]*\\.${FIELD_NAME}[ \t]*=[^\n]*,\n" "" EDGE_AGENT_PATCHED_SOURCE_CONTENT "${EDGE_AGENT_SOURCE_CONTENT}")
    if(EDGE_AGENT_PATCHED_SOURCE_CONTENT STREQUAL EDGE_AGENT_SOURCE_CONTENT)
        return()
    endif()

    file(WRITE "${SOURCE_PATH}" "${EDGE_AGENT_PATCHED_SOURCE_CONTENT}")
    message(STATUS "[edge_agent] Applied generated board config patch '${PATCH_NAME}'")
endfunction()

if(NOT DEFINED EDGE_AGENT_IDF_PATH OR EDGE_AGENT_IDF_PATH STREQUAL "")
    message(FATAL_ERROR "[edge_agent] EDGE_AGENT_IDF_PATH is not set")
endif()

if(NOT DEFINED EDGE_AGENT_BOARD_PERIPH_CONFIG)
    message(FATAL_ERROR "[edge_agent] EDGE_AGENT_BOARD_PERIPH_CONFIG is not set")
endif()

edge_agent_remove_generated_field_if_header_lacks(
    "${EDGE_AGENT_IDF_PATH}/components/esp_driver_i2s/include/driver/i2s_std.h"
    "bclk_div"
    "${EDGE_AGENT_BOARD_PERIPH_CONFIG}"
    "i2s_std_clk_bclk_div"
)

edge_agent_remove_generated_field_if_header_lacks(
    "${EDGE_AGENT_IDF_PATH}/components/esp_driver_rmt/include/driver/rmt_tx.h"
    "init_level"
    "${EDGE_AGENT_BOARD_PERIPH_CONFIG}"
    "rmt_tx_flags_init_level"
)
