add_library(canopennode STATIC
    "${PROJECT_SOURCE_DIR}/src/canopen/port/driver.c"
    "${PROJECT_SOURCE_DIR}/od/generated/OD.c")
foreach(source IN ITEMS CANopen.c 301/CO_ODinterface.c 301/CO_NMT_Heartbeat.c
        301/CO_HBconsumer.c 301/CO_Emergency.c 301/CO_SYNC.c 301/CO_PDO.c
        301/CO_SDOserver.c 301/CO_SDOclient.c 301/CO_fifo.c)
    target_sources(canopennode PRIVATE "${CANOPENNODE_SOURCE_DIR}/${source}")
endforeach()
target_compile_features(canopennode PRIVATE c_std_99)
target_include_directories(canopennode PUBLIC "${CANOPENNODE_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src/canopen/port" "${PROJECT_SOURCE_DIR}/od/generated")
