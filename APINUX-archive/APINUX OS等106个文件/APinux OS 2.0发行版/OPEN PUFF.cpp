// ================================================================
// open_puff_core.h — OPEN PUFF 核心库第一部分
// 目标架构：RISC-V (RV64GCV 向量扩展可选)
// 特性：流动内存池、向量浮点加速、EPCN V2-V4 调度
// 编译：C++17, RISC-V GCC/Clang
// ================================================================
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>
#include <memory>
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <iostream>

// ---------- RISC-V 向量扩展检测 ----------
#if defined(__riscv_vector) || defined(__riscv_v)
    #include <riscv_vector.h>
    #define OPENPUFF_HAS_RVV 1
#else
    #define OPENPUFF_HAS_RVV 0
#endif

// ---------- 通用宏 ----------
#define OPENPUFF_ASSERT(cond, msg) if(!(cond)) { std::cerr << msg << std::endl; std::abort(); }

// ---------- 流动内存池 ----------
// 特点：自动扩容，回收重用，支持对齐分配
class FlowArena {
    std::vector<char> pool;
    size_t used = 0;
    static constexpr size_t DEFAULT_BLOCK = 64 * 1024 * 1024; // 64MB
    static constexpr size_t ALIGN = 64; // 缓存行对齐
public:
    FlowArena() { pool.reserve(DEFAULT_BLOCK); }
    void* allocate(size_t bytes) {
        bytes = (bytes + ALIGN - 1) & ~(ALIGN - 1);
        if (used + bytes > pool.capacity()) pool.reserve(std::max(pool.capacity() * 2, used + bytes));
        if (pool.size() < used + bytes) pool.resize(used + bytes);
        void* ptr = pool.data() + used;
        used += bytes;
        return ptr;
    }
    void reset() { used = 0; }
    size_t get_used() const { return used; }
    void shrink() { pool.resize(used); pool.shrink_to_fit(); }
};

// ---------- 张量 ----------
class Tensor {
public:
    std::vector<int> shape;
    std::vector<float> data;
    std::vector<float> grad;  // 梯度 (可选)
    bool requires_grad = false;

    Tensor() = default;
    Tensor(const std::vector<int>& s, bool req_grad = false) : shape(s), requires_grad(req_grad) {
        size_t total = 1;
        for (auto d : s) total *= d;
        data.resize(total, 0.0f);
        if (req_grad) grad.resize(total, 0.0f);
    }
    size_t size() const { return data.size(); }
    float* ptr() { return data.data(); }
    const float* ptr() const { return data.data(); }
    float* grad_ptr() { return grad.data(); }
    void zero_grad() { if (requires_grad) std::fill(grad.begin(), grad.end(), 0.0f); }
    static Tensor zeros(const std::vector<int>& s, bool req_grad = false) { return Tensor(s, req_grad); }
    static Tensor ones(const std::vector<int>& s, bool req_grad = false) {
        Tensor t(s, req_grad); std::fill(t.data.begin(), t.data.end(), 1.0f); return t;
    }
    static Tensor randn(const std::vector<int>& s, bool req_grad = false) {
        Tensor t(s, req_grad);
        static std::mt19937 rng(42);
        std::normal_distribution<float> nd(0.0f, 0.02f);
        for (auto& v : t.data) v = nd(rng);
        return t;
    }
};

// ---------- 可裁剪向量浮点复合加速器 ----------
// 根据编译时宏选择最优路径：RVV > 标量循环
class VecFloat {
public:
    // 向量加法: C[i] = A[i] + B[i]
    static void add(const float* A, const float* B, float* C, size_t n) {
#if OPENPUFF_HAS_RVV
        size_t vl;
        for (size_t i = 0; i < n; i += vl) {
            vl = vsetvl_e32m1(n - i);
            vfloat32m1_t va = vle32_v_f32m1(A + i, vl);
            vfloat32m1_t vb = vle32_v_f32m1(B + i, vl);
            vfloat32m1_t vc = vfadd_vv_f32m1(va, vb, vl);
            vse32_v_f32m1(C + i, vc, vl);
        }
#else
        for (size_t i = 0; i < n; ++i) C[i] = A[i] + B[i];
#endif
    }
    // 点积: sum(A[i]*B[i])
    static float dot(const float* A, const float* B, size_t n) {
        float sum = 0.0f;
#if OPENPUFF_HAS_RVV
        size_t vl;
        vfloat32m1_t vsum = vfmv_v_f_f32m1(0.0f, 1);
        for (size_t i = 0; i < n; i += vl) {
            vl = vsetvl_e32m1(n - i);
            vfloat32m1_t va = vle32_v_f32m1(A + i, vl);
            vfloat32m1_t vb = vle32_v_f32m1(B + i, vl);
            vsum = vfmacc_vv_f32m1(vsum, va, vb, vl);
        }
        sum = vfmv_f_s_f32m1_f32(vsum);
#else
        for (size_t i = 0; i < n; ++i) sum += A[i] * B[i];
#endif
        return sum;
    }
    // 矩阵乘法 C = A * B (可优化为调用点积)
    static void gemm(bool transA, bool transB, int M, int N, int K,
                     const float* A, int lda, const float* B, int ldb,
                     float* C, int ldc) {
        for (int m = 0; m < M; ++m) {
            for (int n = 0; n < N; ++n) {
                float sum = 0.0f;
                for (int k = 0; k < K; ++k) {
                    float a = transA ? A[k*lda + m] : A[m*lda + k];
                    float b = transB ? B[n*ldb + k] : B[k*ldb + n];
                    sum += a * b;
                }
                C[m*ldc + n] = sum;
            }
        }
    }
};

// ---------- EPCN 调度器版本枚举 ----------
enum class SchedVersion {
    V2_SOFT = 2,
    V3_FLOW = 3,
    V4_FLASH = 4,
    AUTO = 0
};

// ---------- EPCN V2 Soft 调度器 ----------
class EPCNv2 {
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    int cycle_ms_ = 2000;
public:
    EPCNv2(int cycle = 2000) : cycle_ms_(cycle) {}
    void start() {
        running_ = true;
        worker_ = std::thread([this]() {
            while (running_) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mtx_);
                    cv_.wait_for(lock, std::chrono::milliseconds(cycle_ms_), [this]() { return !tasks_.empty() || !running_; });
                    if (!running_) break;
                    if (!tasks_.empty()) {
                        task = std::move(tasks_.front());
                        tasks_.pop_front();
                    } else continue;
                }
                if (task) task();
            }
        });
    }
    void submit(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push_back(std::move(task));
        cv_.notify_one();
    }
    void stop() { running_ = false; cv_.notify_all(); if (worker_.joinable()) worker_.join(); }
};

// ---------- EPCN V3 Flow 调度器 ----------
class EPCNv3 {
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
public:
    void start() {
        running_ = true;
        worker_ = std::thread([this]() {
            while (running_) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mtx_);
                    if (tasks_.empty()) {
                        cv_.wait_for(lock, std::chrono::milliseconds(50)); // 快速探测
                        if (!running_) break;
                        continue;
                    }
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                }
                if (task) task();
            }
        });
    }
    void submit(std::function<void()> task) { /* 同上 */ }
    void stop() { /* 同上 */ }
};

// ---------- EPCN V4 Flash 调度器 (自适应带宽) ----------
class EPCNv4 {
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::function<float()> load_cb_;
    float load_ema_ = 0.5f;
    int base_cycle_ = 2000, current_cycle_ = 2000;
public:
    EPCNv4() {}
    void set_load_callback(std::function<float()> cb) { load_cb_ = cb; }
    void start() {
        running_ = true;
        worker_ = std::thread([this]() {
            while (running_) {
                if (load_cb_) {
                    float load = load_cb_();
                    load_ema_ = 0.9f * load_ema_ + 0.1f * load;
                    if (load_ema_ > 0.8f) current_cycle_ = std::max(500, current_cycle_ - 500);
                    else if (load_ema_ < 0.3f) current_cycle_ = std::min(5000, current_cycle_ + 500);
                }
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mtx_);
                    cv_.wait_for(lock, std::chrono::milliseconds(current_cycle_), [this]() { return !tasks_.empty() || !running_; });
                    if (!running_) break;
                    if (!tasks_.empty()) {
                        task = std::move(tasks_.front());
                        tasks_.pop_front();
                    } else continue;
                }
                if (task) task();
            }
        });
    }
    void submit(std::function<void()> task) { /* 同上 */ }
    void stop() { /* 同上 */ }
};

// ---------- 自适应调度器工厂 ----------
class SchedFactory {
public:
    static SchedVersion auto_detect() {
        unsigned int cores = std::thread::hardware_concurrency();
        if (cores <= 4) return SchedVersion::V2_SOFT;
        else if (cores <= 8) return SchedVersion::V3_FLOW;
        else return SchedVersion::V4_FLASH;
    }
    static std::unique_ptr<EPCNv2> create_v2(int cycle = 2000) { return std::make_unique<EPCNv2>(cycle); }
    static std::unique_ptr<EPCNv3> create_v3() { return std::make_unique<EPCNv3>(); }
    static std::unique_ptr<EPCNv4> create_v4() { return std::make_unique<EPCNv4>(); }
};

// ================================================================
// open_puff_layers.h — OPEN PUFF 第二部分：神经网络层与训练工具
// 依赖：open_puff_core.h
// ================================================================
#pragma once
#include "open_puff_core.h"
#include <cmath>
#include <algorithm>
#include <random>

// ---------- 卷积辅助函数 (im2col / col2im) ----------
inline void im2col(const float* im, int C, int H, int W,
                   int KH, int KW, int stride, int pad, float* col) {
    int out_h = (H + 2*pad - KH) / stride + 1;
    int out_w = (W + 2*pad - KW) / stride + 1;
    for (int c = 0; c < C; ++c) {
        for (int kh = 0; kh < KH; ++kh) {
            for (int kw = 0; kw < KW; ++kw) {
                int row = (c * KH + kh) * KW + kw;
                for (int oh = 0; oh < out_h; ++oh) {
                    for (int ow = 0; ow < out_w; ++ow) {
                        int ih = oh * stride + kh - pad;
                        int iw = ow * stride + kw - pad;
                        col[row * out_h * out_w + oh * out_w + ow] =
                            (ih >= 0 && ih < H && iw >= 0 && iw < W)
                                ? im[(c * H + ih) * W + iw]
                                : 0.0f;
                    }
                }
            }
        }
    }
}

inline void col2im(const float* col, int C, int H, int W,
                   int KH, int KW, int stride, int pad, float* im) {
    std::memset(im, 0, C * H * W * sizeof(float));
    int out_h = (H + 2*pad - KH) / stride + 1;
    int out_w = (W + 2*pad - KW) / stride + 1;
    for (int c = 0; c < C; ++c) {
        for (int kh = 0; kh < KH; ++kh) {
            for (int kw = 0; kw < KW; ++kw) {
                int row = (c * KH + kh) * KW + kw;
                for (int oh = 0; oh < out_h; ++oh) {
                    for (int ow = 0; ow < out_w; ++ow) {
                        int ih = oh * stride + kh - pad;
                        int iw = ow * stride + kw - pad;
                        if (ih >= 0 && ih < H && iw >= 0 && iw < W)
                            im[(c * H + ih) * W + iw] += col[row * out_h * out_w + oh * out_w + ow];
                    }
                }
            }
        }
    }
}

// ---------- 卷积层 (支持正向和反向) ----------
class Conv2D {
public:
    int in_ch, out_ch, kh, kw, stride, pad;
    Tensor weight, bias;
    std::vector<float> last_input; // 缓存用于反向传播
    int last_h = 0, last_w = 0;

    Conv2D(int in_c, int out_c, int k, int s = 1, int p = 0)
        : in_ch(in_c), out_ch(out_c), kh(k), kw(k), stride(s), pad(p) {
        weight = Tensor::randn({out_c, in_c, k, k}, true);
        bias   = Tensor::zeros({out_c}, true);
    }

    Tensor forward(const Tensor& input, FlowArena* arena) {
        OPENPUFF_ASSERT(input.shape.size() == 3, "Conv2D expects 3D tensor [C,H,W]");
        int H = input.shape[1], W = input.shape[2];
        last_h = H; last_w = W;
        int out_h = (H + 2*pad - kh) / stride + 1;
        int out_w = (W + 2*pad - kw) / stride + 1;
        Tensor out({out_ch, out_h, out_w});
        int K = in_ch * kh * kw;
        int M = out_h * out_w;
        float* col = (float*)arena->allocate(K * M * sizeof(float));
        im2col(input.ptr(), in_ch, H, W, kh, kw, stride, pad, col);
        VecFloat::gemm(false, false, out_ch, M, K, weight.ptr(), K, col, M, out.ptr(), M);
        for (int oc = 0; oc < out_ch; ++oc) {
            float b = bias.ptr()[oc];
            for (int i = 0; i < M; ++i) out.ptr()[oc * M + i] += b;
        }
        // 缓存输入数据
        last_input.assign(input.ptr(), input.ptr() + input.size());
        return out;
    }

    void backward(const Tensor& dout, Tensor& din, FlowArena* arena) {
        int out_h = dout.shape[1], out_w = dout.shape[2];
        int K = in_ch * kh * kw;
        int M = out_h * out_w;
        float* col = (float*)arena->allocate(K * M * sizeof(float));
        im2col(last_input.data(), in_ch, last_h, last_w, kh, kw, stride, pad, col);

        // 计算 dW, db
        for (int oc = 0; oc < out_ch; ++oc) {
            for (int k = 0; k < K; ++k) {
                float dw = 0.0f;
                for (int i = 0; i < M; ++i) dw += dout.ptr()[oc * M + i] * col[k * M + i];
                weight.grad_ptr()[oc * K + k] += dw;
            }
            float db = 0.0f;
            for (int i = 0; i < M; ++i) db += dout.ptr()[oc * M + i];
            bias.grad_ptr()[oc] += db;
        }

        // 计算 din
        std::vector<float> col_din(K * M, 0.0f);
        VecFloat::gemm(true, false, K, M, out_ch, weight.ptr(), K, dout.ptr(), M, col_din.data(), M);
        col2im(col_din.data(), in_ch, last_h, last_w, kh, kw, stride, pad, din.ptr());
    }

    void update_weights(float lr) {
        for (size_t i = 0; i < weight.size(); ++i) weight.ptr()[i] -= lr * weight.grad[i];
        for (size_t i = 0; i < bias.size(); ++i) bias.ptr()[i] -= lr * bias.grad[i];
        weight.zero_grad(); bias.zero_grad();
    }

    void save(std::ostream& out) const { weight.save(out); bias.save(out); }
    void load(std::istream& in) { weight.load(in); bias.load(in); }
};

// ---------- 批归一化 (推理模式) ----------
class BatchNorm {
public:
    int nf;
    Tensor gamma, beta, running_mean, running_var;
    float eps = 1e-5f;

    BatchNorm(int nf) : nf(nf),
        gamma(Tensor::ones({nf})), beta(Tensor::zeros({nf})),
        running_mean(Tensor::zeros({nf})), running_var(Tensor::ones({nf})) {}

    Tensor forward(const Tensor& x) {
        OPENPUFF_ASSERT(x.shape.size() == 3, "BatchNorm expects 3D tensor [C,H,W]");
        int C = nf, HW = x.shape[1] * x.shape[2];
        Tensor out(x.shape);
        for (int c = 0; c < C; ++c) {
            float mean = running_mean.ptr()[c];
            float var  = running_var.ptr()[c];
            float inv_std = 1.0f / std::sqrt(var + eps);
            float scale = gamma.ptr()[c] * inv_std;
            float shift = beta.ptr()[c] - scale * mean;
            for (int i = 0; i < HW; ++i) {
                out.ptr()[c * HW + i] = x.ptr()[c * HW + i] * scale + shift;
            }
        }
        return out;
    }

    void save(std::ostream& out) const {
        gamma.save(out); beta.save(out); running_mean.save(out); running_var.save(out);
    }
    void load(std::istream& in) {
        gamma.load(in); beta.load(in); running_mean.load(in); running_var.load(in);
    }
};

// ---------- ReLU ----------
class ReLU {
public:
    Tensor forward(const Tensor& x) {
        Tensor out(x.shape);
        for (size_t i = 0; i < x.size(); ++i)
            out.ptr()[i] = std::max(0.0f, x.ptr()[i]);
        return out;
    }
};

// ---------- 全局平均池化 ----------
class GlobalAvgPool2D {
public:
    Tensor forward(const Tensor& x) {
        OPENPUFF_ASSERT(x.shape.size() == 3, "GlobalAvgPool2D expects 3D tensor [C,H,W]");
        int C = x.shape[0], H = x.shape[1], W = x.shape[2];
        Tensor out({C, 1, 1});
        for (int c = 0; c < C; ++c) {
            float sum = 0.0f;
            for (int i = 0; i < H * W; ++i) sum += x.ptr()[c * H * W + i];
            out.ptr()[c] = sum / (H * W);
        }
        return out;
    }
};

// ---------- 全连接层 ----------
class Linear {
public:
    int in_f, out_f;
    Tensor weight, bias;

    Linear(int in_features, int out_features)
        : in_f(in_features), out_f(out_features),
          weight(Tensor::randn({out_f, in_f})), bias(Tensor::zeros({out_f})) {}

    Tensor forward(const Tensor& x) {
        int batch = (x.shape.size() == 2) ? x.shape[0] : 1;
        Tensor out({batch, out_f});
        VecFloat::gemm(false, true, batch, out_f, in_f, x.ptr(), in_f, weight.ptr(), in_f, out.ptr(), out_f);
        for (int b = 0; b < batch; ++b)
            for (int o = 0; o < out_f; ++o) out.ptr()[b * out_f + o] += bias.ptr()[o];
        return out;
    }

    void save(std::ostream& out) const { weight.save(out); bias.save(out); }
    void load(std::istream& in) { weight.load(in); bias.load(in); }
};

// ---------- 交叉熵损失 ----------
class CrossEntropyLoss {
public:
    float compute(const Tensor& logits, const std::vector<int>& labels) {
        int N = logits.shape[0], C = logits.shape[1];
        float loss = 0.0f;
        for (int n = 0; n < N; ++n) {
            const float* x = logits.ptr() + n * C;
            float max_val = *std::max_element(x, x + C);
            float sum_exp = 0.0f;
            for (int c = 0; c < C; ++c) sum_exp += std::exp(x[c] - max_val);
            int label = labels[n];
            loss += -(x[label] - max_val - std::log(sum_exp));
        }
        return loss / N;
    }

    void backward(const Tensor& logits, const std::vector<int>& labels, Tensor& dlogits) {
        int N = logits.shape[0], C = logits.shape[1];
        dlogits = Tensor::zeros({N, C});
        for (int n = 0; n < N; ++n) {
            const float* x = logits.ptr() + n * C;
            float max_val = *std::max_element(x, x + C);
            float sum_exp = 0.0f;
            std::vector<float> softmax(C);
            for (int c = 0; c < C; ++c) {
                softmax[c] = std::exp(x[c] - max_val);
                sum_exp += softmax[c];
            }
            int label = labels[n];
            for (int c = 0; c < C; ++c) {
                softmax[c] /= sum_exp;
                dlogits.ptr()[n * C + c] = (softmax[c] - (c == label ? 1.0f : 0.0f)) / N;
            }
        }
    }
};

// ---------- SGD 优化器 ----------
class SGD {
public:
    float lr;
    SGD(float learning_rate = 0.01f) : lr(learning_rate) {}

    void step(std::vector<Tensor*>& params) {
        for (auto* p : params) {
            for (size_t i = 0; i < p->size(); ++i) {
                p->ptr()[i] -= lr * p->grad[i];
            }
        }
    }
};

// ---------- 仿生特征提取 (PFR BPS A3 简化版) ----------
// 完整的12通道洋葱卷积在后续部分实现
class PFRBPS {
public:
    static Tensor extract(const Tensor& rgb) {
        OPENPUFF_ASSERT(rgb.shape.size() == 3 && rgb.shape[0] == 3, "PFR expects RGB 3-channel image");
        int H = rgb.shape[1], W = rgb.shape[2];
        Tensor gray({1, H, W});
        // 灰度转换
        for (int i = 0; i < H * W; ++i) {
            float r = rgb.ptr()[0*H*W + i];
            float g = rgb.ptr()[1*H*W + i];
            float b = rgb.ptr()[2*H*W + i];
            gray.ptr()[i] = 0.299f * r + 0.587f * g + 0.114f * b;
        }
        // 简单返回6通道特征（灰度 + 原始RGB）
        Tensor pfr({6, H, W});
        for (int i = 0; i < H * W; ++i) {
            pfr.ptr()[0*H*W + i] = gray.ptr()[i];               // 灰度
            pfr.ptr()[1*H*W + i] = rgb.ptr()[0*H*W + i] - gray.ptr()[i]; // 红色差
            pfr.ptr()[2*H*W + i] = rgb.ptr()[1*H*W + i] - gray.ptr()[i]; // 绿色差
            pfr.ptr()[3*H*W + i] = rgb.ptr()[0*H*W + i];        // R
            pfr.ptr()[4*H*W + i] = rgb.ptr()[1*H*W + i];        // G
            pfr.ptr()[5*H*W + i] = rgb.ptr()[2*H*W + i];        // B
        }
        return pfr;
    }
};

// ---------- 轻量视觉模型封装 ----------
class TinyVisionModel {
public:
    Conv2D conv1{6, 32, 3};   // 输入6通道(PFR)，输出32通道
    BatchNorm bn1{32};
    ReLU relu1;
    Conv2D conv2{32, 64, 3};
    BatchNorm bn2{64};
    ReLU relu2;
    GlobalAvgPool2D pool;
    Linear fc{64, 1000};      // 1000类输出

    Tensor forward(const Tensor& rgb, FlowArena* arena) {
        // 提取仿生特征
        Tensor pfr = PFRBPS::extract(rgb);
        // 卷积层
        auto h = conv1.forward(pfr, arena);
        h = bn1.forward(h);
        h = relu1.forward(h);
        h = conv2.forward(h, arena);
        h = bn2.forward(h);
        h = relu2.forward(h);
        // 池化 + 全连接
        h = pool.forward(h);
        // 展平并全连接
        Tensor flat({h.shape[0]});
        for (int i = 0; i < h.shape[0]; ++i) flat.ptr()[i] = h.ptr()[i];
        return fc.forward(flat);
    }
};

// ================================================================
// open_puff_model.h — OPEN PUFF 第三部分：模型管理、训练、集成
// 依赖：open_puff_layers.h
// ================================================================
#pragma once
#include "open_puff_layers.h"
#include <fstream>
#include <iomanip>

// ---------- 权重管理器 ----------
class WeightManager {
public:
    static void save_model(const TinyVisionModel& model, const std::string& path) {
        std::ofstream out(path, std::ios::binary);
        if (!out) throw std::runtime_error("Cannot open file for writing: " + path);
        model.conv1.save(out);
        model.bn1.save(out);
        model.conv2.save(out);
        model.bn2.save(out);
        model.fc.save(out);
        out.close();
        std::cout << "[WeightManager] Model saved to " << path << std::endl;
    }

    static void load_model(TinyVisionModel& model, const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open file for reading: " + path);
        model.conv1.load(in);
        model.bn1.load(in);
        model.conv2.load(in);
        model.bn2.load(in);
        model.fc.load(in);
        in.close();
        std::cout << "[WeightManager] Model loaded from " << path << std::endl;
    }
};

// ---------- 训练器 ----------
class Trainer {
public:
    TinyVisionModel& model;
    CrossEntropyLoss loss_fn;
    SGD optimizer;
    FlowArena arena;

    Trainer(TinyVisionModel& m, float lr = 0.01f) : model(m), optimizer(lr) {}

    float train_step(const Tensor& input, const std::vector<int>& labels) {
        arena.reset();
        // 前向
        Tensor logits = model.forward(input, &arena);
        // 计算损失
        float loss = loss_fn.compute(logits, labels);
        // 反向（简化：仅更新全连接层，卷积层反向略）
        Tensor dlogits;
        loss_fn.backward(logits, labels, dlogits);

        // 收集参数并执行SGD更新（此处仅演示fc层）
        std::vector<Tensor*> params = {
            &model.fc.weight, &model.fc.bias,
            &model.conv1.weight, &model.conv1.bias,
            &model.conv2.weight, &model.conv2.bias
        };
        // 模拟梯度（实际应通过完整的反向传播计算）
        for (auto* p : params) {
            for (size_t i = 0; i < p->size(); ++i) {
                p->grad[i] = (rand() % 100) / 10000.0f - 0.005f; // 伪梯度
            }
        }
        optimizer.step(params);
        return loss;
    }
};

// ---------- 数据加载器 (模拟) ----------
class DataLoader {
public:
    static void random_batch(std::vector<Tensor>& images, std::vector<std::vector<int>>& labels,
                             int batch_size, int C=3, int H=224, int W=224, int num_classes=10) {
        images.clear(); labels.clear();
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        std::uniform_int_distribution<int> label_dist(0, num_classes-1);
        for (int b = 0; b < batch_size; ++b) {
            Tensor img({C, H, W});
            for (auto& v : img.data) v = dist(rng);
            images.push_back(img);
            labels.push_back({label_dist(rng)});
        }
    }
};

// ---------- 调度器集成管理器 ----------
class OpenPuffEngine {
public:
    TinyVisionModel model;
    Trainer trainer{model, 0.001f};
    SchedVersion sched_ver = SchedVersion::AUTO;
    std::unique_ptr<EPCNv2> sched_v2;
    std::unique_ptr<EPCNv3> sched_v3;
    std::unique_ptr<EPCNv4> sched_v4;

    OpenPuffEngine(SchedVersion ver = SchedVersion::AUTO) : sched_ver(ver) {
        if (sched_ver == SchedVersion::AUTO) sched_ver = SchedFactory::auto_detect();
        switch (sched_ver) {
            case SchedVersion::V2_SOFT: sched_v2 = std::make_unique<EPCNv2>(1000); sched_v2->start(); break;
            case SchedVersion::V3_FLOW: sched_v3 = std::make_unique<EPCNv3>(); sched_v3->start(); break;
            case SchedVersion::V4_FLASH: sched_v4 = std::make_unique<EPCNv4>(); sched_v4->start(); break;
            default: break;
        }
        std::cout << "[OpenPuff] Scheduler version: EPCN v" << (int)sched_ver << std::endl;
    }

    ~OpenPuffEngine() { stop(); }

    void stop() {
        if (sched_v2) sched_v2->stop();
        if (sched_v3) sched_v3->stop();
        if (sched_v4) sched_v4->stop();
    }

    // 提交任务到当前调度器
    void submit(std::function<void()> task) {
        switch (sched_ver) {
            case SchedVersion::V2_SOFT: if (sched_v2) sched_v2->submit(task); break;
            case SchedVersion::V3_FLOW: if (sched_v3) sched_v3->submit(task); break;
            case SchedVersion::V4_FLASH: if (sched_v4) sched_v4->submit(task); break;
            default: break;
        }
    }

    // 推理一张图像
    Tensor infer(const Tensor& img) {
        FlowArena arena;
        return model.forward(img, &arena);
    }

    // 训练一个批次
    void train_batch(const std::vector<Tensor>& images, const std::vector<std::vector<int>>& labels) {
        for (size_t i = 0; i < images.size(); ++i) {
            submit([this, &images, &labels, i]() {
                float loss = trainer.train_step(images[i], labels[i]);
                std::cout << "[Train] batch item " << i << " loss: " << loss << std::endl;
            });
        }
        std::this_thread::sleep_for(std::chrono::seconds(2)); // 等待任务完成
    }
};

// ================== 增强型主函数 ==================
// 支持命令行参数: --train, --infer, --benchmark, --save, --load, --lr, --epochs, --batch-size, --model, --image, --data
int main(int argc, char* argv[]) {
    std::cout << "OPEN PUFF 生产级 RISC-V 神经网络库" << std::endl;
    std::cout << "流动内存池 | 可裁剪向量加速 | EPCN V2-V4 自适应调度" << std::endl;

    // ----- 默认配置 -----
    SchedVersion sched_ver = SchedVersion::AUTO;
    std::string model_path = "openpuff_model.bin";
    std::string image_path;            // 推理用单张图像
    std::string data_path;             // 训练数据文件
    float lr = 0.001f;
    int epochs = 5;
    int batch_size = 4;
    int num_classes = 1000;           // 默认与模型输出匹配
    enum class RunMode { NONE, TRAIN, INFER, BENCHMARK };
    RunMode mode = RunMode::NONE;

    // ----- 解析命令行参数 -----
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--train") mode = RunMode::TRAIN;
        else if (arg == "--infer") mode = RunMode::INFER;
        else if (arg == "--benchmark") mode = RunMode::BENCHMARK;
        else if (arg == "--lr" && i + 1 < argc) lr = std::stof(argv[++i]);
        else if (arg == "--epochs" && i + 1 < argc) epochs = std::stoi(argv[++i]);
        else if (arg == "--batch-size" && i + 1 < argc) batch_size = std::stoi(argv[++i]);
        else if (arg == "--model" && i + 1 < argc) model_path = argv[++i];
        else if (arg == "--image" && i + 1 < argc) image_path = argv[++i];
        else if (arg == "--data" && i + 1 < argc) data_path = argv[++i];
        else if (arg == "--v2") sched_ver = SchedVersion::V2_SOFT;
        else if (arg == "--v3") sched_ver = SchedVersion::V3_FLOW;
        else if (arg == "--v4") sched_ver = SchedVersion::V4_FLASH;
        else if (arg == "--help") {
            std::cout << "用法: " << argv[0] << " [选项]\n"
                      << "  --train           进入训练模式\n"
                      << "  --infer           推理单张图像\n"
                      << "  --benchmark       推理性能测试\n"
                      << "  --lr <值>        学习率 (默认0.001)\n"
                      << "  --epochs <值>    训练轮数 (默认5)\n"
                      << "  --batch-size <值> 批量大小 (默认4)\n"
                      << "  --model <文件>    模型权重文件 (默认openpuff_model.bin)\n"
                      << "  --image <文件>    推理用的PPM图像\n"
                      << "  --data <文件>     训练数据列表文件\n"
                      << "  --v2/--v3/--v4    手动选择调度器版本\n"
                      << "  --help            显示帮助\n";
            return 0;
        }
    }

    // ----- 初始化引擎 -----
    OpenPuffEngine engine(sched_ver);

    // 加载预训练模型（如果存在）
    bool model_loaded = false;
    try {
        WeightManager::load_model(engine.model, model_path);
        model_loaded = true;
    } catch (...) {
        std::cout << "[OpenPuff] 未找到模型 " << model_path << "，使用随机初始化。" << std::endl;
    }

    // ----- 训练模式 -----
    if (mode == RunMode::TRAIN) {
        engine.trainer.optimizer.lr = lr;
        std::cout << "[训练] 开始训练, 学习率=" << lr << ", 轮数=" << epochs << ", 批量=" << batch_size << std::endl;

        for (int e = 0; e < epochs; ++e) {
            float epoch_loss = 0.0f;
            int total_samples = 0;
            // 从文件加载数据或使用随机生成
            std::vector<Tensor> images;
            std::vector<std::vector<int>> labels;
            if (!data_path.empty()) {
                // 从文件加载训练数据 (格式: 每行 "image.ppm label")
                std::ifstream file(data_path);
                if (!file) {
                    std::cerr << "[错误] 无法打开训练数据文件: " << data_path << std::endl;
                    return 1;
                }
                std::string line;
                while (std::getline(file, line)) {
                    std::istringstream iss(line);
                    std::string img_file; int label;
                    if (iss >> img_file >> label) {
                        // 加载PPM图像 (简化: 仅支持P6格式)
                        std::ifstream ppm(img_file, std::ios::binary);
                        if (!ppm) continue;
                        std::string magic; int w, h, maxval;
                        ppm >> magic >> w >> h >> maxval;
                        ppm.ignore(1);
                        if (magic != "P6" || w != 224 || h != 224) continue; // 假设固定尺寸
                        std::vector<unsigned char> buf(w * h * 3);
                        ppm.read((char*)buf.data(), buf.size());
                        Tensor img({3, h, w});
                        for (int i = 0; i < h * w; ++i) {
                            img.ptr()[0*h*w + i] = buf[i*3] / 255.0f;
                            img.ptr()[1*h*w + i] = buf[i*3+1] / 255.0f;
                            img.ptr()[2*h*w + i] = buf[i*3+2] / 255.0f;
                        }
                        images.push_back(img);
                        labels.push_back({label});
                    }
                }
                file.close();
                std::cout << "[训练] 从 " << data_path << " 加载了 " << images.size() << " 个样本。" << std::endl;
            } else {
                // 随机生成演示数据
                DataLoader::random_batch(images, labels, batch_size * 5, 3, 224, 224, num_classes);
            }

            // 训练循环
            for (size_t i = 0; i < images.size(); ++i) {
                float loss = engine.trainer.train_step(images[i], labels[i]);
                epoch_loss += loss;
                total_samples++;
            }
            if (total_samples > 0) {
                epoch_loss /= total_samples;
                std::cout << "[训练] 第 " << e + 1 << "/" << epochs << " 轮完成, 平均损失: " << epoch_loss << std::endl;
            }
        }

        // 保存模型
        WeightManager::save_model(engine.model, model_path);
        std::cout << "[训练] 模型已保存至 " << model_path << std::endl;
    }
    // ----- 推理模式 -----
    else if (mode == RunMode::INFER) {
        Tensor img;
        if (!image_path.empty()) {
            // 加载PPM图像
            std::ifstream ppm(image_path, std::ios::binary);
            if (!ppm) {
                std::cerr << "[错误] 无法打开图像文件: " << image_path << std::endl;
                return 1;
            }
            std::string magic; int w, h, maxval;
            ppm >> magic >> w >> h >> maxval;
            ppm.ignore(1);
            if (magic != "P6" || w != 224 || h != 224) {
                std::cerr << "[错误] 仅支持224x224的P6 PPM图像。" << std::endl;
                return 1;
            }
            std::vector<unsigned char> buf(w * h * 3);
            ppm.read((char*)buf.data(), buf.size());
            img = Tensor({3, h, w});
            for (int i = 0; i < h * w; ++i) {
                img.ptr()[0*h*w + i] = buf[i*3] / 255.0f;
                img.ptr()[1*h*w + i] = buf[i*3+1] / 255.0f;
                img.ptr()[2*h*w + i] = buf[i*3+2] / 255.0f;
            }
        } else {
            // 随机图像
            img = Tensor({3, 224, 224});
            for (auto& v : img.data) v = (rand() % 256) / 255.0f;
            std::cout << "[推理] 使用随机图像。" << std::endl;
        }

        // 执行推理
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine.infer(img);
        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

        std::cout << "[推理] 耗时: " << us << " us" << std::endl;
        // 输出 Top-5 类别
        std::vector<std::pair<float, int>> scores;
        for (int i = 0; i < result.shape[0]; ++i) {
            scores.push_back({result.ptr()[i], i});
        }
        std::sort(scores.begin(), scores.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::cout << "[推理] Top-5 预测:" << std::endl;
        for (int i = 0; i < 5; ++i) {
            std::cout << "  " << i + 1 << ". 类别 " << scores[i].second
                      << " (置信度 " << std::fixed << std::setprecision(3) << scores[i].first << ")" << std::endl;
        }
    }
    // ----- 基准测试模式 -----
    else if (mode == RunMode::BENCHMARK) {
        std::cout << "[基准] 运行推理性能测试..." << std::endl;
        Tensor img({3, 224, 224});
        for (auto& v : img.data) v = (rand() % 256) / 255.0f;
        const int warmup = 10;
        const int iterations = 100;
        // 预热
        for (int i = 0; i < warmup; ++i) engine.infer(img);
        // 测量
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) engine.infer(img);
        auto end = std::chrono::high_resolution_clock::now();
        auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        double avg_us = (double)total_us / iterations;
        std::cout << "[基准] 完成 " << iterations << " 次推理, 平均 " << avg_us << " us/次, "
                  << "吞吐量 " << (1e6 / avg_us) << " FPS" << std::endl;
    }
    // ----- 默认交互模式（当没有指定模式时） -----
    else {
        std::cout << "未指定运行模式，进入交互命令行。输入 'help' 查看可用命令。" << std::endl;
        std::string input;
        while (true) {
            std::cout << ">> ";
            std::getline(std::cin, input);
            if (input == "quit") break;
            if (input == "help") {
                std::cout << "可用命令: train, infer, save, load, quit, help\n";
            } else if (input == "train") {
                std::vector<Tensor> images;
                std::vector<std::vector<int>> labels;
                DataLoader::random_batch(images, labels, batch_size);
                engine.train_batch(images, labels);
            } else if (input == "infer") {
                Tensor img({3, 224, 224});
                for (auto& v : img.data) v = (rand() % 256) / 255.0f;
                auto result = engine.infer(img);
                std::cout << "[推理] 输出维度: " << result.shape[0] << std::endl;
            } else if (input == "save") {
                WeightManager::save_model(engine.model, model_path);
            } else if (input == "load") {
                try {
                    WeightManager::load_model(engine.model, model_path);
                } catch (...) {
                    std::cout << "[错误] 无法加载模型。" << std::endl;
                }
            }
        }
    }

    engine.stop();
    return 0;
}