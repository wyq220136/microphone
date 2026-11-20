idf_component_register(
    SRCS "main.cpp"
    REQUIRES esp-dl
    PRIV_REQUIRES esp_driver_uart
)

option(GTCRN_GRU_DIAGNOSTICS "Run an isolated GRU and collect exact input differences" OFF)
if(GTCRN_GRU_DIAGNOSTICS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GTCRN_GRU_DIAGNOSTICS=1)
endif()

set(model_file "${PROJECT_DIR}/models/gtcrn.espdl")

if(NOT EXISTS "${model_file}")
    message(FATAL_ERROR "GTCRN model not found: ${model_file}")
endif()

# Reconfigure after replacing the model so the compiled size check stays current.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${model_file}")
file(SIZE "${model_file}" model_size_bytes)
target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE GTCRN_MODEL_SIZE_BYTES=${model_size_bytes}
)

# Add the model image to `idf.py flash`. The partition label must match
# partitions.csv and the label passed to dl::Model in main.cpp.
esptool_py_flash_to_partition(flash "model" "${model_file}")
