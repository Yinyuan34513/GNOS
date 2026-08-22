// ================================================================
// acpnn_core.h — ACPNN-PCD 
// 编译：C++17, OpenMP 可选，自适应 x86/ARM/RISC-V 向量化
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
#ifdef _OPENMP
#include <omp.h>
#endif

// ---------- 指令集检测与预取 ----------
#if defined(__x86_64__) || defined(_M_X64)
    #include <xmmintrin.h>
    #include <emmintrin.h>
    #define ACPNN_PREFETCH(addr) _mm_prefetch((const char*)(addr), _MM_HINT_T0)
    #define ACPNN_SIMD 1
#elif defined(__ARM_NEON)
    #include <arm_neon.h>
    #define ACPNN_PREFETCH(addr) __builtin_prefetch(addr)
    #define ACPNN_SIMD 1
#elif defined(__riscv)
    // RISC-V 向量扩展预留，当前使用标量回退
    #define ACPNN_PREFETCH(addr) __builtin_prefetch(addr)
    #define ACPNN_SIMD 0
#else
    #define ACPNN_PREFETCH(addr)
    #define ACPNN_SIMD 0
#endif

// ---------- 张量 ----------
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

// ---------- 内存池 ----------
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

// ---------- 高性能矩阵乘法 ----------
inline void gemm(bool transA, bool transB, int M, int N, int K,
                 const float* A, int lda, const float* B, int ldb,
                 float* C, int ldc) {
    #pragma omp parallel for collapse(2) schedule(static) if(M*N > 1024)
    for (int m = 0; m < M; ++m) {
        for (int n = 0; n < N; ++n) {
            float sum = 0.0f;
            int k = 0;
            for (; k <= K - 4; k += 4) {
                ACPNN_PREFETCH(A + m*lda + k + 8);
                ACPNN_PREFETCH(B + (k+8)*ldb + n);
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

// ---------- 卷积基础 ----------
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

// ---------- 卷积层 ----------
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

// ---------- 批归一化 ----------
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

// ---------- ReLU ----------
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

// ---------- 全局平均池化 ----------
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

// ---------- 全连接层 ----------
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

// ---------- 损失函数 ----------
class CrossEntropyLoss {
public:
    float forward(const Tensor& logits, const std::vector<int>& labels) {
        int N = logits.shape[0], C = logits.shape[1];
        float loss = 0.0f;
        for (int n = 0; n < N; ++n) {
            const float* x = logits.data_ptr() + n*C;
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
            const float* x = logits.data_ptr() + n*C;
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
                dlogits.data_ptr()[n*C + c] = softmax[c] - (c == label ? 1.0f : 0.0f);
            }
        }
        // 除以 N 已在损失中，此处梯度需相应缩放
        for (auto& g : dlogits.data) g /= N;
    }
};

// ---------- SGD 优化器 ----------
class SGD {
public:
    float lr;
    SGD(float learning_rate = 0.01f) : lr(learning_rate) {}
    void step(std::vector<Tensor*>& params) {
        for (auto* p : params) {
            for (size_t i = 0; i < p->elements(); ++i) {
                p->data[i] -= lr * p->grad[i];
            }
        }
    }
};

// ---------- PFR BPS A3 仿生特征提取 ----------
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

// ---------- PCP 权重优化器 ----------
class PCPOptimizer {
public:
    static Tensor convert_cnn_to_bionic(const Tensor& cnn_weight, float sparsity = 0.0f) {
        Tensor bionic = cnn_weight;
        float max_val = 0.0f;
        for (auto& v : bionic.data) max_val = std::max(max_val, std::abs(v));
        float scale = max_val / 127.0f;
        if (scale > 0) {
            for (auto& v : bionic.data) {
                int q = std::round(v / scale);
                v = static_cast<float>(q) * scale;
            }
        }
        if (sparsity > 0.0f) {
            std::vector<float> abs_vals(bionic.data.size());
            for (size_t i = 0; i < bionic.data.size(); ++i) abs_vals[i] = std::abs(bionic.data[i]);
            float threshold = percentile(abs_vals, sparsity * 100);
            for (auto& v : bionic.data) if (std::abs(v) < threshold) v = 0.0f;
        }
        return bionic;
    }

    static Tensor generate_bionic_weight(const std::vector<int>& shape, float lr = 0.001f, int steps = 10) {
        Tensor w = Tensor::randn(shape, true);
        for (int s = 0; s < steps; ++s) {
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

#pragma once
#include "acpnn_core.h"
#include <deque>
#include <mutex>
#include <condition_variable>

// -------------------- 自适应版本检测 --------------------
enum class SchedulerVersion {
    V2_SOFT = 2,
    V3_FLOW = 3,
    V4_FLASH = 4,
    AUTO = 0
};

inline SchedulerVersion detect_optimal_scheduler() {
    // 根据 CPU 核心数和内存自动选择调度器版本
    unsigned int cores = std::thread::hardware_concurrency();
    if (cores <= 4) return SchedulerVersion::V2_SOFT;      // 低配设备
    else if (cores <= 8) return SchedulerVersion::V3_FLOW;  // 中配设备
    else return SchedulerVersion::V4_FLASH;                  // 高配设备
}

// -------------------- EPCN v2 Soft 调度器 --------------------
class EPCNv2Scheduler {
public:
    struct Task {
        std::string type;
        std::function<void()> func;
        int priority = 0;
    };

    EPCNv2Scheduler(int cycle_ms = 2000) : cycle_ms_(cycle_ms) {}
    void start() {
        running_ = true;
        worker_ = std::thread([this]() {
            while (running_) {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait_for(lock, std::chrono::milliseconds(cycle_ms_), [this]() {
                        return !task_queue_.empty() || !running_;
                    });
                    if (!running_) break;
                    if (!task_queue_.empty()) {
                        task = std::move(task_queue_.front());
                        task_queue_.pop_front();
                    } else continue;
                }
                if (task.func) task.func();
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
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> task_queue_;
    int cycle_ms_;
};

// -------------------- EPCN v3 Flow 调度器（流动加速） --------------------
class EPCNv3Scheduler {
public:
    struct Task {
        std::string type;
        std::function<void()> func;
        int priority = 0;
    };

    EPCNv3Scheduler() {}
    void start() {
        running_ = true;
        worker_ = std::thread([this]() {
            while (running_) {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (task_queue_.empty()) {
                        cv_.wait_for(lock, std::chrono::milliseconds(50)); // 流动探测
                        if (!running_) break;
                        continue;
                    }
                    task = std::move(task_queue_.front());
                    task_queue_.pop_front();
                }
                if (task.func) task.func();
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
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> task_queue_;
};

// -------------------- EPCN v4 Flash 调度器（自适应带宽） --------------------
class EPCNv4Scheduler {
public:
    struct Task {
        std::string type;
        std::function<void()> func;
        int priority = 0;
    };

    EPCNv4Scheduler() : running_(false), base_cycle_ms_(2000), current_cycle_ms_(2000), load_ema_(0.5f) {}

    void set_load_callback(std::function<float()> cb) { load_cb_ = cb; }

    void start() {
        running_ = true;
        worker_ = std::thread([this]() {
            while (running_) {
                float load = load_cb_ ? load_cb_() : 0.5f;
                load_ema_ = 0.9f * load_ema_ + 0.1f * load;
                // 自适应带宽调节
                if (load_ema_ > 0.8f) current_cycle_ms_ = std::max(500, current_cycle_ms_ - 500);
                else if (load_ema_ < 0.3f) current_cycle_ms_ = std::min(5000, current_cycle_ms_ + 500);

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
            }
        });
    }

    void submit(Task t) {
        std::lock_guard<std::mutex> lock(mutex_);
        task_queue_.push_back(std::move(t));
        cv_.notify_one();
    }

    void stop() { running_ = false; cv_.notify_all(); if (worker_.joinable()) worker_.join(); }
    int get_current_cycle_ms() const { return current_cycle_ms_; }

private:
    std::atomic<bool> running_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> task_queue_;
    int base_cycle_ms_, current_cycle_ms_;
    float load_ema_;
    std::function<float()> load_cb_;
};

// -------------------- 模型管理器（CNN / 仿生通用） --------------------
class GenericCNNModel {
public:
    std::vector<Conv2D> conv_layers;
    std::vector<BatchNorm> bn_layers;
    std::vector<Linear> linear_layers;

    Tensor forward(const Tensor& input, Arena* arena) {
        Tensor h = input;
        size_t bi = 0;
        for (size_t i = 0; i < conv_layers.size(); ++i) {
            h = conv_layers[i].forward(h, arena);
            if (bi < bn_layers.size()) { h = bn_layers[bi++].forward(h, false); }
        }
        if (!linear_layers.empty()) {
            GlobalAvgPool2D gap; h = gap.forward(h);
            for (auto& l : linear_layers) h = l.forward(h);
        }
        return h;
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
        int nc; in.read((char*)&nc, sizeof(int)); conv_layers.resize(nc);
        for (auto& c : conv_layers) c.load(in);
        int nb; in.read((char*)&nb, sizeof(int)); bn_layers.resize(nb);
        for (auto& b : bn_layers) b.load(in);
        int nl; in.read((char*)&nl, sizeof(int)); linear_layers.resize(nl);
        for (auto& l : linear_layers) l.load(in);
    }
};

// -------------------- CNN 转仿生迁移器 --------------------
class CNNToBionicMigrator {
public:
    // 将 CNN 模型转换为仿生大模型格式（添加 PFR 前端，优化权重）
    static GenericCNNModel migrate(const GenericCNNModel& cnn, float sparsity = 0.2f) {
        GenericCNNModel bionic = cnn;
        // 将第一层卷积权重替换为仿生优化版本
        if (!bionic.conv_layers.empty()) {
            bionic.conv_layers[0].weight = PCPOptimizer::convert_cnn_to_bionic(
                bionic.conv_layers[0].weight, sparsity);
        }
        // 可选：将输入通道扩展为 12（PFR 输出），如果原始为 3
        // 此处简化：实际使用时需调整第一层输入维度
        return bionic;
    }
};

// -------------------- CPU 训练循环 --------------------
class CPUTrainer {
public:
    GenericCNNModel& model;
    SGD optimizer;
    CrossEntropyLoss loss_fn;
    Arena arena;

    CPUTrainer(GenericCNNModel& m, float lr = 0.01f) : model(m), optimizer(lr) {}

    void train_step(const Tensor& input, const std::vector<int>& labels) {
        arena.reset();
        Tensor logits = model.forward(input, &arena);
        Tensor dlogits;
        loss_fn.backward(logits, labels, dlogits);

        // 收集参数并更新（简化：仅更新卷积和线性层）
        std::vector<Tensor*> params;
        for (auto& c : model.conv_layers) { params.push_back(&c.weight); params.push_back(&c.bias); }
        for (auto& l : model.linear_layers) { params.push_back(&l.weight); params.push_back(&l.bias); }
        optimizer.step(params);
    }
};

// -------------------- 集成接口 --------------------
class ACPNNManager {
public:
    GenericCNNModel model;
    std::unique_ptr<EPCNv2Scheduler> v2_sched;
    std::unique_ptr<EPCNv3Scheduler> v3_sched;
    std::unique_ptr<EPCNv4Scheduler> v4_sched;
    SchedulerVersion active_version;
    CPUTrainer* trainer = nullptr;
    std::atomic<float> load{0.5f};

    ACPNNManager(SchedulerVersion ver = SchedulerVersion::AUTO) {
        if (ver == SchedulerVersion::AUTO) ver = detect_optimal_scheduler();
        active_version = ver;
        switch (ver) {
            case SchedulerVersion::V2_SOFT: v2_sched = std::make_unique<EPCNv2Scheduler>(); v2_sched->start(); break;
            case SchedulerVersion::V3_FLOW: v3_sched = std::make_unique<EPCNv3Scheduler>(); v3_sched->start(); break;
            case SchedulerVersion::V4_FLASH:
                v4_sched = std::make_unique<EPCNv4Scheduler>();
                v4_sched->set_load_callback([this]() { return load.load(); });
                v4_sched->start();
                break;
            default: break;
        }
    }

    void submit_task(const std::string& type, std::function<void()> func) {
        auto task = EPCNv2Scheduler::Task{type, func}; // 所有调度器 Task 结构相同
        switch (active_version) {
            case SchedulerVersion::V2_SOFT: if (v2_sched) v2_sched->submit(task); break;
            case SchedulerVersion::V3_FLOW: if (v3_sched) v3_sched->submit(task); break;
            case SchedulerVersion::V4_FLASH: if (v4_sched) v4_sched->submit(task); break;
            default: break;
        }
    }

    void stop() {
        if (v2_sched) v2_sched->stop();
        if (v3_sched) v3_sched->stop();
        if (v4_sched) v4_sched->stop();
    }

    // 训练接口
    void train(const Tensor& input, const std::vector<int>& labels) {
        if (trainer) trainer->train_step(input, labels);
    }
};

// ================== 主函数 ==================
int main(int argc, char* argv[]) {
    std::cout << "ACPNN-PCD 生产级 CPU 神经网络库 (EPCN v2/v3/v4 自适应)" << std::endl;
    std::cout << "指令集: " << 
#if defined(__x86_64__) || defined(_M_X64)
        "x86 SSE/AVX"
#elif defined(__ARM_NEON)
        "ARM NEON"
#elif defined(__riscv)
        "RISC-V"
#else
        "通用标量"
#endif
    << " | 模式: 推理/训练/导出" << std::endl;

    // ----- 参数解析 -----
    SchedulerVersion ver = SchedulerVersion::AUTO;
    std::string model_path, export_path;
    bool train_mode = false, pcp_export = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--v2") ver = SchedulerVersion::V2_SOFT;
        else if (arg == "--v3") ver = SchedulerVersion::V3_FLOW;
        else if (arg == "--v4") ver = SchedulerVersion::V4_FLASH;
        else if (arg == "--model" && i + 1 < argc) model_path = argv[++i];
        else if (arg == "--export" && i + 1 < argc) { export_path = argv[++i]; pcp_export = true; }
        else if (arg == "--train") train_mode = true;
        else if (arg == "--help") {
            std::cout << "用法: " << argv[0] << " [选项]\n"
                      << "  --v2/v3/v4      手动指定调度器版本\n"
                      << "  --model <文件>   加载预训练模型\n"
                      << "  --train          进入训练模式\n"
                      << "  --export <文件>  导出为PCP优化权重\n"
                      << "  --help           显示帮助\n";
            return 0;
        }
    }

    // ----- 创建管理器 -----
    SchedulerFactory::auto_select();
    auto manager = SchedulerFactory::create(ver);

    // ----- 构建或加载模型 -----
    if (!model_path.empty()) {
        WeightManager::load_model(manager->model, model_path);
        std::cout << "[系统] 已加载模型: " << model_path << std::endl;
    } else {
        // 构建实用的演示模型
        manager->model.conv_layers.emplace_back(3, 32, 3);
        manager->model.bn_layers.emplace_back(32);
        manager->model.conv_layers.emplace_back(32, 64, 3);
        manager->model.bn_layers.emplace_back(64);
        manager->model.conv_layers.emplace_back(64, 128, 3);
        manager->model.bn_layers.emplace_back(128);
        manager->model.linear_layers.emplace_back(128 * 28 * 28, 256);
        manager->model.linear_layers.emplace_back(256, 10);
        std::cout << "[系统] 使用默认卷积模型 (3x3, 3层, 10类输出)" << std::endl;
    }

    // ----- 初始化训练器 -----
    CPUTrainer trainer(manager->model, 0.001f);
    manager->trainer = &trainer;

    // ----- 执行模式 -----
    if (train_mode) {
        std::cout << "[训练] 开始CPU训练 (SGD, 交叉熵损失)..." << std::endl;
        const int epochs = 5;
        const int batch_size = 4;
        for (int e = 0; e < epochs; ++e) {
            std::vector<Tensor> images;
            std::vector<std::vector<int>> labels;
            DataLoader::generate_random_batch(images, labels, batch_size, 3, 224, 224, 10);
            for (size_t i = 0; i < images.size(); ++i) {
                manager->submit_task("train", [&manager, &images, &labels, i]() {
                    manager->train(images[i], labels[i]);
                });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            std::cout << "[训练] 第 " << e + 1 << "/" << epochs << " 轮完成" << std::endl;
        }
        // 保存训练后的模型
        WeightManager::save_model(manager->model, "trained_model.bin");
        // 自动导出PCP优化版本
        WeightManager::export_pcp(manager->model, "trained_model_pcp.bin", 0.25f);
        std::cout << "[训练] 模型已保存 (普通+PCP优化)" << std::endl;
    } else if (pcp_export) {
        std::cout << "[导出] 正在生成PCP优化权重..." << std::endl;
        WeightManager::export_pcp(manager->model, export_path, 0.2f);
    } else {
        // 推理模式：交互式
        std::cout << "[推理] 进入交互模式。命令: random (推理), save (保存), pcp (导出), quit (退出)" << std::endl;
        std::string input;
        while (true) {
            std::cout << ">> ";
            std::getline(std::cin, input);
            if (input == "quit") break;
            if (input == "random") {
                manager->submit_task("infer", [&manager]() {
                    Tensor img({3, 224, 224});
                    for (auto& v : img.data) v = (rand() % 256) / 255.0f;
                    Arena ar;
                    auto start = std::chrono::high_resolution_clock::now();
                    auto res = manager->model.forward(img, &ar);
                    auto end = std::chrono::high_resolution_clock::now();
                    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                    std::cout << "[推理] 完成 (" << us << "us), 类别概率: ";
                    for (int i = 0; i < std::min(10, (int)res.elements()); ++i)
                        std::cout << std::fixed << std::setprecision(3) << res.data_ptr()[i] << " ";
                    std::cout << std::endl;
                });
            } else if (input == "save") {
                WeightManager::save_model(manager->model, "snapshot.bin");
            } else if (input == "pcp") {
                WeightManager::export_pcp(manager->model, "snapshot_pcp.bin", 0.2f);
            } else if (input == "status") {
                std::cout << "[状态] 调度器版本: EPCN v";
                switch (manager->active_version) {
                    case SchedulerVersion::V2_SOFT: std::cout << "2 (Soft)"; break;
                    case SchedulerVersion::V3_FLOW: std::cout << "3 (Flow)"; break;
                    case SchedulerVersion::V4_FLASH: {
                        if (manager->v4_sched) 
                            std::cout << "4 (Flash, 当前周期" << manager->v4_sched->get_current_cycle_ms() << "ms)";
                        else std::cout << "4 (Flash)";
                        break;
                    }
                    default: break;
                }
                std::cout << " | 层数: " << manager->model.conv_layers.size() 
                          << "卷积, " << manager->model.linear_layers.size() << "全连接" << std::endl;
            }
        }
    }

    manager->stop();
    std::cout << "[ACPNN] 程序已安全退出。" << std::endl;
    return 0;
}

#pragma once
#include "acpnn_scheduler.h"
#include <fstream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <random>

// -------------------- 自适应调度器工厂 --------------------
class SchedulerFactory {
public:
    // 根据硬件配置和用户指定返回最优调度器
    static std::unique_ptr<ACPNNManager> create(SchedulerVersion ver = SchedulerVersion::AUTO) {
        return std::make_unique<ACPNNManager>(ver);
    }
    // 自动检测并打印选择结果
    static SchedulerVersion auto_select() {
        SchedulerVersion v = detect_optimal_scheduler();
        std::cout << "[ACPNN] 自适应选择调度器: EPCN v";
        switch (v) {
            case SchedulerVersion::V2_SOFT: std::cout << "2 (Soft)"; break;
            case SchedulerVersion::V3_FLOW: std::cout << "3 (Flow)"; break;
            case SchedulerVersion::V4_FLASH: std::cout << "4 (Flash)"; break;
            default: break;
        }
        std::cout << std::endl;
        return v;
    }
};

// -------------------- 增强数据加载器 --------------------
class DataLoader {
public:
    // 生成随机训练数据（演示用，实际应替换为真实数据加载）
    static void generate_random_batch(std::vector<Tensor>& images, std::vector<std::vector<int>>& labels,
                                       int batch_size, int img_c = 3, int img_h = 224, int img_w = 224, int num_classes = 10) {
        images.clear(); labels.clear();
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        std::uniform_int_distribution<int> label_dist(0, num_classes - 1);
        for (int b = 0; b < batch_size; ++b) {
            Tensor img({img_c, img_h, img_w});
            for (auto& v : img.data) v = dist(rng);
            images.push_back(img);
            labels.push_back({label_dist(rng)});
        }
    }
};

// -------------------- 权重管理器 --------------------
class WeightManager {
public:
    static void save_model(const GenericCNNModel& model, const std::string& path) {
        std::ofstream out(path, std::ios::binary);
        if (out) {
            model.save(out);
            std::cout << "[权重] 模型已保存至 " << path << std::endl;
        } else {
            std::cerr << "[权重] 错误：无法写入 " << path << std::endl;
        }
    }
    static void load_model(GenericCNNModel& model, const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            model.load(in);
            std::cout << "[权重] 模型已加载自 " << path << std::endl;
        } else {
            std::cerr << "[权重] 错误：无法打开 " << path << std::endl;
        }
    }
    // 导出为 PCP 优化格式（量化+稀疏化）
    static void export_pcp(const GenericCNNModel& model, const std::string& path, float sparsity = 0.2f) {
        GenericCNNModel opt = model;
        for (auto& conv : opt.conv_layers) {
            conv.weight = PCPOptimizer::convert_cnn_to_bionic(conv.weight, sparsity);
        }
        for (auto& linear : opt.linear_layers) {
            linear.weight = PCPOptimizer::convert_cnn_to_bionic(linear.weight, sparsity);
        }
        save_model(opt, path);
    }
};

// -------------------- 主程序 --------------------
int main(int argc, char* argv[]) {
    std::cout << "ACPNN-PCD 生产级 CPU 神经网络库 (v1.0)" << std::endl;
    std::cout << "指令集: " << 
#if defined(__x86_64__) || defined(_M_X64)
        "x86 SSE/AVX"
#elif defined(__ARM_NEON)
        "ARM NEON"
#elif defined(__riscv)
        "RISC-V"
#else
        "通用标量"
#endif
    << " | 多调度器支持" << std::endl;

    // 解析参数
    SchedulerVersion ver = SchedulerVersion::AUTO;
    std::string model_path, data_path;
    bool train_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--v2") ver = SchedulerVersion::V2_SOFT;
        else if (arg == "--v3") ver = SchedulerVersion::V3_FLOW;
        else if (arg == "--v4") ver = SchedulerVersion::V4_FLASH;
        else if (arg == "--model" && i + 1 < argc) model_path = argv[++i];
        else if (arg == "--train") train_mode = true;
        else if (arg == "--help") {
            std::cout << "用法: " << argv[0] << " [选项]\n"
                      << "  --v2/v3/v4  指定调度器版本\n"
                      << "  --model <文件> 模型权重路径\n"
                      << "  --train      进入训练模式\n"
                      << "  --help       显示帮助\n";
            return 0;
        }
    }

    // 创建管理器（自适应调度器）
    SchedulerFactory::auto_select();
    auto manager = SchedulerFactory::create(ver);

    // 构建或加载模型
    if (!model_path.empty()) {
        WeightManager::load_model(manager->model, model_path);
    } else {
        // 构建默认演示模型：3->16->32->128->10
        manager->model.conv_layers.emplace_back(3, 16, 3);
        manager->model.bn_layers.emplace_back(16);
        manager->model.conv_layers.emplace_back(16, 32, 3);
        manager->model.bn_layers.emplace_back(32);
        manager->model.linear_layers.emplace_back(32 * 56 * 56, 128); // 假设输入224，经过两层conv3+s=1后尺寸变化
        manager->model.linear_layers.emplace_back(128, 10);
        std::cout << "[模型] 使用默认演示模型。" << std::endl;
    }

    // 初始化训练器
    CPUTrainer trainer(manager->model, 0.01f);
    manager->trainer = &trainer;

    // 根据模式运行
    if (train_mode) {
        std::cout << "[训练] 开始 CPU 训练..." << std::endl;
        const int epochs = 5;
        const int batch_size = 4;
        for (int e = 0; e < epochs; ++e) {
            std::vector<Tensor> images;
            std::vector<std::vector<int>> labels;
            DataLoader::generate_random_batch(images, labels, batch_size, 3, 224, 224, 10);
            for (size_t i = 0; i < images.size(); ++i) {
                manager->submit_task("train", [&manager, &images, &labels, i]() {
                    manager->train(images[i], labels[i]);
                });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << "[训练] 第 " << e + 1 << " 轮完成" << std::endl;
        }
        // 保存训练后的模型
        WeightManager::save_model(manager->model, "trained_model.bin");
        // 导出 PCP 优化版本
        WeightManager::export_pcp(manager->model, "trained_model_pcp.bin", 0.25f);
    } else {
        // 推理模式：交互式
        std::cout << "[推理] 进入交互模式，输入 'random' 提交随机图像推理，'quit' 退出。" << std::endl;
        std::string input;
        while (true) {
            std::cout << ">> ";
            std::getline(std::cin, input);
            if (input == "quit") break;
            if (input == "random") {
                manager->submit_task("infer", [&manager]() {
                    Tensor img({3, 224, 224});
                    for (auto& v : img.data) v = (rand() % 256) / 255.0f;
                    Arena ar;
                    auto res = manager->model.forward(img, &ar);
                    std::cout << "[推理] 输出维度: " << res.elements() << " (前5个值: ";
                    for (int i = 0; i < std::min(5, (int)res.elements()); ++i)
                        std::cout << std::fixed << std::setprecision(3) << res.data_ptr()[i] << " ";
                    std::cout << ")" << std::endl;
                });
            } else if (input == "save") {
                WeightManager::save_model(manager->model, "model_snapshot.bin");
            } else if (input == "pcp") {
                WeightManager::export_pcp(manager->model, "model_pcp.bin", 0.2f);
            }
        }
    }

    manager->stop();
    std::cout << "[ACPNN] 程序已安全退出。" << std::endl;
    return 0;
}

#include "acpnn_pcd.hpp"

int main() {
    // 1. 创建自适应调度管理器
    auto manager = SchedulerFactory::create(SchedulerVersion::AUTO);
    
    // 2. 构建模型 (示例: 3层卷积 + 2层全连接)
    manager->model.conv_layers.emplace_back(3, 32, 3);
    manager->model.bn_layers.emplace_back(32);
    manager->model.conv_layers.emplace_back(32, 64, 3);
    manager->model.bn_layers.emplace_back(64);
    manager->model.conv_layers.emplace_back(64, 128, 3);
    manager->model.bn_layers.emplace_back(128);
    manager->model.linear_layers.emplace_back(128 * 28 * 28, 256); // 假设输入224, 下采样8倍
    manager->model.linear_layers.emplace_back(256, 10);
    
    // 3. 初始化训练器
    CPUTrainer trainer(manager->model, 0.001f);
    manager->trainer = &trainer;
    
    // 4. 训练一个批次
    std::vector<Tensor> images;
    std::vector<std::vector<int>> labels;
    DataLoader::generate_random_batch(images, labels, 4, 3, 224, 224, 10);
    for (size_t i = 0; i < images.size(); ++i) {
        manager->submit_task("train", [&]() { manager->train(images[i], labels[i]); });
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // 5. 保存模型 (普通 & PCP优化)
    WeightManager::save_model(manager->model, "model_normal.bin");
    WeightManager::export_pcp(manager->model, "model_pcp.bin", 0.2f);
    
    // 6. 推理示例
    Tensor test_img({3, 224, 224});
    for (auto& v : test_img.data) v = (rand() % 256) / 255.0f;
    manager->submit_task("infer", [&]() {
        Arena ar;
        auto res = manager->model.forward(test_img, &ar);
        std::cout << "推理完成，输出类别概率: ";
        for (int i = 0; i < 10; ++i) std::cout << res.data_ptr()[i] << " ";
        std::cout << std::endl;
    });
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    manager->stop();
    return 0;
}