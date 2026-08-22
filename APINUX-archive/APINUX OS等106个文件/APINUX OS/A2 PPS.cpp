// ================================================================
// rdc_integrated.h — 茸崽RDC集成库 (第一部分: 基础/YUV/文字识别)
// #include "rdc_integrated.h"
// 编译: g++ -std=c++17 -O3 -march=native -fopenmp your_app.cpp -o app
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
#include <sstream>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <atomic>
#include <deque>
#ifdef _OPENMP
#include <omp.h>
#endif
 
// ---------- 基础张量 ----------
class Tensor {
public:
    std::vector<int> shape;
    std::vector<float> data;
    std::vector<float> grad;
    bool requires_grad = false;
 
    Tensor() = default;
    Tensor(const std::vector<int>& s, bool req = false);
    size_t elements() const { return data.size(); }
    float* data_ptr() { return data.data(); }
    const float* data_ptr() const { return data.data(); }
    float* grad_ptr() { return grad.data(); }
    void zero_grad() { if(requires_grad) std::fill(grad.begin(), grad.end(), 0.0f); }
    static Tensor zeros(const std::vector<int>& s, bool req=false);
    static Tensor ones(const std::vector<int>& s, bool req=false);
    static Tensor randn(const std::vector<int>& s, bool req=false);
    void save(std::ostream& out) const;
    void load(std::istream& in);
};
 
// ---------- 内存池 ----------
class Arena {
    std::vector<char> pool;
    size_t used = 0;
    static constexpr size_t DEFAULT_SIZE = 128 * 1024 * 1024;
public:
    void* alloc(size_t bytes) {
        bytes = (bytes + 63) & ~63;
        if(used+bytes > pool.size()) pool.resize(std::max(pool.size()*2, used+bytes));
        void* p = pool.data() + used; used += bytes; return p;
    }
    void reset() { used = 0; }
};
 
// ---------- 数学原语 ----------
inline void gemm(bool transA, bool transB, int M, int N, int K,
                 const float* A, int lda, const float* B, int ldb,
                 float* C, int ldc) {
    #pragma omp parallel for collapse(2) if(M*N > 1024)
    for(int m=0; m<M; ++m)
        for(int n=0; n<N; ++n) {
            float sum=0.0f;
            for(int k=0; k<K; ++k) {
                float a = transA ? A[k*lda+m] : A[m*lda+k];
                float b = transB ? B[n*ldb+k] : B[k*ldb+n];
                sum += a*b;
            }
            C[m*ldc+n] = sum;
        }
}
 
// ---------- 上采样 ----------
inline Tensor upsample2x(const Tensor& feat) {
    int C=feat.shape[0], H=feat.shape[1], W=feat.shape[2];
    Tensor up({C, H*2, W*2});
    for(int c=0; c<C; ++c)
        for(int h=0; h<H; ++h)
            for(int w=0; w<W; ++w) {
                float v = feat.data_ptr()[c*H*W + h*W + w];
                up.data_ptr()[c*H*2*W*2 + (h*2)*W*2 + (w*2)] = v;
                up.data_ptr()[c*H*2*W*2 + (h*2)*W*2 + (w*2)+1] = v;
                up.data_ptr()[c*H*2*W*2 + (h*2+1)*W*2 + (w*2)] = v;
                up.data_ptr()[c*H*2*W*2 + (h*2+1)*W*2 + (w*2)+1] = v;
            }
    return up;
}
 
inline Tensor resize_bilinear(const Tensor& feat, int target_h, int target_w) {
    int C=feat.shape[0], H=feat.shape[1], W=feat.shape[2];
    Tensor out({C, target_h, target_w});
    float sh=(float)H/target_h, sw=(float)W/target_w;
    for(int c=0; c<C; ++c)
        for(int y=0; y<target_h; ++y) {
            float src_y = y*sh;
            int y0=(int)src_y, y1=std::min(y0+1, H-1);
            float dy = src_y - y0;
            for(int x=0; x<target_w; ++x) {
                float src_x = x*sw;
                int x0=(int)src_x, x1=std::min(x0+1, W-1);
                float dx = src_x - x0;
                float v00 = feat.data_ptr()[c*H*W + y0*W + x0];
                float v01 = feat.data_ptr()[c*H*W + y0*W + x1];
                float v10 = feat.data_ptr()[c*H*W + y1*W + x0];
                float v11 = feat.data_ptr()[c*H*W + y1*W + x1];
                out.data_ptr()[c*target_h*target_w + y*target_w + x] =
                    (v00*(1-dx)+v01*dx)*(1-dy) + (v10*(1-dx)+v11*dx)*dy;
            }
        }
    return out;
}
 
// ================== YUV 转换与轻量化存储 ==================
inline void rgb_to_yuv(float r, float g, float b, float& y, float& u, float& v) {
    y =  0.299f * r + 0.587f * g + 0.114f * b;
    u = -0.14713f * r - 0.28886f * g + 0.436f * b;
    v =  0.615f * r - 0.51499f * g - 0.10001f * b;
}
 
// YUV 轻量化存储：只保留关键 Y 通道和降采样的 U/V，用于快速检索
struct YUVLightRecord {
    float y_mean;          // 平均亮度
    float u_mean, v_mean;  // 平均色度
    std::vector<float> y_hist;  // 亮度直方图 (16 bins)
    int64_t timestamp;
    void save(std::ostream& out) const {
        out.write((char*)&y_mean, sizeof(float));
        out.write((char*)&u_mean, sizeof(float));
        out.write((char*)&v_mean, sizeof(float));
        int sz = y_hist.size(); out.write((char*)&sz, sizeof(int));
        out.write((char*)y_hist.data(), sz*sizeof(float));
        out.write((char*)×tamp, sizeof(int64_t));
    }
    void load(std::istream& in) {
        in.read((char*)&y_mean, sizeof(float));
        in.read((char*)&u_mean, sizeof(float));
        in.read((char*)&v_mean, sizeof(float));
        int sz; in.read((char*)&sz, sizeof(int));
        y_hist.resize(sz);
        in.read((char*)y_hist.data(), sz*sizeof(float));
        in.read((char*)×tamp, sizeof(int64_t));
    }
};
 
class YUVLightStore {
    std::vector<YUVLightRecord> records;
public:
    void add_frame(const Tensor& rgb) {
        if(rgb.shape[0]!=3) return;
        int H=rgb.shape[1], W=rgb.shape[2];
        YUVLightRecord rec;
        double sy=0, su=0, sv=0;
        std::vector<int> hist(16,0);
        for(int i=0; i<H*W; ++i) {
            float r=rgb.data_ptr()[0*H*W+i], g=rgb.data_ptr()[1*H*W+i], b=rgb.data_ptr()[2*H*W+i];
            float y,u,v;
            rgb_to_yuv(r,g,b,y,u,v);
            sy+=y; su+=u; sv+=v;
            int bin = std::min(15, (int)(y*16));
            hist[bin]++;
        }
        rec.y_mean = sy/(H*W);
        rec.u_mean = su/(H*W);
        rec.v_mean = sv/(H*W);
        rec.y_hist.resize(16);
        for(int i=0;i<16;++i) rec.y_hist[i] = hist[i]/(float)(H*W);
        rec.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        records.push_back(rec);
    }
    const std::vector<YUVLightRecord>& get_records() const { return records; }
    void save(std::ostream& out) const {
        int sz = records.size(); out.write((char*)&sz, sizeof(int));
        for(auto& r : records) r.save(out);
    }
    void load(std::istream& in) {
        int sz; in.read((char*)&sz, sizeof(int));
        records.resize(sz);
        for(int i=0;i<sz;++i) records[i].load(in);
    }
};
 
// ================== 文字识别模块 (轻量OCR) ==================
// 基于仿生特征 + 线性分类器，可识别26个英文字母+10个数字（共36类）
class TextRecognizer {
public:
    static constexpr int NUM_CLASSES = 36;
    // 输入特征：使用PFR BPS的12通道全局平均池化后接线性层
    Linear fc;  // 12 -> 36
    TextRecognizer() : fc(12, NUM_CLASSES) {}
    Tensor forward(const Tensor& pfr_features) {
        // pfr_features: [12, H, W]
        int H=pfr_features.shape[1], W=pfr_features.shape[2];
        Tensor pool({12,1,1});
        for(int c=0;c<12;++c) {
            float sum=0;
            for(int i=0;i<H*W;++i) sum+=pfr_features.data_ptr()[c*H*W+i];
            pool.data_ptr()[c] = sum/(H*W);
        }
        Tensor flat({12});
        for(int c=0;c<12;++c) flat.data_ptr()[c]=pool.data_ptr()[c];
        return fc.forward(flat);
    }
    int predict_char(const Tensor& pfr_features) {
        Tensor logits = forward(pfr_features);
        return std::max_element(logits.data.begin(), logits.data.end()) - logits.data.begin();
    }
    // 预生成起始权重 (随机初始化，但提供加载)
    void init_pretrained() {
        // 使用固定的随机种子生成可复现的起始权重
        std::mt19937 rng(42);
        std::normal_distribution<float> nd(0.0f, 0.02f);
        for(auto& v : fc.weight.data) v = nd(rng);
        for(auto& v : fc.bias.data) v = 0.0f;
    }
    void save(std::ostream& out) const { fc.save(out); }
    void load(std::istream& in) { fc.load(in); }
};
 
// ================== 基础CNN层 (仅保留推理所需) ==================
class Conv2D {
public:
    int in_ch, out_ch, kh, kw, stride, pad;
    Tensor weight, bias;
    Conv2D(int in_c, int out_c, int k, int s=1, int p=0);
    Tensor forward(const Tensor& input, Arena* arena);
    void save(std::ostream& out) const;
    void load(std::istream& in);
    // 反向传播省略（本库专注推理和加载）
};
class BatchNorm {
public:
    int nf;
    Tensor gamma, beta, running_mean, running_var;
    BatchNorm(int nf);
    Tensor forward(const Tensor& x);
    void save(std::ostream& out) const;
    void load(std::istream& in);
};
class ReLU {
public:
    Tensor forward(const Tensor& x);
};
class GlobalAvgPool2D {
public:
    Tensor forward(const Tensor& x);
};
// ================================================================
// rdc_integrated_part2.h — 物体识别、EPCN加载器、集成API、主函数
// ================================================================
#pragma once
#include "rdc_integrated.h"
#include <deque>
#include <sys/stat.h>
 
// ================== 基础CNN层完整实现 ==================
inline void im2col(const float* im, int C, int H, int W,
                   int KH, int KW, int stride, int pad, float* col) {
    int out_h = (H + 2*pad - KH) / stride + 1;
    int out_w = (W + 2*pad - KW) / stride + 1;
    #pragma omp parallel for collapse(2)
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
 
inline Conv2D::Conv2D(int in_c, int out_c, int k, int s, int p)
    : in_ch(in_c), out_ch(out_c), kh(k), kw(k), stride(s), pad(p) {
    weight = Tensor::randn({out_c, in_c, k, k}, false);
    bias   = Tensor::zeros({out_c}, false);
}
 
inline Tensor Conv2D::forward(const Tensor& input, Arena* arena) {
    int H = input.shape[1], W = input.shape[2];
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
    return out;
}
inline void Conv2D::save(std::ostream& out) const { weight.save(out); bias.save(out); }
inline void Conv2D::load(std::istream& in) { weight.load(in); bias.load(in); }
 
inline BatchNorm::BatchNorm(int nf) : nf(nf),
    gamma(Tensor::ones({nf}, false)), beta(Tensor::zeros({nf}, false)),
    running_mean(Tensor::zeros({nf})), running_var(Tensor::ones({nf})) {}
 
inline Tensor BatchNorm::forward(const Tensor& x) {
    int C = nf, HW = x.shape[1]*x.shape[2];
    Tensor out(x.shape);
    for (int c = 0; c < C; ++c) {
        float mean = running_mean.data_ptr()[c];
        float var  = running_var.data_ptr()[c];
        float inv_std = 1.0f / std::sqrt(var + 1e-5f);
        for (int i = 0; i < HW; ++i) {
            float xc = x.data_ptr()[c*HW + i] - mean;
            out.data_ptr()[c*HW + i] = gamma.data_ptr()[c] * xc * inv_std + beta.data_ptr()[c];
        }
    }
    return out;
}
inline void BatchNorm::save(std::ostream& out) const { gamma.save(out); beta.save(out); running_mean.save(out); running_var.save(out); }
inline void BatchNorm::load(std::istream& in) { gamma.load(in); beta.load(in); running_mean.load(in); running_var.load(in); }
 
inline Tensor ReLU::forward(const Tensor& x) {
    Tensor out(x.shape);
    for (size_t i = 0; i < x.elements(); ++i)
        out.data_ptr()[i] = std::max(0.0f, x.data_ptr()[i]);
    return out;
}
 
inline Tensor GlobalAvgPool2D::forward(const Tensor& x) {
    int C = x.shape[0], H = x.shape[1], W = x.shape[2];
    Tensor out({C, 1, 1});
    for (int c = 0; c < C; ++c) {
        float sum = 0.0f;
        for (int i = 0; i < H*W; ++i) sum += x.data_ptr()[c*H*W + i];
        out.data_ptr()[c] = sum / (H*W);
    }
    return out;
}
 
// ================== 物体识别模块（轻量检测头） ==================
class ObjectDetector {
public:
    static constexpr int MAX_CLASSES = 80;
    // 基于PFR特征的简单分类 + 回归头
    Conv2D cls_conv;    // 12 -> MAX_CLASSES (1x1)
    Conv2D reg_conv;    // 12 -> 4 (bbox, 1x1)
    ObjectDetector() : cls_conv(12, MAX_CLASSES, 1), reg_conv(12, 4, 1) {}
     
    struct Detection {
        int class_id;
        float confidence;
        float x, y, w, h; // 归一化坐标
    };
 
    // 输入PFR特征 [12, H, W]，返回检测列表
    std::vector<Detection> forward(const Tensor& pfr, float thresh = 0.5f) {
        thread_local Arena arena;
        arena.reset();
        Tensor cls_out = cls_conv.forward(pfr, &arena);  // [80, H, W]
        Tensor reg_out = reg_conv.forward(pfr, &arena);  // [4, H, W]
        int H = cls_out.shape[1], W = cls_out.shape[2];
        std::vector<Detection> dets;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                // 找最大类别
                float max_conf = -1e9f;
                int best_cls = 0;
                for (int c = 0; c < MAX_CLASSES; ++c) {
                    float conf = cls_out.data_ptr()[c*H*W + y*W + x];
                    if (conf > max_conf) { max_conf = conf; best_cls = c; }
                }
                if (max_conf > thresh) {
                    Detection det;
                    det.class_id = best_cls;
                    det.confidence = max_conf;
                    det.x = reg_out.data_ptr()[0*H*W + y*W + x];
                    det.y = reg_out.data_ptr()[1*H*W + y*W + x];
                    det.w = reg_out.data_ptr()[2*H*W + y*W + x];
                    det.h = reg_out.data_ptr()[3*H*W + y*W + x];
                    dets.push_back(det);
                }
            }
        }
        return dets;
    }
     
    void init_pretrained() {
        std::mt19937 rng(42);
        std::normal_distribution<float> nd(0.0f, 0.02f);
        for (auto& v : cls_conv.weight.data) v = nd(rng);
        for (auto& v : cls_conv.bias.data) v = 0.0f;
        for (auto& v : reg_conv.weight.data) v = nd(rng);
        for (auto& v : reg_conv.bias.data) v = 0.0f;
    }
    void save(std::ostream& out) const { cls_conv.save(out); reg_conv.save(out); }
    void load(std::istream& in) { cls_conv.load(in); reg_conv.load(in); }
};
 
// ================== EPCN 加载优化器 ==================
class EPCNLoader {
public:
    struct ModelEntry {
        std::string name;
        std::string path;
        int version = 1;
        std::chrono::system_clock::time_point loaded_time;
    };
    std::deque<ModelEntry> history;
    int max_history = 5;
 
    // 加载权重文件，自动版本管理
    bool load_weights(const std::string& name, const std::string& path, 
                      std::function<bool(std::istream&)> loader) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::cerr << "[EPCN] 无法打开权重文件: " << path << std::endl;
            return false;
        }
        if (!loader(in)) {
            std::cerr << "[EPCN] 权重加载失败: " << path << std::endl;
            return false;
        }
        ModelEntry entry{name, path, 1, std::chrono::system_clock::now()};
        history.push_back(entry);
        if (history.size() > max_history) history.pop_front();
        std::cout << "[EPCN] 成功加载 " << name << " 版本 " << entry.version << std::endl;
        return true;
    }
 
    // 热加载：从最新版本重新加载
    bool hot_reload(const std::string& name, std::function<bool(std::istream&)> loader) {
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            if (it->name == name) {
                return load_weights(name, it->path, loader);
            }
        }
        std::cerr << "[EPCN] 未找到模型 " << name << " 的历史记录" << std::endl;
        return false;
    }
 
    void save_history(std::ostream& out) const {
        int sz = history.size(); out.write((char*)&sz, sizeof(int));
        for (auto& e : history) {
            int name_len = e.name.size(); out.write((char*)&name_len, sizeof(int));
            out.write(e.name.c_str(), name_len);
            int path_len = e.path.size(); out.write((char*)&path_len, sizeof(int));
            out.write(e.path.c_str(), path_len);
            out.write((char*)&e.version, sizeof(int));
        }
    }
    void load_history(std::istream& in) {
        int sz; in.read((char*)&sz, sizeof(int));
        history.resize(sz);
        for (int i=0;i<sz;++i) {
            int name_len; in.read((char*)&name_len, sizeof(int));
            history[i].name.resize(name_len);
            in.read(&history[i].name[0], name_len);
            int path_len; in.read((char*)&path_len, sizeof(int));
            history[i].path.resize(path_len);
            in.read(&history[i].path[0], path_len);
            in.read((char*)&history[i].version, sizeof(int));
        }
    }
};
 
// ================== 集成API ==================
class RDCIntegrated {
public:
    TextRecognizer text_recognizer;
    ObjectDetector  object_detector;
    YUVLightStore   yuv_store;
    EPCNLoader      epcn_loader;
 
    // PFR BPS A3 特征提取（与主项目一致）
    static Tensor pfr_extract(const Tensor& rgb) {
        // 复用之前的洋葱卷积实现（此处内联简化版，确保独立）
        int H = rgb.shape[1], W = rgb.shape[2];
        // 灰阶
        Tensor gray({1, H, W});
        for (int i = 0; i < H*W; ++i) {
            float r = rgb.data_ptr()[0*H*W + i], g = rgb.data_ptr()[1*H*W + i], b = rgb.data_ptr()[2*H*W + i];
            gray.data_ptr()[i] = 0.299f*r + 0.587f*g + 0.114f*b;
        }
        // 简化特征（仅演示，完整版需包含边缘等）
        Tensor pfr({12, H, W});
        // 直接填充（实际应使用PFRBPSA3::extract，为保持独立这里用简化版）
        return pfr; 
    }
 
    // 统一识别：文字+物体
    struct RecognitionResult {
        int recognized_char = -1;           // 文字
        std::vector<ObjectDetector::Detection> objects; // 物体
    };
 
    RecognitionResult recognize(const Tensor& image) {
        RecognitionResult res;
        Tensor pfr = pfr_extract(image);
        // 文字识别
        res.recognized_char = text_recognizer.predict_char(pfr);
        // 物体检测
        res.objects = object_detector.forward(pfr, 0.5f);
        // YUV存储
        yuv_store.add_frame(image);
        return res;
    }
 
    // 加载预训练起始权重（降低训练门槛）
    void load_pretrained() {
        std::string base_path = "./pretrained/";
        // 创建目录
        mkdir(base_path.c_str(), 0755);
         
        // 尝试从文件加载，不存在则生成并保存
        auto try_load = [&](const std::string& name, auto& module) {
            std::string path = base_path + name + ".bin";
            std::ifstream in(path, std::ios::binary);
            if (in) {
                module.load(in);
                std::cout << "[集成] 加载预训练权重: " << name << std::endl;
            } else {
                module.init_pretrained();
                std::ofstream out(path, std::ios::binary);
                module.save(out);
                std::cout << "[集成] 生成并保存起始权重: " << name << std::endl;
            }
        };
        try_load("text_recognizer", text_recognizer);
        try_load("object_detector", object_detector);
    }
 
    void save_all(const std::string& dir) {
        mkdir(dir.c_str(), 0755);
        std::ofstream out(dir + "/text_recognizer.bin", std::ios::binary);
        text_recognizer.save(out);
        out.close();
        out.open(dir + "/object_detector.bin", std::ios::binary);
        object_detector.save(out);
        out.close();
        out.open(dir + "/yuv_store.bin", std::ios::binary);
        yuv_store.save(out);
    }
    void load_all(const std::string& dir) {
        std::ifstream in(dir + "/text_recognizer.bin", std::ios::binary);
        text_recognizer.load(in);
        in.close();
        in.open(dir + "/object_detector.bin", std::ios::binary);
        object_detector.load(in);
        in.close();
        in.open(dir + "/yuv_store.bin", std::ios::binary);
        yuv_store.load(in);
    }
};
 
// ================== 主函数（示例） ==================
// int main() {
//     RDCIntegrated rdc;
//     rdc.load_pretrained();
//     
//     // 模拟加载图像（实际需V4L2或文件读取）
//     Tensor image({3, 224, 224});
//     auto result = rdc.recognize(image);
//     
//     std::cout << "文字识别: " << result.recognized_char << std::endl;
//     std::cout << "检测物体: " << result.objects.size() << " 个" << std::endl;
//     return 0;
// }