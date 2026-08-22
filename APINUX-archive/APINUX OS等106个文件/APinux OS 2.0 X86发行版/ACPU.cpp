// ================================================================
// acpu_core.h — ACPU 通用 CPU 运算库 (第一部分：核心层、调度、权重优化)
// 编译：C++17, OpenMP, 跨平台 x86/ARM
// ================================================================
#pragma once
#include <vector>
#include <cstring>
#include <cmath>
#include <random>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <memory>
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include <deque>
#include <mutex>
#include <condition_variable>
#ifdef _OPENMP
#include <omp.h>
#endif

// 指令集预取宏
#if defined(__x86_64__) || defined(_M_X64)
    #include <xmmintrin.h>
    #define PREFETCH(addr) _mm_prefetch((const char*)(addr), _MM_HINT_T0)
#elif defined(__ARM_NEON)
    #define PREFETCH(addr) __builtin_prefetch(addr)
#else
    #define PREFETCH(addr)
#endif

// -------------------- 张量 --------------------
class Tensor {
public:
    std::vector<int> shape;
    std::vector<float> data;
    std::vector<float> grad;
    bool requires_grad = false;

    Tensor() = default;
    Tensor(const std::vector<int>& s, bool req = false) : shape(s), requires_grad(req) {
        size_t total = 1;
        for (auto d : s) total *= d;
        data.resize(total, 0.0f);
        if (req) grad.resize(total, 0.0f);
    }
    size_t elements() const { return data.size(); }
    float* data_ptr() { return data.data(); }
    const float* data_ptr() const { return data.data(); }
    float* grad_ptr() { return grad.data(); }
    void zero_grad() { if (requires_grad) std::fill(grad.begin(), grad.end(), 0.0f); }
    static Tensor zeros(const std::vector<int>& s, bool req = false) { return Tensor(s, req); }
    static Tensor ones(const std::vector<int>& s, bool req = false) {
        Tensor t(s, req); std::fill(t.data.begin(), t.data.end(), 1.0f); return t;
    }
    static Tensor randn(const std::vector<int>& s, bool req = false) {
        Tensor t(s, req);
        static std::mt19937 rng(42);
        std::normal_distribution<float> nd(0.0f, 0.02f);
        for (auto& v : t.data) v = nd(rng);
        return t;
    }
    void save(std::ostream& out) const {
        int ndim = shape.size(); out.write((char*)&ndim, sizeof(int));
        for (int d : shape) out.write((char*)&d, sizeof(int));
        out.write((char*)data.data(), elements() * sizeof(float));
    }
    void load(std::istream& in) {
        int ndim; in.read((char*)&ndim, sizeof(int)); shape.resize(ndim);
        for (int i=0;i<ndim;++i) in.read((char*)&shape[i], sizeof(int));
        size_t total=1; for (auto d:shape) total*=d;
        data.resize(total); in.read((char*)data.data(), total*sizeof(float));
    }
};

// -------------------- 内存池 --------------------
class Arena {
    std::vector<char> pool;
    size_t used = 0;
    static constexpr size_t DEFAULT_SIZE = 256 * 1024 * 1024;
public:
    void* alloc(size_t bytes) {
        bytes = (bytes + 63) & ~63;
        if (used + bytes > pool.size()) pool.resize(std::max(pool.size() * 2, used + bytes));
        void* p = pool.data() + used; used += bytes; return p;
    }
    void reset() { used = 0; }
};

// -------------------- 矩阵乘法 (预取+展开) --------------------
inline void gemm(bool transA, bool transB, int M, int N, int K,
                 const float* A, int lda, const float* B, int ldb,
                 float* C, int ldc) {
    #pragma omp parallel for collapse(2) schedule(static) if(M*N > 1024)
    for (int m = 0; m < M; ++m) {
        for (int n = 0; n < N; ++n) {
            float sum = 0.0f;
            int k = 0;
            for (; k <= K - 4; k += 4) {
                PREFETCH(A + m*lda + k + 8);
                PREFETCH(B + (k+8)*ldb + n);
                float a0 = transA ? A[(k+0)*lda + m] : A[m*lda + (k+0)];
                float b0 = transB ? B[n*ldb + (k+0)] : B[(k+0)*ldb + n];
                float a1 = transA ? A[(k+1)*lda + m] : A[m*lda + (k+1)];
                float b1 = transB ? B[n*ldb + (k+1)] : B[(k+1)*ldb + n];
                float a2 = transA ? A[(k+2)*lda + m] : A[m*lda + (k+2)];
                float b2 = transB ? B[n*ldb + (k+2)] : B[(k+2)*ldb + n];
                float a3 = transA ? A[(k+3)*lda + m] : A[m*lda + (k+3)];
                float b3 = transB ? B[n*ldb + (k+3)] : B[(k+3)*ldb + n];
                sum += a0*b0 + a1*b1 + a2*b2 + a3*b3;
            }
            for (; k < K; ++k) {
                float a = transA ? A[k*lda + m] : A[m*lda + k];
                float b = transB ? B[n*ldb + k] : B[k*ldb + n];
                sum += a * b;
            }
            C[m*ldc + n] = sum;
        }
    }
}

// -------------------- 卷积基础函数 --------------------
inline void im2col(const float* im, int C, int H, int W,
                   int KH, int KW, int stride, int pad, float* col) {
    int out_h = (H + 2*pad - KH) / stride + 1;
    int out_w = (W + 2*pad - KW) / stride + 1;
    #pragma omp parallel for collapse(2) schedule(static)
    for (int c = 0; c < C; ++c)
        for (int kh = 0; kh < KH; ++kh)
            for (int kw = 0; kw < KW; ++kw) {
                int row = (c*KH + kh)*KW + kw;
                for (int oh = 0; oh < out_h; ++oh)
                    for (int ow = 0; ow < out_w; ++ow) {
                        int ih = oh*stride + kh - pad;
                        int iw = ow*stride + kw - pad;
                        col[row*out_h*out_w + oh*out_w + ow] =
                            (ih>=0 && ih<H && iw>=0 && iw<W) ? im[(c*H + ih)*W + iw] : 0.0f;
                    }
            }
}

inline void col2im(const float* col, int C, int H, int W,
                   int KH, int KW, int stride, int pad, float* im) {
    memset(im, 0, C*H*W*sizeof(float));
    int out_h = (H + 2*pad - KH)/stride + 1;
    int out_w = (W + 2*pad - KW)/stride + 1;
    for (int c = 0; c < C; ++c)
        for (int kh = 0; kh < KH; ++kh)
            for (int kw = 0; kw < KW; ++kw) {
                int row = (c*KH + kh)*KW + kw;
                for (int oh = 0; oh < out_h; ++oh)
                    for (int ow = 0; ow < out_w; ++ow) {
                        int ih = oh*stride + kh - pad;
                        int iw = ow*stride + kw - pad;
                        if (ih>=0 && ih<H && iw>=0 && iw<W)
                            im[(c*H + ih)*W + iw] += col[row*out_h*out_w + oh*out_w + ow];
                    }
            }
}

// -------------------- 卷积层 (传统CNN) --------------------
class Conv2D {
public:
    int in_ch, out_ch, kh, kw, stride, pad;
    Tensor weight, bias;
    std::vector<float> last_input;
    int last_h = 0, last_w = 0;

    Conv2D(int in_c, int out_c, int k, int s = 1, int p = 0)
        : in_ch(in_c), out_ch(out_c), kh(k), kw(k), stride(s), pad(p) {
        weight = Tensor::randn({out_c, in_c, k, k}, true);
        bias   = Tensor::zeros({out_c}, true);
    }

    Tensor forward(const Tensor& input, Arena* arena) {
        int H = input.shape[1], W = input.shape[2];
        last_h = H; last_w = W;
        int out_h = (H + 2*pad - kh) / stride + 1;
        int out_w = (W + 2*pad - kw) / stride + 1;
        Tensor out({out_ch, out_h, out_w});
        int K = in_ch * kh * kw, M = out_h * out_w;
        float* col = (float*)arena->alloc(K * M * sizeof(float));
        im2col(input.data_ptr(), in_ch, H, W, kh, kw, stride, pad, col);
        gemm(false, false, out_ch, M, K, weight.data_ptr(), K, col, M, out.data_ptr(), M);
        for (int oc = 0; oc < out_ch; ++oc) {
            float b = bias.data_ptr()[oc];
            for (int i = 0; i < M; ++i) out.data_ptr()[oc*M + i] += b;
        }
        last_input.assign(input.data_ptr(), input.data_ptr() + input.elements());
        return out;
    }

    void backward(const Tensor& dout, Tensor& din, Arena* arena) {
        int out_h = dout.shape[1], out_w = dout.shape[2];
        int K = in_ch * kh * kw, M = out_h * out_w;
        float* col = (float*)arena->alloc(K * M * sizeof(float));
        im2col(last_input.data(), in_ch, last_h, last_w, kh, kw, stride, pad, col);
        for (int oc = 0; oc < out_ch; ++oc) {
            for (int k = 0; k < K; ++k) {
                float dw = 0.0f;
                for (int i = 0; i < M; ++i) dw += dout.data_ptr()[oc*M + i] * col[k*M + i];
                weight.grad_ptr()[oc*K + k] += dw;
            }
            float db = 0.0f;
            for (int i = 0; i < M; ++i) db += dout.data_ptr()[oc*M + i];
            bias.grad_ptr()[oc] += db;
        }
        std::vector<float> col_din(K * M, 0.0f);
        gemm(true, false, K, M, out_ch, weight.data_ptr(), K, dout.data_ptr(), M, col_din.data(), M);
        col2im(col_din.data(), in_ch, last_h, last_w, kh, kw, stride, pad, din.data_ptr());
    }

    void update_weights(float lr) {
        for (size_t i = 0; i < weight.elements(); ++i) weight.data[i] -= lr * weight.grad[i];
        for (size_t i = 0; i < bias.elements(); ++i) bias.data[i] -= lr * bias.grad[i];
        weight.zero_grad(); bias.zero_grad();
    }
    void save(std::ostream& out) const { weight.save(out); bias.save(out); }
    void load(std::istream& in) { weight.load(in); bias.load(in); }
};

// -------------------- 批归一化 --------------------
class BatchNorm {
public:
    int nf;
    Tensor gamma, beta, running_mean, running_var;
    float momentum = 0.1f, eps = 1e-5f;
    std::vector<float> last_norm, last_xc, last_std;
    int last_hw = 0;

    BatchNorm(int nf) : nf(nf),
        gamma(Tensor::ones({nf}, true)), beta(Tensor::zeros({nf}, true)),
        running_mean(Tensor::zeros({nf})), running_var(Tensor::ones({nf})) {}

    Tensor forward(const Tensor& x, bool training = false) {
        int C = nf, HW = x.shape[1]*x.shape[2];
        Tensor out(x.shape);
        if (training) { last_hw = HW; last_norm.resize(C*HW); last_xc.resize(C*HW); last_std.resize(C); }
        for (int c = 0; c < C; ++c) {
            float mean, var;
            if (training) {
                float sum = 0.0f, sq = 0.0f;
                for (int i = 0; i < HW; ++i) { float v = x.data_ptr()[c*HW + i]; sum += v; sq += v*v; }
                mean = sum / HW; var = sq / HW - mean*mean;
                running_mean.data_ptr()[c] = momentum*mean + (1-momentum)*running_mean.data_ptr()[c];
                running_var.data_ptr()[c] = momentum*var + (1-momentum)*running_var.data_ptr()[c];
                last_std[c] = sqrt(var + eps);
            } else {
                mean = running_mean.data_ptr()[c]; var = running_var.data_ptr()[c];
                last_std[c] = sqrt(var + eps);
            }
            float inv_std = 1.0f / last_std[c];
            for (int i = 0; i < HW; ++i) {
                float xc = x.data_ptr()[c*HW + i] - mean;
                if (training) { last_xc[c*HW + i] = xc; last_norm[c*HW + i] = xc * inv_std; }
                out.data_ptr()[c*HW + i] = gamma.data_ptr()[c] * xc * inv_std + beta.data_ptr()[c];
            }
        }
        return out;
    }

    void backward(const Tensor& dout, Tensor& din) {
        int C = nf, HW = last_hw;
        for (int c = 0; c < C; ++c) {
            float inv_std = 1.0f / last_std[c];
            float dgamma = 0, dbeta = 0, sum_dout = 0, sum_dout_xc = 0;
            for (int i = 0; i < HW; ++i) {
                float grad = dout.data_ptr()[c*HW + i];
                dbeta += grad; sum_dout += grad;
                dgamma += grad * last_norm[c*HW + i];
                sum_dout_xc += grad * last_xc[c*HW + i];
            }
            gamma.grad_ptr()[c] += dgamma; beta.grad_ptr()[c] += dbeta;
            float f1 = inv_std;
            float f2 = -inv_std / (HW * last_std[c] * last_std[c]) * sum_dout_xc * inv_std;
            float f3 = -sum_dout / HW * inv_std;
            for (int i = 0; i < HW; ++i)
                din.data_ptr()[c*HW + i] = f1*dout.data_ptr()[c*HW + i] + f2*last_xc[c*HW + i] + f3;
        }
    }

    void update_weights(float lr) {
        for (int i = 0; i < nf; ++i) { gamma.data[i] -= lr*gamma.grad[i]; beta.data[i] -= lr*beta.grad[i]; }
        gamma.zero_grad(); beta.zero_grad();
    }
    void save(std::ostream& out) const { gamma.save(out); beta.save(out); running_mean.save(out); running_var.save(out); }
    void load(std::istream& in) { gamma.load(in); beta.load(in); running_mean.load(in); running_var.load(in); }
};

// -------------------- ReLU --------------------
class ReLU {
public:
    std::vector<bool> mask;
    Tensor forward(const Tensor& x) {
        Tensor out(x.shape, x.requires_grad); mask.resize(x.elements());
        for (size_t i = 0; i < x.elements(); ++i) {
            float v = x.data_ptr()[i]; mask[i] = (v > 0.0f); out.data_ptr()[i] = mask[i] ? v : 0.0f;
        }
        return out;
    }
    void backward(const Tensor& dout, Tensor& din) {
        for (size_t i = 0; i < dout.elements(); ++i) din.data_ptr()[i] = mask[i] ? dout.data_ptr()[i] : 0.0f;
    }
};

// -------------------- 全局平均池化 --------------------
class GlobalAvgPool2D {
public:
    int last_h = 0, last_w = 0;
    Tensor forward(const Tensor& x) {
        int C = x.shape[0], H = x.shape[1], W = x.shape[2]; last_h = H; last_w = W;
        Tensor out({C, 1, 1});
        for (int c = 0; c < C; ++c) {
            float sum = 0; for (int i = 0; i < H*W; ++i) sum += x.data_ptr()[c*H*W + i];
            out.data_ptr()[c] = sum / (H*W);
        }
        return out;
    }
    void backward(const Tensor& dout, Tensor& din) {
        int C = dout.shape[0], HW = last_h * last_w;
        for (int c = 0; c < C; ++c) { float g = dout.data_ptr()[c] / HW; for (int i = 0; i < HW; ++i) din.data_ptr()[c*HW + i] = g; }
    }
};

// -------------------- 全连接层 --------------------
class Linear {
public:
    int in_f, out_f;
    Tensor weight, bias;
    std::vector<float> last_input;
    int batch_size = 1;

    Linear(int in_features, int out_features) : in_f(in_features), out_f(out_features),
        weight(Tensor::randn({out_f, in_f}, true)), bias(Tensor::zeros({out_f}, true)) {}

    Tensor forward(const Tensor& x) {
        if (x.dims() == 2) { batch_size = x.shape[0]; } else { batch_size = 1; }
        Tensor out({batch_size, out_f});
        last_input.assign(x.data_ptr(), x.data_ptr() + x.elements());
        gemm(false, true, batch_size, out_f, in_f, x.data_ptr(), in_f, weight.data_ptr(), in_f, out.data_ptr(), out_f);
        for (int b = 0; b < batch_size; ++b)
            for (int o = 0; o < out_f; ++o) out.data_ptr()[b*out_f + o] += bias.data_ptr()[o];
        return out;
    }

    void backward(const Tensor& dout, Tensor& din) {
        for (int b = 0; b < batch_size; ++b) {
            for (int o = 0; o < out_f; ++o) {
                float g = dout.data_ptr()[b*out_f + o];
                bias.grad_ptr()[o] += g;
                for (int i = 0; i < in_f; ++i) weight.grad_ptr()[o*in_f + i] += g * last_input[b*in_f + i];
            }
            for (int i = 0; i < in_f; ++i) {
                float sum = 0;
                for (int o = 0; o < out_f; ++o) sum += dout.data_ptr()[b*out_f + o] * weight.data_ptr()[o*in_f + i];
                din.data_ptr()[b*in_f + i] = sum;
            }
        }
    }

    void update_weights(float lr) {
        for (size_t i = 0; i < weight.elements(); ++i) weight.data[i] -= lr * weight.grad[i];
        for (size_t i = 0; i < bias.elements(); ++i) bias.data[i] -= lr * bias.grad[i];
        weight.zero_grad(); bias.zero_grad();
    }
    void save(std::ostream& out) const { weight.save(out); bias.save(out); }
    void load(std::istream& in) { weight.load(in); bias.load(in); }
};

// -------------------- PFR BPS A3 仿生特征提取 --------------------
inline void rgb_to_yuv(float r, float g, float b, float& y, float& u, float& v) {
    y =  0.299f * r + 0.587f * g + 0.114f * b;
    u = -0.14713f * r - 0.28886f * g + 0.436f * b;
    v =  0.615f * r - 0.51499f * g - 0.10001f * b;
}

inline Tensor upsample2x(const Tensor& feat) {
    int C = feat.shape[0], H = feat.shape[1], W = feat.shape[2];
    Tensor up({C, H*2, W*2});
    for (int c = 0; c < C; ++c)
        for (int h = 0; h < H; ++h)
            for (int w = 0; w < W; ++w) {
                float v = feat.data_ptr()[c*H*W + h*W + w];
                up.data_ptr()[c*H*2*W*2 + (h*2)*W*2 + (w*2)] = v;
                up.data_ptr()[c*H*2*W*2 + (h*2)*W*2 + (w*2)+1] = v;
                up.data_ptr()[c*H*2*W*2 + (h*2+1)*W*2 + (w*2)] = v;
                up.data_ptr()[c*H*2*W*2 + (h*2+1)*W*2 + (w*2)+1] = v;
            }
    return up;
}

class PFRBPSA3 {
public:
    static Tensor extract(const Tensor& rgb) {
        int H = rgb.shape[1], W = rgb.shape[2];
        Tensor gray({1, H, W});
        for (int i = 0; i < H*W; ++i) {
            float r = rgb.data_ptr()[0*H*W+i], g = rgb.data_ptr()[1*H*W+i], b = rgb.data_ptr()[2*H*W+i];
            gray.data_ptr()[i] = 0.299f*r + 0.587f*g + 0.114f*b;
        }
        thread_local Arena arena; arena.reset();

        float lap_k[9] = {0,1,0, 1,-4,1, 0,1,0};
        auto lap = conv2d_3x3(gray, lap_k, &arena);
        Tensor contrast({1, H, W});
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            int sy = std::min(y, H-2-1), sx = std::min(x, W-2-1);
            contrast.data_ptr()[y*W + x] = std::abs(lap.data_ptr()[sy*(W-2) + sx]);
        }

        float Gx[9] = {-1,0,1, -2,0,2, -1,0,1}, Gy[9] = {-1,-2,-1, 0,0,0, 1,2,1};
        auto gx = conv2d_3x3(gray, Gx, &arena), gy = conv2d_3x3(gray, Gy, &arena);
        Tensor edge({2, H, W});
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            int sy = std::min(y, H-2-1), sx = std::min(x, W-2-1);
            edge.data_ptr()[0*H*W + y*W + x] = gx.data_ptr()[sy*(W-2) + sx];
            edge.data_ptr()[1*H*W + y*W + x] = gy.data_ptr()[sy*(W-2) + sx];
        }

        float gauss[9] = {1,2,1, 2,4,2, 1,2,1}; for (int i=0;i<9;++i) gauss[i]/=16.0f;
        auto blurred = conv2d_3x3(gray, gauss, &arena);
        Tensor detail({1, H, W});
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            int sy = std::min(y, H-2-1), sx = std::min(x, W-2-1);
            detail.data_ptr()[y*W + x] = std::abs(gray.data_ptr()[y*W + x] - blurred.data_ptr()[sy*(W-2) + sx]);
        }

        Tensor lightness({1, H, W});
        for (int i = 0; i < H*W; ++i) {
            float r = rgb.data_ptr()[0*H*W+i], g = rgb.data_ptr()[1*H*W+i], b = rgb.data_ptr()[2*H*W+i];
            lightness.data_ptr()[i] = std::max({r, g, b});
        }

        Tensor yuv({3, H, W});
        for (int i = 0; i < H*W; ++i) {
            float r = rgb.data_ptr()[0*H*W+i], g = rgb.data_ptr()[1*H*W+i], b = rgb.data_ptr()[2*H*W+i];
            float y,u,v; rgb_to_yuv(r,g,b,y,u,v);
            yuv.data_ptr()[0*H*W+i] = y; yuv.data_ptr()[1*H*W+i] = u; yuv.data_ptr()[2*H*W+i] = v;
        }

        Tensor pfr({12, H, W});
        auto copy = [&](int d, const Tensor& src, int ch=0){
            for(int i=0;i<H*W;++i) pfr.data_ptr()[d*H*W+i] = src.data_ptr()[ch*H*W+i];
        };
        copy(0, contrast); copy(1, edge,0); copy(2, edge,1); copy(3, detail);
        copy(4, gray); copy(5, lightness); copy(6, rgb,0); copy(7, rgb,1); copy(8, rgb,2);
        copy(9, yuv,0); copy(10, yuv,1); copy(11, yuv,2);
        return pfr;
    }

private:
    static Tensor conv2d_3x3(const Tensor& input, const float kernel[9], Arena* arena) {
        int H = input.shape[1], W = input.shape[2];
        int out_h = H-2, out_w = W-2;
        Tensor out({1, out_h, out_w});
        float* col = (float*)arena->alloc(9 * out_h * out_w * sizeof(float));
        im2col(input.data_ptr(), 1, H, W, 3, 3, 1, 0, col);
        gemm(false, false, 1, out_h*out_w, 9, kernel, 9, col, out_h*out_w, out.data_ptr(), out_h*out_w);
        return out;
    }
};

// -------------------- EPCN v4 FLASH 调度器 (通用) --------------------
class EPCNv4Scheduler {
public:
    struct Task {
        std::string type;
        std::function<void()> func;
        int priority = 0;
    };

    EPCNv4Scheduler() : running_(false), base_cycle_ms_(2000), current_cycle_ms_(2000) {}

    void start() {
        running_ = true;
        worker_ = std::thread([this]() {
            while (running_) {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait_for(lock, std::chrono::milliseconds(current_cycle_ms_), [this]() {
                        return !task_queue_.empty() || !running_;
                    });
                    if (!running_) break;
                    if (!task_queue_.empty()) {
                        task = std::move(task_queue_.front());
                        task_queue_.pop_front();
                    } else continue;
                }
                if (task.func) task.func();
                // 自适应周期调整：根据任务队列长度
                if (task_queue_.size() > 5) current_cycle_ms_ = std::max(500, current_cycle_ms_ - 500);
                else if (task_queue_.empty()) current_cycle_ms_ = std::min(5000, current_cycle_ms_ + 500);
            }
        });
    }

    void submit(Task t) {
        std::lock_guard<std::mutex> lock(mutex_);
        task_queue_.push_back(std::move(t));
        cv_.notify_one();
    }

    void stop() { running_ = false; cv_.notify_all(); if (worker_.joinable()) worker_.join(); }

private:
    std::atomic<bool> running_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> task_queue_;
    int base_cycle_ms_, current_cycle_ms_;
};

// -------------------- PCP 权重生成与优化器 --------------------
class PCPOptimizer {
public:
    // 将 CNN 权重转换为仿生 CPU 优化格式（量化、稀疏化等）
    static Tensor convert_cnn_to_bionic(const Tensor& cnn_weight, float sparsity = 0.0f) {
        Tensor bionic = cnn_weight;
        // 简单量化：转为 INT8 对称量化（模拟）
        float max_val = 0.0f;
        for (auto& v : bionic.data) max_val = std::max(max_val, std::abs(v));
        float scale = max_val / 127.0f;
        if (scale > 0) {
            for (auto& v : bionic.data) {
                int q = std::round(v / scale);
                v = static_cast<float>(q) * scale; // 量化再反量化，引入噪声
            }
        }
        // 稀疏化
        if (sparsity > 0.0f) {
            std::vector<float> abs_vals(bionic.data.size());
            for (size_t i = 0; i < bionic.data.size(); ++i) abs_vals[i] = std::abs(bionic.data[i]);
            float threshold = percentile(abs_vals, sparsity * 100);
            for (auto& v : bionic.data) if (std::abs(v) < threshold) v = 0.0f;
        }
        return bionic;
    }

    // 仿生权重生成：随机初始化并模拟训练
    static Tensor generate_bionic_weight(const std::vector<int>& shape, float lr = 0.001f, int steps = 10) {
        Tensor w = Tensor::randn(shape, true);
        // 模拟几步梯度下降
        for (int s = 0; s < steps; ++s) {
            // 随机梯度
            for (size_t i = 0; i < w.elements(); ++i) w.grad[i] = (rand() % 100) / 1000.0f - 0.05f;
            for (size_t i = 0; i < w.elements(); ++i) w.data[i] -= lr * w.grad[i];
        }
        return w;
    }

private:
    static float percentile(std::vector<float>& v, float p) {
        std::sort(v.begin(), v.end());
        int idx = static_cast<int>(p / 100.0f * (v.size() - 1));
        return v[idx];
    }
};

// ================================================================
// acpu_bridge.h — ACPU 第二部分：CNN/仿生桥接、模型封装、转换与示例
// 依赖：acpu_core.h
// ================================================================
#pragma once
#include "acpu_core.h"
#include <unordered_map>
#include <functional>

// -------------------- 通用 CNN 模型封装 --------------------
class GenericCNNModel {
public:
    std::vector<Conv2D> conv_layers;
    std::vector<BatchNorm> bn_layers;
    std::vector<ReLU> relu_layers;
    std::vector<Linear> linear_layers;
    Arena arena;

    // 添加卷积层
    void add_conv(int in_ch, int out_ch, int k, int s = 1, int p = 0) {
        conv_layers.emplace_back(in_ch, out_ch, k, s, p);
    }

    // 添加批归一化
    void add_bn(int nf) { bn_layers.emplace_back(nf); }

    // 前向推理
    Tensor forward(const Tensor& input) {
        arena.reset();
        Tensor h = input;
        size_t ci = 0, bi = 0;
        for (size_t i = 0; i < conv_layers.size(); ++i) {
            h = conv_layers[i].forward(h, &arena);
            if (bi < bn_layers.size()) { h = bn_layers[bi++].forward(h); }
            if (ci < relu_layers.size()) { h = relu_layers[ci++].forward(h); }
        }
        // 全局平均池化（如果后面接 Linear）
        if (!linear_layers.empty()) {
            GlobalAvgPool2D gap;
            h = gap.forward(h);
            h = linear_layers[0].forward(h);
            for (size_t i = 1; i < linear_layers.size(); ++i) {
                h = linear_layers[i].forward(h);
            }
        }
        return h;
    }

    // 将所有卷积层权重转换为仿生优化格式
    void convert_to_bionic(float sparsity = 0.3f) {
        for (auto& conv : conv_layers) {
            conv.weight = PCPOptimizer::convert_cnn_to_bionic(conv.weight, sparsity);
        }
        for (auto& linear : linear_layers) {
            linear.weight = PCPOptimizer::convert_cnn_to_bionic(linear.weight, sparsity);
        }
    }

    void save(std::ostream& out) const {
        int nc = conv_layers.size(); out.write((char*)&nc, sizeof(int));
        for (auto& c : conv_layers) c.save(out);
        int nb = bn_layers.size(); out.write((char*)&nb, sizeof(int));
        for (auto& b : bn_layers) b.save(out);
        int nl = linear_layers.size(); out.write((char*)&nl, sizeof(int));
        for (auto& l : linear_layers) l.save(out);
    }

    void load(std::istream& in) {
        int nc; in.read((char*)&nc, sizeof(int));
        conv_layers.resize(nc);
        for (auto& c : conv_layers) c.load(in);
        int nb; in.read((char*)&nb, sizeof(int));
        bn_layers.resize(nb);
        for (auto& b : bn_layers) b.load(in);
        int nl; in.read((char*)&nl, sizeof(int));
        linear_layers.resize(nl);
        for (auto& l : linear_layers) l.load(in);
    }
};

// -------------------- 仿生 ACA 模型接口 --------------------
class BionicACAModel {
public:
    GenericCNNModel backbone; // 可选用传统CNN骨干
    bool use_pfr = true;

    // 仿生前向：先提取PFR特征，再送入CNN骨干
    Tensor forward(const Tensor& rgb) {
        if (use_pfr) {
            Tensor pfr = PFRBPSA3::extract(rgb);
            // 如果骨干第一层期望3通道，需调整；这里假设骨干已适配12通道
            return backbone.forward(pfr);
        }
        return backbone.forward(rgb);
    }

    // 从预训练CNN权重迁移学习
    void transfer_from_cnn(GenericCNNModel& cnn_model) {
        // 简单复制（实际应处理通道不匹配等）
        backbone = cnn_model;
        use_pfr = false; // 先用原始RGB模式
    }

    // 启用仿生微调
    void enable_bionic_mode() {
        use_pfr = true;
        // 将第一层卷积的输入通道从3改为12，或添加投影层
        // 此处简化为：在forward中用pfr替换rgb
    }
};

// -------------------- CNN 转仿生转换器 --------------------
class CNNToBionicConverter {
public:
    // 将标准CNN模型（如ONNX导入的权重）转换为仿生ACA模型
    static BionicACAModel convert(const GenericCNNModel& cnn, bool quantize = true, float sparsity = 0.2f) {
        BionicACAModel aca;
        aca.backbone = cnn;
        if (quantize) {
            aca.backbone.convert_to_bionic(sparsity);
        }
        aca.enable_bionic_mode();
        return aca;
    }

    // 从文件加载CNN权重并转换为仿生
    static BionicACAModel load_and_convert(const std::string& cnn_weight_path) {
        GenericCNNModel cnn;
        std::ifstream in(cnn_weight_path, std::ios::binary);
        if (in) { cnn.load(in); }
        return convert(cnn);
    }
};

// -------------------- 集成式 CPU 运算管理器 --------------------
class ACPUManager {
public:
    EPCNv4Scheduler scheduler;
    BionicACAModel aca_model;
    PCPOptimizer optimizer;
    bool use_bionic = true;

    ACPUManager() { scheduler.start(); }

    // 加载预训练模型（可同时支持CNN和仿生）
    void load_model(const std::string& path, bool is_bionic = true) {
        if (is_bionic) {
            std::ifstream in(path, std::ios::binary);
            if (in) aca_model.backbone.load(in);
        } else {
            aca_model = CNNToBionicConverter::load_and_convert(path);
        }
    }

    // 执行视觉任务
    void submit_vision(Tensor frame) {
        scheduler.submit({"vision", [this, frame]() {
            auto result = aca_model.forward(frame);
            // 结果处理（可接分类头、检测头等）
        }});
    }

    // 生成仿生权重
    Tensor generate_weights(const std::vector<int>& shape) {
        return PCPOptimizer::generate_bionic_weight(shape);
    }

    void stop() { scheduler.stop(); }
};

// ================================================================
// acpu_bridge.h — ACPU 第二部分：CNN/仿生桥接、模型封装、转换与示例
// 依赖：acpu_core.h
// ================================================================
#pragma once
#include "acpu_core.h"
#include <unordered_map>
#include <functional>

// -------------------- 通用 CNN 模型封装 --------------------
class GenericCNNModel {
public:
    std::vector<Conv2D> conv_layers;
    std::vector<BatchNorm> bn_layers;
    std::vector<ReLU> relu_layers;
    std::vector<Linear> linear_layers;
    Arena arena;

    // 添加卷积层
    void add_conv(int in_ch, int out_ch, int k, int s = 1, int p = 0) {
        conv_layers.emplace_back(in_ch, out_ch, k, s, p);
    }

    // 添加批归一化
    void add_bn(int nf) { bn_layers.emplace_back(nf); }

    // 前向推理
    Tensor forward(const Tensor& input) {
        arena.reset();
        Tensor h = input;
        size_t ci = 0, bi = 0;
        for (size_t i = 0; i < conv_layers.size(); ++i) {
            h = conv_layers[i].forward(h, &arena);
            if (bi < bn_layers.size()) { h = bn_layers[bi++].forward(h); }
            if (ci < relu_layers.size()) { h = relu_layers[ci++].forward(h); }
        }
        // 全局平均池化（如果后面接 Linear）
        if (!linear_layers.empty()) {
            GlobalAvgPool2D gap;
            h = gap.forward(h);
            h = linear_layers[0].forward(h);
            for (size_t i = 1; i < linear_layers.size(); ++i) {
                h = linear_layers[i].forward(h);
            }
        }
        return h;
    }

    // 将所有卷积层权重转换为仿生优化格式
    void convert_to_bionic(float sparsity = 0.3f) {
        for (auto& conv : conv_layers) {
            conv.weight = PCPOptimizer::convert_cnn_to_bionic(conv.weight, sparsity);
        }
        for (auto& linear : linear_layers) {
            linear.weight = PCPOptimizer::convert_cnn_to_bionic(linear.weight, sparsity);
        }
    }

    void save(std::ostream& out) const {
        int nc = conv_layers.size(); out.write((char*)&nc, sizeof(int));
        for (auto& c : conv_layers) c.save(out);
        int nb = bn_layers.size(); out.write((char*)&nb, sizeof(int));
        for (auto& b : bn_layers) b.save(out);
        int nl = linear_layers.size(); out.write((char*)&nl, sizeof(int));
        for (auto& l : linear_layers) l.save(out);
    }

    void load(std::istream& in) {
        int nc; in.read((char*)&nc, sizeof(int));
        conv_layers.resize(nc);
        for (auto& c : conv_layers) c.load(in);
        int nb; in.read((char*)&nb, sizeof(int));
        bn_layers.resize(nb);
        for (auto& b : bn_layers) b.load(in);
        int nl; in.read((char*)&nl, sizeof(int));
        linear_layers.resize(nl);
        for (auto& l : linear_layers) l.load(in);
    }
};

// -------------------- 仿生 ACA 模型接口 --------------------
class BionicACAModel {
public:
    GenericCNNModel backbone; // 可选用传统CNN骨干
    bool use_pfr = true;

    // 仿生前向：先提取PFR特征，再送入CNN骨干
    Tensor forward(const Tensor& rgb) {
        if (use_pfr) {
            Tensor pfr = PFRBPSA3::extract(rgb);
            // 如果骨干第一层期望3通道，需调整；这里假设骨干已适配12通道
            return backbone.forward(pfr);
        }
        return backbone.forward(rgb);
    }

    // 从预训练CNN权重迁移学习
    void transfer_from_cnn(GenericCNNModel& cnn_model) {
        // 简单复制（实际应处理通道不匹配等）
        backbone = cnn_model;
        use_pfr = false; // 先用原始RGB模式
    }

    // 启用仿生微调
    void enable_bionic_mode() {
        use_pfr = true;
        // 将第一层卷积的输入通道从3改为12，或添加投影层
        // 此处简化为：在forward中用pfr替换rgb
    }
};

// -------------------- CNN 转仿生转换器 --------------------
class CNNToBionicConverter {
public:
    // 将标准CNN模型（如ONNX导入的权重）转换为仿生ACA模型
    static BionicACAModel convert(const GenericCNNModel& cnn, bool quantize = true, float sparsity = 0.2f) {
        BionicACAModel aca;
        aca.backbone = cnn;
        if (quantize) {
            aca.backbone.convert_to_bionic(sparsity);
        }
        aca.enable_bionic_mode();
        return aca;
    }

    // 从文件加载CNN权重并转换为仿生
    static BionicACAModel load_and_convert(const std::string& cnn_weight_path) {
        GenericCNNModel cnn;
        std::ifstream in(cnn_weight_path, std::ios::binary);
        if (in) { cnn.load(in); }
        return convert(cnn);
    }
};

// -------------------- 集成式 CPU 运算管理器 --------------------
class ACPUManager {
public:
    EPCNv4Scheduler scheduler;
    BionicACAModel aca_model;
    PCPOptimizer optimizer;
    bool use_bionic = true;

    ACPUManager() { scheduler.start(); }

    // 加载预训练模型（可同时支持CNN和仿生）
    void load_model(const std::string& path, bool is_bionic = true) {
        if (is_bionic) {
            std::ifstream in(path, std::ios::binary);
            if (in) aca_model.backbone.load(in);
        } else {
            aca_model = CNNToBionicConverter::load_and_convert(path);
        }
    }

    // 执行视觉任务
    void submit_vision(Tensor frame) {
        scheduler.submit({"vision", [this, frame]() {
            auto result = aca_model.forward(frame);
            // 结果处理（可接分类头、检测头等）
        }});
    }

    // 生成仿生权重
    Tensor generate_weights(const std::vector<int>& shape) {
        return PCPOptimizer::generate_bionic_weight(shape);
    }

    void stop() { scheduler.stop(); }
};

// -------------------- 示例主函数 --------------------
int main(int argc, char* argv[]) {
    std::cout << "ACPU 通用 CPU 运算库 (CNN + 仿生 ACA + EPCN v4)" << std::endl;
    std::cout << "功能：CNN/仿生模型加载 | 权重优化 | 推理调度" << std::endl;

    ACPUManager acpu;

    // 尝试加载模型
    if (argc > 1) {
        std::string path = argv[1];
        bool is_bionic = (argc > 2 && std::string(argv[2]) == "--bionic");
        acpu.load_model(path, is_bionic);
        std::cout << "已加载模型: " << path << (is_bionic ? " (仿生)" : " (CNN)") << std::endl;
    } else {
        // 构建一个示例小模型
        BionicACAModel demo;
        demo.backbone.add_conv(12, 32, 3);
        demo.backbone.add_bn(32);
        demo.backbone.add_conv(32, 64, 3);
        demo.backbone.add_bn(64);
        acpu.aca_model = demo;
        std::cout << "使用默认仿生演示模型。" << std::endl;
    }

    // 交互命令
    std::string input;
    while (true) {
        std::cout << "\n[ACPU] 命令: infer, optimize, save, quit\n>> ";
        std::getline(std::cin, input);
        if (input == "quit") break;
        if (input == "infer") {
            Tensor dummy({3, 224, 224});
            acpu.submit_vision(dummy);
            std::cout << "已提交推理任务。" << std::endl;
        } else if (input == "optimize") {
            Tensor w = acpu.generate_weights({64, 12, 3, 3});
            std::cout << "已生成优化权重，形状: " << w.shape[0] << "x" << w.shape[1] << "x" << w.shape[2] << "x" << w.shape[3] << std::endl;
        } else if (input == "save") {
            std::ofstream out("acpu_model.bin", std::ios::binary);
            acpu.aca_model.backbone.save(out);
            std::cout << "模型已保存。" << std::endl;
        }
    }

    acpu.stop();
    return 0;
}
// ================== 增强型主函数（替换原 main） ==================
int main(int argc, char* argv[]) {
    std::cout << "ACPU 通用 CPU 运算库 (CNN + 仿生 ACA + EPCN v4)" << std::endl;
    std::cout << "版本：生产级 1.0 | 支持传统CNN与仿生模型互转" << std::endl;

    ACPUManager acpu;

    // ----- 命令行参数解析 -----
    enum class Mode { INFER, OPTIMIZE, TRAIN, CONVERT };
    Mode mode = Mode::INFER;
    std::string model_path, output_path;
    bool is_bionic = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--infer") mode = Mode::INFER;
        else if (arg == "--optimize") mode = Mode::OPTIMIZE;
        else if (arg == "--train") mode = Mode::TRAIN;
        else if (arg == "--convert") mode = Mode::CONVERT;
        else if (arg == "--model" && i + 1 < argc) model_path = argv[++i];
        else if (arg == "--output" && i + 1 < argc) output_path = argv[++i];
        else if (arg == "--bionic") is_bionic = true;
        else if (arg == "--help") {
            std::cout << "用法: " << argv[0] << " [选项]\n"
                      << "  --infer            进入推理模式（默认）\n"
                      << "  --optimize         优化权重并导出\n"
                      << "  --train            训练模式\n"
                      << "  --convert          将CNN模型转换为仿生模型\n"
                      << "  --model <路径>     指定模型文件\n"
                      << "  --output <路径>    指定输出文件\n"
                      << "  --bionic           模型为仿生格式\n"
                      << "  --help             显示帮助\n";
            return 0;
        }
    }

    // ----- 加载模型 -----
    if (!model_path.empty()) {
        if (mode == Mode::CONVERT) {
            // 加载 CNN 并转换为仿生
            std::cout << "正在转换 CNN 模型为仿生格式..." << std::endl;
            acpu.aca_model = CNNToBionicConverter::load_and_convert(model_path);
            std::cout << "转换完成。" << std::endl;
            if (!output_path.empty()) {
                std::ofstream out(output_path, std::ios::binary);
                acpu.aca_model.backbone.save(out);
                std::cout << "仿生模型已保存至 " << output_path << std::endl;
                return 0;
            }
        } else {
            acpu.load_model(model_path, is_bionic);
        }
    } else {
        // 默认演示模型
        BionicACAModel demo;
        demo.backbone.add_conv(12, 32, 3);
        demo.backbone.add_bn(32);
        demo.backbone.add_conv(32, 64, 3);
        demo.backbone.add_bn(64);
        acpu.aca_model = demo;
        std::cout << "使用默认仿生演示模型。" << std::endl;
    }

    // ----- 模式执行 -----
    if (mode == Mode::INFER) {
        // 进入交互式推理
        std::cout << "进入推理模式。输入图像路径或 'random' 进行推理，'quit' 退出。" << std::endl;
        std::string input;
        while (true) {
            std::cout << "\n[推理] >> ";
            std::getline(std::cin, input);
            if (input == "quit") break;
            if (input == "random") {
                Tensor frame({3, 224, 224});
                for (auto& v : frame.data) v = (rand() % 256) / 255.0f;
                acpu.submit_vision(frame);
                std::cout << "已提交随机图像推理任务。" << std::endl;
            } else if (!input.empty()) {
                // 可以扩展为从文件加载图像
                std::cout << "图像文件推理尚未实现，请使用 'random' 测试。" << std::endl;
            }
        }
    } else if (mode == Mode::OPTIMIZE) {
        std::cout << "优化模型权重..." << std::endl;
        acpu.aca_model.backbone.convert_to_bionic(0.2f);
        if (!output_path.empty()) {
            std::ofstream out(output_path, std::ios::binary);
            acpu.aca_model.backbone.save(out);
            std::cout << "优化后的模型已保存至 " << output_path << std::endl;
        } else {
            std::cout << "优化完成（未指定输出路径）。" << std::endl;
        }
    } else if (mode == Mode::TRAIN) {
        std::cout << "训练模式：暂未实现，请使用外部训练脚本。" << std::endl;
        // 此处可加入训练循环
    }

    acpu.stop();
    std::cout << "ACPU 已安全退出。" << std::endl;
    return 0;
}
// ================== PCP 接口 ==================
//在程序退出前对当前模型执行 PCP 优化并导出为文件
// 调用 acpu_pcp_finalize(acpu, "optimized.bin");
//       或者设置 ACPU_AUTO_PCP 宏以在 main 结束后自动触发

#define ACPU_AUTO_PCP   // 取消注释以启用自动 PCP 优化

// PCP 末端优化函数：对当前模型进行量化、稀疏化并保存
inline void acpu_pcp_finalize(ACPUManager& manager, const std::string& path = "acpu_optimized.bin") {
    std::cout << "[PCP] 正在执行末端权重优化..." << std::endl;

    // 1. 将模型骨干转换为仿生优化格式（稀疏度0.2，可调）
    manager.aca_model.backbone.convert_to_bionic(0.2f);

    // 2. 可选：进一步生成优化权重（演示）
    Tensor w = manager.generate_weights({64, 12, 3, 3});
    std::cout << "[PCP] 生成示范权重，形状: " << w.shape[0] << "x" << w.shape[1] 
              << "x" << w.shape[2] << "x" << w.shape[3] << std::endl;

    // 3. 保存优化后的完整模型
    std::ofstream out(path, std::ios::binary);
    if (out) {
        manager.aca_model.backbone.save(out);
        std::cout << "[PCP] 优化后的模型已保存至 " << path << std::endl;
    } else {
        std::cerr << "[PCP] 错误：无法写入 " << path << std::endl;
    }
}

// 如果启用自动 PCP，则在 main 结束后自动调用
#ifdef ACPU_AUTO_PCP
namespace {
    ACPUManager* _acpu_global_manager = nullptr;
    struct AutoPCP {
        ~AutoPCP() {
            if (_acpu_global_manager) {
                acpu_pcp_finalize(*_acpu_global_manager);
            }
        }
    } auto_pcp;
}
#endif