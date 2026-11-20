set(model_file "${PROJECT_DIR}/models/gtcrn_stream.espdl")
set(manifest_file "${PROJECT_DIR}/models/gtcrn_stream.json")
option(SINGLEMIC_GTCRN_SELF_TEST "Check continuous golden model vectors on startup" OFF)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${model_file}" "${manifest_file}" "${PROJECT_DIR}/tools/model_contract.py"
    "${PROJECT_DIR}/tools/golden_contract.py" "${PROJECT_DIR}/models/golden_stream.json")
execute_process(COMMAND "${PYTHON}" "${PROJECT_DIR}/tools/model_contract.py"
    "${model_file}" "${manifest_file}" "${CMAKE_CURRENT_BINARY_DIR}/model_contract.h"
    RESULT_VARIABLE contract_result)
if(NOT contract_result EQUAL 0)
    message(FATAL_ERROR "Export a matching streaming model and manifest first; see INTEGRATION.md")
endif()
if(SINGLEMIC_GTCRN_SELF_TEST)
    execute_process(COMMAND "${PYTHON}" "${PROJECT_DIR}/tools/golden_contract.py"
        "${PROJECT_DIR}/models/golden_stream.json" "${manifest_file}"
        "${CMAKE_CURRENT_BINARY_DIR}/golden_contract.h" RESULT_VARIABLE golden_result)
    if(NOT golden_result EQUAL 0)
        message(FATAL_ERROR "Golden vectors do not match the model")
    endif()
endif()
idf_component_register(
    SRCS entry.cpp ../app_controller.cpp ../audio_processor.cpp ../audio_pipeline.cpp
         ../gtcrn_engine.cpp ../spectral_dsp.cpp ../I2SSampler.cpp ../button.cpp
         ../packet_protocol.cpp ../spp_transport.cpp ../reset_progress.cpp
    INCLUDE_DIRS .. "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES "$ENV{SINGLEMIC_ARDUINO_COMPONENT_NAME}" esp-dl
    PRIV_REQUIRES esp_driver_i2s esp_driver_uart esp_timer esp_partition app_update bt mbedtls
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SINGLEMIC_ENABLE_GTCRN=1)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SINGLEMIC_GTCRN_SELF_TEST=$<BOOL:${SINGLEMIC_GTCRN_SELF_TEST}>)
esptool_py_flash_to_partition(flash "model" "${model_file}")
