#pragma once

#include "dl_module_gru.hpp"
#include <cstdio>
#include <cstring>
#include <memory>

inline void trace_isolated_gru(int direction, int time, int batch,
                               const int16_t *xw, const int16_t *hr,
                               const float *gates, const int16_t *state, int hidden_size)
{
    // The v11 residual starts at inter_rnn/rnn1 [6,0,21,2]: 6254 vs 6253.
    // Record all forward steps for batch 21, including the pre-divergence state.
    if (direction != 0 || batch != 21) return;
    const auto integers = [&](const char *stage, const int16_t *values, int count) {
        printf("GRU_TRACE %d %d %d %s", direction, time, batch, stage);
        for (int i = 0; i < count; ++i) printf(" %d", (int)values[i]);
        printf("\n");
    };
    integers("XW", xw, 3 * hidden_size); // Includes W bias after INT16 saturation.
    integers("HR", hr, 3 * hidden_size); // Includes R bias after INT16 saturation.
    printf("GRU_TRACE %d %d %d GATES", direction, time, batch);
    for (int i = 0; i < 3 * hidden_size; ++i) {
        uint32_t bits;
        static_assert(sizeof(bits) == sizeof(float), "Diagnostic requires FP32");
        memcpy(&bits, gates + i, sizeof(bits));
        printf(" %08lx", (unsigned long)bits); // Exact FP32 bits, no dtype conversion.
    }
    printf("\n");
    integers("H", state, hidden_size);
}

// Run the production GRU with serialized golden X, W, R and B in an independent
// context. This does not replace any tensor or hidden state in the full model.
inline void diagnose_isolated_gru(dl::Model *model, const char *node_name)
{
    constexpr const char *tag = "gru_isolated";
    auto *fbs = model->get_fbs_model();
    fbs->load_map();
    std::vector<std::string> inputs, outputs;
    fbs->get_operation_inputs_and_outputs(node_name, inputs, outputs);
    if (inputs.size() != 4 || outputs.size() != 2) {
        ESP_LOGE(tag, "Diagnostic requires X,W,R,B and Y,Y_h");
        fbs->clear_map();
        return;
    }
    {
        dl::ModelContext context;
        std::unique_ptr<dl::module::Module> op(dl::module::GRU::deserialize(fbs, node_name));
        std::unique_ptr<dl::TensorBase> gt(fbs->get_test_output_tensor(outputs[0]));
        if (!op || !gt || gt->get_dtype() != dl::DATA_TYPE_INT16) {
            ESP_LOGE(tag, "Missing INT16 test output or unsupported GRU");
            fbs->clear_map();
            return;
        }
        static_cast<dl::module::GRU *>(op.get())->m_trace_callback = trace_isolated_gru;
        for (size_t i = 0; i < inputs.size(); ++i) {
            auto *tensor = i == 0 ? fbs->get_test_output_tensor(inputs[i]) : fbs->get_parameter(inputs[i]);
            if (!tensor || !tensor->get_element_ptr()) {
                delete tensor;
                ESP_LOGE(tag, "Missing diagnostic tensor %s", inputs[i].c_str());
                fbs->clear_map();
                return;
            }
            op->m_inputs_index.push_back(context.push_back_tensor(tensor));
        }
        for (const auto &name : outputs) {
            // Outputs are graph ValueInfo, not initializer Tensor metadata.
            // Clone the descriptor already resolved by the model's allocator;
            // this also covers Y_h, which need not have a serialized test value.
            auto *descriptor = model->get_intermediate(name);
            if (!descriptor || descriptor->get_dtype() != dl::DATA_TYPE_INT16) {
                ESP_LOGE(tag, "Missing INT16 runtime output descriptor: %s", name.c_str());
                fbs->clear_map();
                return;
            }
            auto *tensor = new dl::TensorBase(descriptor->get_shape(), nullptr,
                                              descriptor->get_exponent(), descriptor->get_dtype());
            if (!tensor->get_element_ptr()) {
                delete tensor;
                ESP_LOGE(tag, "Diagnostic output allocation failed");
                fbs->clear_map();
                return;
            }
            op->m_outputs_index.push_back(context.push_back_tensor(tensor));
        }
        op->forward(&context, dl::RUNTIME_MODE_SINGLE_CORE);
        auto *output = context.get_tensor(op->m_outputs_index[0]);
        if (!output->is_same_shape(gt.get()) || output->get_dtype() != gt->get_dtype() ||
            output->exponent != gt->exponent) {
            ESP_LOGE(tag, "Isolated output metadata mismatch");
            fbs->clear_map();
            return;
        }
        int changed = 0, max_lsb = 0;
        const auto *a = output->get_element_ptr<int16_t>();
        const auto *g = gt->get_element_ptr<int16_t>();
        for (int i = 0; i < output->get_size(); ++i) {
            const int difference = std::abs((int)a[i] - (int)g[i]);
            changed += difference != 0;
            max_lsb = std::max(max_lsb, difference);
        }
        ESP_LOGI(tag, "GRU_ISOLATED %s changed=%d/%d max_lsb=%d", node_name, changed, output->get_size(), max_lsb);
        // Keep all differences machine-readable for comparison with the host replay.
        for (int i = 0; i < output->get_size(); ++i) {
            if (a[i] != g[i]) printf("GRU_ISOLATED_DIFF %d %d %d\n", i, (int)g[i], (int)a[i]);
        }
    }
    fbs->clear_map();
}
