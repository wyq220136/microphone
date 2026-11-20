#include <cstddef>
#include <cstdio>
#include <unistd.h>

#include "dl_model_base.hpp"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#if GTCRN_GRU_DIAGNOSTICS
#include "gru_diagnostic.hpp"
#endif

namespace {

#ifndef GTCRN_MODEL_SIZE_BYTES
#error "GTCRN_MODEL_SIZE_BYTES must be provided by main/CMakeLists.txt"
#endif

constexpr char kTag[] = "gtcrn_test";
constexpr char kModelPartition[] = "model";
constexpr char kRuntimeRevision[] = "uart-console-yield-v15";
constexpr std::size_t kExpectedModelBytes = GTCRN_MODEL_SIZE_BYTES;

void initialize_console_logging()
{
#if CONFIG_ESP_CONSOLE_UART
    // The default UART VFS busy-waits on TX FIFO space. The long operator
    // profile can then starve IDLE0 for more than the task watchdog timeout.
    // Switch before model work to interrupt-driven, task-blocking writes.
    // Keep the boot console's existing pins, baud rate and framing.
    fflush(stdout);
    fflush(stderr);
    fsync(fileno(stdout));
    constexpr auto port = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
    if (!uart_is_driver_installed(port)) {
        ESP_ERROR_CHECK(uart_driver_install(port, 256, 1024, 0, nullptr, 0));
    }
    uart_vfs_dev_use_driver(port);
#endif
}

void log_heap(const char *stage)
{
    ESP_LOGI(kTag,
             "%s: internal_free=%u, internal_largest=%u, psram_free=%u, psram_largest=%u",
             stage,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

} // namespace

extern "C" void app_main(void)
{
    initialize_console_logging();
    ESP_LOGI(kTag, "GTCRN ESP-DL model validation starting");
    ESP_LOGI(kTag, "runtime revision: %s", kRuntimeRevision);
#if CONFIG_ESP_CONSOLE_UART
    ESP_LOGI(kTag, "Console UART%d: interrupt-driven TX enabled", CONFIG_ESP_CONSOLE_UART_NUM);
#endif
#if GTCRN_GRU_DIAGNOSTICS
    ESP_LOGI(kTag, "GRU FP32 LUT SHA256: %s", dl::math::gru_lut_fp32::sha256);
#endif
    log_heap("before_model_load");

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        kModelPartition);
    if (partition == nullptr) {
        ESP_LOGE(kTag, "Partition '%s' was not found", kModelPartition);
        return;
    }

    ESP_LOGI(kTag,
             "model partition: address=0x%08x size=%u bytes; build model=%u bytes",
             static_cast<unsigned>(partition->address),
             static_cast<unsigned>(partition->size),
             static_cast<unsigned>(kExpectedModelBytes));
    if (partition->size < kExpectedModelBytes) {
        ESP_LOGE(kTag,
                 "Model partition is too small: need at least %u bytes",
                 static_cast<unsigned>(kExpectedModelBytes));
        return;
    }

    // Keep model parameters in flash to minimize PSRAM use on the N8R2 module.
    // max_internal_size=0 lets ESP-DL place large intermediate buffers in PSRAM.
    auto *model = new dl::Model(kModelPartition,
                                fbs::MODEL_LOCATION_IN_FLASH_PARTITION,
                                0,
                                dl::MEMORY_MANAGER_GREEDY,
                                nullptr,
                                false);

    if (!model->is_built()) {
        ESP_LOGE(kTag, "Model load/build failed; inspect the ESP-DL errors above");
        delete model;
        return;
    }

    log_heap("after_model_load");
    model->profile_memory();

    ESP_LOGI(kTag, "Running embedded model test vectors");
#if GTCRN_GRU_DIAGNOSTICS
    constexpr char diagnostic_node[] = "/dpgrnn1/inter_rnn/rnn1/GRU";
    diagnose_isolated_gru(model, diagnostic_node);
    const esp_err_t test_result = model->test(diagnostic_node);
#else
    const esp_err_t test_result = model->test();
#endif
    if (test_result != ESP_OK) {
        ESP_LOGE(kTag, "GTCRN model test failed: %s", esp_err_to_name(test_result));
        delete model;
        return;
    }

    ESP_LOGI(kTag, "GTCRN model test passed; profiling operators by latency");
    model->profile_module(true);
    log_heap("after_model_profile");

    delete model;
    ESP_LOGI(kTag, "GTCRN validation completed successfully");
}
