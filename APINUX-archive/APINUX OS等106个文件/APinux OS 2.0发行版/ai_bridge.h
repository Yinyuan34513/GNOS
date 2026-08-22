// ================================================================
// ai_bridge.h — APinux OS AI 引擎桥接
// 集成：ACPNN-PCD, ACPU, PCP, OPENPUFF, A2 PPS
// ================================================================
#pragma once
#include "kernel.h"
#include <cstdint>

// 前向声明 (各库接口)
namespace acpnn { void init(); void* load_model(const char* path); void infer(void* model, const float* input, float* output); }
namespace acpu  { void init(); void* load_model(const char* path); void train(void* model, const float* data, const int* labels, int n); }
namespace pcp   { void init(); void* optimize_weights(const float* w, size_t n, float sparsity); }
namespace open_puff { void init(); void* create_model(); void forward(void* model, const float* in, float* out); }
namespace a2_pps { void init(); void* load(const char* path); void process(void* ctx, const uint8_t* img, char* text_out); }

// ---------- APinux AI 管理器 ----------
class AIPlatform {
public:
    enum Engine { ENGINE_ACPNN, ENGINE_ACPU, ENGINE_PCP, ENGINE_OPENPUFF, ENGINE_A2 };

    void init_all();
    void* load_model(Engine eng, const char* path);
    void infer(Engine eng, void* model, const float* input, float* output);
    void train(Engine eng, void* model, const float* data, const int* labels, int n);
    void optimize_weights(Engine eng, const float* weights, size_t n, float sparsity, float* out);
    void recognize_text(Engine eng, void* ctx, const uint8_t* img, char* text_out);
};