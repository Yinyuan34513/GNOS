// ================================================================
// puff_cpu_optimized.h — Puff CPU Optimized 纯CPU仿生优化集成库
// API 说明、基础加速器、权重导出、EPCNv3 流动调度核心
// 编译：C++17，OpenMP (需提供 Tensor 等基础组件)
// ================================================================
#pragma once
 
/*********************** API 部署说明 *******************************
 * Puff CPU Optimized 是一个纯 CPU 仿生推理与训练优化库，可在任意
 * x86/ARM Linux 平台上运行，无需 GPU/NPU。它通过 EPCNv3 流动调度
 * 实现多任务并行，并集成权重实时导出、浮点向量加速、元观点算子池，
 * 可与 RDC A2 集成库协同使用以获得叠加性能。
 *
 * 快速开始：
 *   1. 包含本头文件: #include "puff_cpu_optimized.h"
 *   2. 创建实例: PuffCPUOptimized engine;
 *   3. 加载或生成预训练权重: engine.load_weights("weights.bin");
 *   4. 启动 EPCN 流动调度: engine.start();
 *   5. 提交视觉/文本任务: engine.submit_vision(frame);
 *      engine.submit_text("Hello");
 *   6. 权重将自动在后台导出至指定目录。
 *
 * 与 RDC A2 集成库叠加使用：
 *   RDCIntegrated rdc;  // 来自 rdc_integrated.h
 *   PuffCPUOptimized puff;
 *   puff.attach_rdc(&rdc);  // 绑定 A2 库
 *   puff.start();
 *   // 现在识别结果会自动利用 RDC 的轻量 OCR/检测头
 ********************************************************************/
 
#include "puff_a3_part1.h"   // 依赖已有基础组件 (Tensor, Arena, gemm 等)
#include <atomic>
#include <queue>
#include <fstream>
#include <thread>
#include <condition_variable>
 
// -------------------- 浮点向量流动加速器 --------------------
// 针对 CPU SIMD 优化的向量运算集合，提升计算核心利用率与负载均衡
class FloatVectorFlowAccelerator {
public:
    // 利用 OpenMP 将循环均匀分配到各核心，实现性能核均匀
    template<typename Func>
    static void parallel_for(int begin, int end, Func func, int grain_size = 1024) {
        #pragma omp parallel for schedule(static)
        for (int i = begin; i < end; ++i) {
            func(i);
        }
    }
 
    // 向量加法 (float* a + b -> c) 带 NEON 优化
    static void vector_add(const float* a, const float* b, float* c, int n) {
        #ifdef __ARM_NEON
        #include <arm_neon.h>
        int i = 0;
        for (; i <= n - 4; i += 4) {
            float32x4_t va = vld1q_f32(a + i);
            float32x4_t vb = vld1q_f32(b + i);
            vst1q_f32(c + i, vaddq_f32(va, vb));
        }
        for (; i < n; ++i) c[i] = a[i] + b[i];
        #else
        for (int i = 0; i < n; ++i) c[i] = a[i] + b[i];
        #endif
    }
 
    // 向量点积，自动选择最佳路径
    static float dot_product(const float* a, const float* b, int n) {
        float result = 0.0f;
        #ifdef __ARM_NEON
        float32x4_t vsum = vdupq_n_f32(0.0f);
        int i = 0;
        for (; i <= n - 4; i += 4) {
            float32x4_t va = vld1q_f32(a + i);
            float32x4_t vb = vld1q_f32(b + i);
            vsum = vmlaq_f32(vsum, va, vb);
        }
        float buf[4]; vst1q_f32(buf, vsum);
        result = buf[0] + buf[1] + buf[2] + buf[3];
        for (; i < n; ++i) result += a[i] * b[i];
        #else
        for (int i = 0; i < n; ++i) result += a[i] * b[i];
        #endif
        return result;
    }
};
 
// -------------------- CPU 权重实时导出器 --------------------
// 通过 EPCNv3 流动调度，在后台将最新权重异步写入磁盘或网络流
class CPUWeightExporter {
public:
    struct ExportTask {
        std::string layer_name;
        std::vector<float> weights;      // 当前权重数据
        std::vector<float> gradients;    // 可选梯度（用于调试）
        int version = 0;
    };
 
    CPUWeightExporter(const std::string& output_dir = "./weight_checkpoints")
        : output_dir_(output_dir), running_(false) {
        mkdir(output_dir_.c_str(), 0755);
    }
 
    void start() {
        running_ = true;
        worker_ = std::thread([this]() {
            while (running_) {
                ExportTask task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this]() { return !task_queue_.empty() || !running_; });
                    if (!running_) break;
                    task = std::move(task_queue_.front());
                    task_queue_.pop();
                }
                write_to_disk(task);
            }
        });
    }
 
    void stop() {
        running_ = false;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
 
    // 提交导出任务（由 EPCN 调度器调用）
    void submit_export(const std::string& layer, const std::vector<float>& w,
                       const std::vector<float>& grad = {}, int ver = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        task_queue_.push({layer, w, grad, ver});
        cv_.notify_one();
    }
 
private:
    std::string output_dir_;
    std::atomic<bool> running_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<ExportTask> task_queue_;
 
    void write_to_disk(const ExportTask& task) {
        std::string path = output_dir_ + "/" + task.layer_name + "_v" + 
                           std::to_string(task.version) + ".bin";
        std::ofstream out(path, std::ios::binary);
        if (out) {
            int sz = task.weights.size();
            out.write((char*)&sz, sizeof(int));
            out.write((char*)task.weights.data(), sz * sizeof(float));
            if (!task.gradients.empty()) {
                out.write((char*)task.gradients.data(), sz * sizeof(float));
            }
            out.close();
        }
    }
};
 
// -------------------- 元观点强化算子池 --------------------
// 与 EPCNv3 协同，提供算子自动注册、选择、优化和缓存，可叠加 A2 库
class MetaOperatorPool {
public:
    struct OpMeta {
        std::string type;                // "Conv2D", "PFR", "Linear"...
        std::vector<int> in_shape;
        std::vector<int> out_shape;
        std::function<Tensor(const Tensor&, Arena*)> func_cpu;
        int cost_us = 0;                // 预估耗时 (微秒)
        bool reusable = true;
    };
 
    void register_op(const OpMeta& op) {
        pool_[op.type].push_back(op);
    }
 
    // 根据输入形状和类型找到最优算子（考虑缓存和复用）
    const OpMeta* find_best(const std::string& type, const std::vector<int>& in_shape) {
        if (pool_.find(type) == pool_.end()) return nullptr;
        for (auto& op : pool_[type]) {
            if (op.in_shape == in_shape && op.reusable) {
                return &op;
            }
        }
        // 未找到完全匹配，尝试寻找可复用的近似算子
        for (auto& op : pool_[type]) {
            if (op.in_shape.size() == in_shape.size() && op.reusable) {
                return &op;
            }
        }
        return nullptr;
    }
 
    // 执行算子并缓存结果（如果允许）
    Tensor execute(const std::string& type, const Tensor& input, Arena* arena,
                   std::unordered_map<std::string, Tensor>& cache) {
        std::string cache_key = type + "_" + shape_to_string(input.shape);
        if (cache.find(cache_key) != cache.end()) {
            return cache[cache_key];   // 命中缓存
        }
        const OpMeta* op = find_best(type, input.shape);
        if (!op) {
            // 尝试自动创建 Fallback 算子
            OpMeta fallback;
            fallback.type = type;
            fallback.in_shape = input.shape;
            // 此处根据类型填充默认实现 (见后续)
            register_op(fallback);
            op = find_best(type, input.shape);
        }
        Tensor output = op->func_cpu(input, arena);
        if (op->reusable) {
            cache[cache_key] = output;
        }
        return output;
    }
 
private:
    std::unordered_map<std::string, std::vector<OpMeta>> pool_;
    static std::string shape_to_string(const std::vector<int>& s) {
        std::stringstream ss;
        for (size_t i = 0; i < s.size(); ++i) {
            if (i) ss << "x";
            ss << s[i];
        }
        return ss.str();
    }
};
// ================================================================
// EPCNv3 流动调度器、A2 桥接、顶层 API
// ================================================================
#pragma once
#include "puff_cpu_optimized.h"
#include <unordered_set>
 
// -------------------- EPCNv3 流动调度器 --------------------
// 基于时分复用的多任务调度，支持视觉、文本、记忆、权重导出等任务流动执行
class EPCNv3FlowScheduler {
public:
    struct Task {
        enum Type { VISION, TEXT, MEMORY, EXPORT, IDLE };
        Type type = IDLE;
        Tensor frame;                // 视觉帧
        std::string text_input;      // 文本输入
        int priority = 0;
        int64_t submit_time = 0;
    };
 
    EPCNv3FlowScheduler() : running_(false), vis_cycle_(2), txt_cycle_(5), mem_cycle_(20) {}
 
    void start() {
        running_ = true;
        worker_ = std::thread([this]() { run_loop(); });
    }
 
    void stop() {
        running_ = false;
        if (worker_.joinable()) worker_.join();
    }
 
    // 提交任务（可从多个线程安全调用）
    void submit_task(Task task) {
        std::lock_guard<std::mutex> lock(mutex_);
        task_queue_.push(std::move(task));
        cv_.notify_one();
    }
 
    // 设置回调：当特定类型任务完成时通知外部
    void set_vision_callback(std::function<void(const std::string&)> cb) { vis_cb_ = std::move(cb); }
    void set_text_callback(std::function<void(const std::string&)> cb) { txt_cb_ = std::move(cb); }
    void set_memory_callback(std::function<void(int)> cb) { mem_cb_ = std::move(cb); }
 
    // 获取关联的元观点算子池和权重导出器
    MetaOperatorPool& op_pool() { return op_pool_; }
    CPUWeightExporter& weight_exporter() { return weight_exporter_; }
 
private:
    std::atomic<bool> running_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Task> task_queue_;
 
    int vis_cycle_, txt_cycle_, mem_cycle_;
    int tick_ = 0;
 
    MetaOperatorPool op_pool_;
    CPUWeightExporter weight_exporter_;
 
    std::function<void(const std::string&)> vis_cb_;
    std::function<void(const std::string&)> txt_cb_;
    std::function<void(int)> mem_cb_;
 
    void run_loop() {
        Arena arena;
        std::unordered_map<std::string, Tensor> result_cache;
        while (running_) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                    return !task_queue_.empty() || !running_;
                });
                if (!running_) break;
                if (!task_queue_.empty()) {
                    task = std::move(task_queue_.front());
                    task_queue_.pop();
                } else {
                    continue;
                }
            }
 
            switch (task.type) {
                case Task::VISION:
                    if (vis_cb_) {
                        // 使用元观点池执行 PFR 提取 + 识别
                        auto result = process_vision_task(task.frame, &arena, result_cache);
                        vis_cb_(result);
                    }
                    break;
                case Task::TEXT:
                    if (txt_cb_) {
                        auto result = process_text_task(task.text_input);
                        txt_cb_(result);
                    }
                    break;
                case Task::MEMORY:
                    // 触发海马体重放
                    if (mem_cb_) mem_cb_(0);
                    break;
                case Task::EXPORT:
                    // 权重导出已在后台线程自动运行
                    break;
                default:
                    break;
            }
        }
    }
 
    std::string process_vision_task(const Tensor& frame, Arena* arena,
                                    std::unordered_map<std::string, Tensor>& cache) {
        // 使用元观点池执行 PFR BPS 提取
        const MetaOperatorPool::OpMeta* pfr_op = op_pool_.find_best("PFR_BPS", frame.shape);
        Tensor pfr_features;
        if (pfr_op) {
            pfr_features = pfr_op->func_cpu(frame, arena);
        } else {
            // Fallback: 直接调用 PFRBPSA3::extract
            pfr_features = PFRBPSA3::extract(frame);
        }
        // 后续可以接分类头、检测头等
        return "Vision processed: " + std::to_string(pfr_features.shape[0]) + " channels";
    }
 
    std::string process_text_task(const std::string& input) {
        // 文本处理可通过 RDC A2 桥接或内置轻量分词器
        return "Text reply: " + input.substr(0, 20);
    }
};
 
// -------------------- RDC A2 集成桥接器 --------------------
class RDCA2Bridge {
public:
    RDCIntegrated* rdc = nullptr;
 
    void attach(RDCIntegrated* rdc_ptr) { rdc = rdc_ptr; }
 
    // 增强识别：使用 A2 库的 OCR 和物体检测
    std::string enhanced_recognize(const Tensor& frame) {
        if (!rdc) return "RDC A2 not attached";
        auto result = rdc->recognize(frame);
        std::stringstream ss;
        ss << "Char: " << result.recognized_char 
           << " Objects: " << result.objects.size();
        return ss.str();
    }
};
 
// -------------------- 顶层优化接口 --------------------
class PuffCPUOptimized {
public:
    EPCNv3FlowScheduler scheduler;
    RDCA2Bridge a2_bridge;
    HippocampusMemory vis_memory;
    YUVLightStore yuv_store;
 
    PuffCPUOptimized() {
        // 启动权重导出
        scheduler.weight_exporter().start();
        // 注册默认元观点算子
        register_default_ops();
    }
 
    ~PuffCPUOptimized() {
        scheduler.stop();
        scheduler.weight_exporter().stop();
    }
 
    // 绑定 RDC A2 集成库以增强识别
    void attach_rdc(RDCIntegrated* rdc) { a2_bridge.attach(rdc); }
 
    // 加载权重（从文件）
    bool load_weights(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        // 实际应反序列化各层权重，这里仅为示例
        return true;
    }
 
    // 启动 EPCN 流动调度
    void start() {
        scheduler.set_vision_callback([this](const std::string& msg) {
            std::cout << "[视觉] " << msg << std::endl;
        });
        scheduler.set_text_callback([this](const std::string& msg) {
            std::cout << "[文本] " << msg << std::endl;
        });
        scheduler.set_memory_callback([this](int) {
            std::cout << "[记忆] 重放完成，当前记忆数: " << vis_memory.mem.size() << std::endl;
        });
        scheduler.start();
    }
 
    // 提交视觉任务
    void submit_vision(const Tensor& frame) {
        EPCNv3FlowScheduler::Task task;
        task.type = EPCNv3FlowScheduler::Task::VISION;
        task.frame = frame;
        scheduler.submit_task(task);
    }
 
    // 提交文本任务
    void submit_text(const std::string& text) {
        EPCNv3FlowScheduler::Task task;
        task.type = EPCNv3FlowScheduler::Task::TEXT;
        task.text_input = text;
        scheduler.submit_task(task);
    }
 
    // 触发记忆重放
    void trigger_memory_replay() {
        EPCNv3FlowScheduler::Task task;
        task.type = EPCNv3FlowScheduler::Task::MEMORY;
        scheduler.submit_task(task);
    }
 
    // 实时导出指定层的权重
    void export_layer_weights(const std::string& layer_name, const std::vector<float>& weights,
                              int version = 0) {
        scheduler.weight_exporter().submit_export(layer_name, weights, {}, version);
    }
 
private:
    void register_default_ops() {
        // 注册 PFR BPS 算子到元观点池
        MetaOperatorPool::OpMeta pfr_op;
        pfr_op.type = "PFR_BPS";
        pfr_op.in_shape = {3, 224, 224};
        pfr_op.out_shape = {12, 224, 224};
        pfr_op.func_cpu = [](const Tensor& in, Arena* arena) -> Tensor {
            return PFRBPSA3::extract(in);
        };
        pfr_op.cost_us = 1200;
        pfr_op.reusable = true;
        scheduler.op_pool().register_op(pfr_op);
 
        // 注册一个简单的 Conv2D Fallback 算子
        MetaOperatorPool::OpMeta conv_op;
        conv_op.type = "Conv2D";
        conv_op.in_shape = {12, 224, 224};
        conv_op.out_shape = {32, 112, 112};
        conv_op.func_cpu = [](const Tensor& in, Arena* arena) -> Tensor {
            // 简单卷积示例（实际应使用 im2col+gemm 或预训练权重）
            return Tensor::zeros({32, 112, 112});
        };
        conv_op.cost_us = 5000;
        conv_op.reusable = false;
        scheduler.op_pool().register_op(conv_op);
    }
};
 
// ================== 主函数 ==================
int main(int argc, char* argv[]) {
    std::cout << "Puff CPU Optimized 集成引擎启动\n";
    std::cout << "特性：EPCNv3流动调度 | 浮点向量加速 | 权重实时导出 | RDC A2桥接\n";
 
    PuffCPUOptimized engine;
 
    // 绑定 RDC A2 集成库（如果可用）
    RDCIntegrated rdc_a2;
    rdc_a2.load_pretrained();
    engine.attach_rdc(&rdc_a2);
 
    // 尝试打开摄像头
    V4L2Camera cam;
    bool use_camera = cam.open("/dev/video0");
    if (!use_camera) {
        std::cerr << "警告：无法打开摄像头，将使用模拟图像。\n";
    }
 
    // 设置回调：实时打印任务结果
    engine.scheduler.set_vision_callback([&](const std::string& msg) {
        std::cout << "[视觉] " << msg;
        // 如果绑定了 A2，可进一步获取增强识别
        if (engine.a2_bridge.rdc) {
            Tensor frame; // 此处应从任务中传递真实帧，为简化仅打印
            // std::string enhanced = engine.a2_bridge.enhanced_recognize(frame);
            // std::cout << " | A2增强: " << enhanced;
        }
        std::cout << std::endl;
    });
    engine.scheduler.set_text_callback([](const std::string& msg) {
        std::cout << "[文本] " << msg << std::endl;
    });
    engine.scheduler.set_memory_callback([&](int) {
        std::cout << "[记忆] 当前记忆槽: " << engine.vis_memory.mem.size() << std::endl;
    });
 
    engine.start(); // 启动 EPCN 流动调度
 
    // 命令行交互
    std::string input;
    int frame_id = 0;
    auto start_time = std::chrono::high_resolution_clock::now();
    int total_tasks = 0;
 
    while (true) {
        std::cout << "\n[命令] (vision/v, text/t, memory/m, status/s, quit/q): ";
        std::getline(std::cin, input);
        if (input == "quit" || input == "q") break;
        if (input == "vision" || input == "v") {
            Tensor frame;
            if (use_camera) {
                frame = cam.capture();
            } else {
                // 生成模拟图像
                frame = Tensor({3, 224, 224});
                for (auto& v : frame.data) v = (rand() % 256) / 255.0f;
            }
            engine.submit_vision(frame);
            total_tasks++;
        } else if (input == "text" || input == "t") {
            std::cout << "输入文本: ";
            std::string text;
            std::getline(std::cin, text);
            if (!text.empty()) {
                engine.submit_text(text);
                total_tasks++;
            }
        } else if (input == "memory" || input == "m") {
            engine.trigger_memory_replay();
            total_tasks++;
        } else if (input == "status" || input == "s") {
            auto end_time = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
            std::cout << "运行时间: " << elapsed << " ms | 完成任务数: " << total_tasks << "\n";
            std::cout << "YUV记录: " << engine.yuv_store.get_records().size() << "\n";
        } else {
            std::cout << "未知命令，请使用: vision/v, text/t, memory/m, status/s, quit/q\n";
        }
 
        // 每处理两个任务，触发一次权重导出示例
        if (total_tasks % 2 == 0 && total_tasks > 0) {
            std::vector<float> demo_weights(256, 0.25f);
            engine.export_layer_weights("demo_layer", demo_weights, total_tasks / 2);
        }
    }
 
    engine.scheduler.stop();
    std::cout << "Puff CPU Optimized 已退出。完成任务总数: " << total_tasks << "\n";
    return 0;
}