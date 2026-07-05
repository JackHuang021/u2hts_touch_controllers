add_library(u2hts_touch_controllers STATIC
    ${CMAKE_CURRENT_LIST_DIR}/gt9xx.c
    ${CMAKE_CURRENT_LIST_DIR}/ft54x6.c
    ${CMAKE_CURRENT_LIST_DIR}/rmi_common.c
    ${CMAKE_CURRENT_LIST_DIR}/rmi_f11.c
    ${CMAKE_CURRENT_LIST_DIR}/cst8xx.c
    ${CMAKE_CURRENT_LIST_DIR}/chsc6540.c
    ${CMAKE_CURRENT_LIST_DIR}/goodix_normandy.c
)

# Local headers (rmi_common.h, s6d6ft0_v1.10_20170918.h etc.)
target_include_directories(u2hts_touch_controllers PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(u2hts_touch_controllers PUBLIC
    u2hts_core
)
