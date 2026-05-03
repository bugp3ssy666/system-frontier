#include <iostream>
#include <vector>
#include <string>
#include <numeric>     // 用于 std::iota
#include <random>      // 用于随机数生成
#include <algorithm>   // 用于 std::shuffle
#include <chrono>      // 用于高精度计时
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <filesystem>

// 引入 LevelDB 头文件
#include "leveldb/db.h"
#include "leveldb/options.h"

// ============================================================================
// 工具类：Zipf 分布生成器 (用于模拟真实的“二八定律”热点访问)
// ============================================================================
class ZipfGenerator {
private:
    std::mt19937 gen_;
    std::discrete_distribution<int> dist_;

public:
    // 构造函数：预计算概率分布
    ZipfGenerator(int N, double s = 0.99) : gen_(std::random_device{}()) {
        std::vector<double> weights(N);
        for (int i = 1; i <= N; ++i) {
            weights[i - 1] = 1.0 / std::pow(i, s); // Zipf 公式
        }
        dist_ = std::discrete_distribution<int>(weights.begin(), weights.end());
    }

    // 生成下一个随机 Key 的编号 (范围 1 到 N)
    int Next() {
        return dist_(gen_) + 1;
    }
};

// ============================================================================
// 辅助函数：将整数格式化为定长字符串 Key (例如: "user_key_0001234")
// ============================================================================
std::string FormatKey(int num) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "user_key_%07d", num);
    return std::string(buffer);
}

// Task2：任务二需要持续随机写入新 Key，而不是反复覆盖旧 Key，所以新增随机前缀 + 递增序号的唯一 Key。
std::string FormatWriteKey(uint64_t seq, uint64_t random_part) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "write_key_%016llx_%016llx",
             static_cast<unsigned long long>(random_part),
             static_cast<unsigned long long>(seq));
    return std::string(buffer);
}

// Task2：任务二要求输出 P50/P90/P99/P99.9，因此新增通用分位数计算函数，输入必须已经排好序。
uint64_t PercentileFromSorted(const std::vector<uint64_t>& sorted_values, double p) {
    if (sorted_values.empty()) {
        return 0;
    }
    size_t index = static_cast<size_t>(std::ceil(p * sorted_values.size())) - 1;
    index = std::min(index, sorted_values.size() - 1);
    return sorted_values[index];
}

// Task2：用于窗口统计时复制并排序最近一段写延迟，方便观察 compaction 期间的延迟波动。
uint64_t PercentileOfWindow(std::vector<uint64_t> values, double p) {
    std::sort(values.begin(), values.end());
    return PercentileFromSorted(values, p);
}

uint64_t ReadMemAvailableKB() {
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    uint64_t value = 0;
    std::string unit;
    while (meminfo >> key >> value >> unit) {
        if (key == "MemAvailable:") {
            return value;
        }
    }

    return 15698752ULL;
}

int main() {
    // ------------------------------------------------------------------------
    // [配置参数]
    // ------------------------------------------------------------------------
    const uint64_t MEM_AVAILABLE_KB = ReadMemAvailableKB(); // Task2：运行时读取当前系统 MemAvailable。
    const uint64_t TARGET_DATASET_BYTES = MEM_AVAILABLE_KB * 1024ULL * 21ULL / 10ULL; // Task2：目标数据量设置为可用内存的 2.1 倍，满足实验数据集大小要求。
    const size_t VALUE_SIZE = 4096; // Task2：使用 4KB value，减少操作次数但仍能写出足够大的数据集。
    const size_t ESTIMATED_KEY_SIZE = 48; // Task2：用于估算每条记录大小，从而计算需要执行多少次 Put。
    const uint64_t NUM_RUN_OPS = TARGET_DATASET_BYTES / (VALUE_SIZE + ESTIMATED_KEY_SIZE) + 1; // Task2：按目标数据量自动计算写入次数。
    const uint64_t REPORT_INTERVAL = 100000; // Task2：每 10 万次写输出一次窗口延迟和 LevelDB 层级状态，观察 compaction 现象。
    const std::string DATA_DIR = "./build/data/task2";
    const std::string RESULT_DIR = "./build/results";
    const std::string DB_PATH = DATA_DIR + "/db";
    const std::string CSV_PATH = RESULT_DIR + "/task2_latency.csv";

    std::cout << "========== LevelDB 任务二：长尾写延迟与 Compaction 观察 ==========\n"; // Task2：更新标题。
    std::filesystem::create_directories(DATA_DIR);
    std::filesystem::create_directories(RESULT_DIR);

    // ------------------------------------------------------------------------
    // 第一步：准备随机数生成器
    // ------------------------------------------------------------------------
    std::cout << "[1/4] 正在准备随机写入参数...\n"; // Task2：不再生成小规模 Load 数据集，改为准备随机写 Key。
    std::mt19937_64 random_engine(std::random_device{}()); // Task2：使用 64 位随机数生成随机 Key 前缀。
    std::cout << "  -> MemAvailable: " << MEM_AVAILABLE_KB << " KiB\n"; // Task2：打印用于计算目标数据集的内存值，方便写报告。
    std::cout << "  -> 目标用户数据量: " << (TARGET_DATASET_BYTES / 1024.0 / 1024.0 / 1024.0) << " GiB\n"; // Task2：确认数据集大于可用内存 2 倍。
    std::cout << "  -> 预计随机写入次数: " << NUM_RUN_OPS << "\n"; // Task2：给出即将执行的 Put 次数。

    // ------------------------------------------------------------------------
    // 第二步：初始化并打开 LevelDB
    // ------------------------------------------------------------------------
    std::cout << "[2/4] 正在初始化 LevelDB...\n";
    leveldb::DB* db;
    leveldb::Options options;
    options.create_if_missing = true;          // 如果目录不存在则创建
    options.write_buffer_size = 4 * 1024 * 1024; // 4MB 的 MemTable (写缓存)
    options.compression = leveldb::kNoCompression; // Task2：关闭压缩，避免全 'x' 的 value 被压得很小，影响“大于内存 2 倍”的落盘效果。

    leveldb::Status destroy_status = leveldb::DestroyDB(DB_PATH, options);
    if (!destroy_status.ok() && !destroy_status.IsNotFound()) {
        std::cerr << "清理旧测试 DB 失败: " << destroy_status.ToString() << "\n";
        return -1;
    }

    leveldb::Status status = leveldb::DB::Open(options, DB_PATH, &db);
    if (!status.ok()) {
        std::cerr << "LevelDB 打开失败: " << status.ToString() << "\n";
        return -1;
    }

    // ------------------------------------------------------------------------
    // 第三步：执行高速随机写入并记录每次写延迟
    // ------------------------------------------------------------------------
    std::cout << "[3/4] 开始持续随机写入...\n"; // Task2：任务二关注写入尾延迟，因此要改为纯 Put 压测。
    auto start_time = std::chrono::high_resolution_clock::now();

    leveldb::WriteOptions write_opts; // 默认异步写，性能好
    write_opts.sync = false; // Task2：保持默认异步 WAL 写入，主要观察 MemTable flush 和后台 compaction 对尾延迟的影响。
    std::string dummy_value(VALUE_SIZE, 'x'); // Task2：value 放大到 4KB，并配合关闭压缩保证真实写入量足够大。
    std::vector<uint64_t> latencies_us; // Task2：保存每次 Put 的微秒级延迟，用于最终计算 P50/P90/P99/P99.9。
    latencies_us.reserve(static_cast<size_t>(NUM_RUN_OPS));
    std::vector<uint64_t> window_latencies_us; // Task2：保存一个窗口内的写延迟，用于观察延迟随时间变化。
    window_latencies_us.reserve(static_cast<size_t>(REPORT_INTERVAL));
    std::ofstream csv_file(CSV_PATH); // Task2：输出窗口延迟 CSV，后续可用 Python/Excel 画随时间变化图。
    csv_file << "elapsed_sec,ops,written_gib,window_p50_us,window_p99_us,l0_files,l1_files,l2_files\n"; // Task2：CSV 表头记录时间、进度、窗口延迟和 compaction 相关层级文件数。

    for (uint64_t i = 0; i < NUM_RUN_OPS; ++i) {
        std::string target_key = FormatWriteKey(i, random_engine()); // Task2：每次 Put 使用新的随机 Key，扩大数据集并触发更多 compaction。

        auto put_start = std::chrono::high_resolution_clock::now(); // Task2：单独记录一次 Put 的开始时间，用于统计写延迟。
        status = db->Put(write_opts, target_key, dummy_value);
        auto put_end = std::chrono::high_resolution_clock::now(); // Task2：单独记录一次 Put 的结束时间，用于统计写延迟。

        if (!status.ok()) { // Task2：压测过程中如果写入失败，立即报告，避免统计结果失真。
            std::cerr << "写入失败: " << status.ToString() << "\n";
            break;
        }

        uint64_t latency_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(put_end - put_start).count()); // Task2：把单次 Put 延迟转换成微秒保存。
        latencies_us.push_back(latency_us);
        window_latencies_us.push_back(latency_us);

        if ((i + 1) % REPORT_INTERVAL == 0) { // Task2：周期性输出窗口延迟和 LevelDB 层级文件数，用来观察 compaction 对写延迟的影响。
            auto now = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = now - start_time;
            std::string l0_files = "N/A", l1_files = "N/A", l2_files = "N/A";
            db->GetProperty("leveldb.num-files-at-level0", &l0_files);
            db->GetProperty("leveldb.num-files-at-level1", &l1_files);
            db->GetProperty("leveldb.num-files-at-level2", &l2_files);

            uint64_t window_p50 = PercentileOfWindow(window_latencies_us, 0.50);
            uint64_t window_p99 = PercentileOfWindow(window_latencies_us, 0.99);
            double written_gib = ((i + 1) * static_cast<double>(VALUE_SIZE + ESTIMATED_KEY_SIZE)) / 1024.0 / 1024.0 / 1024.0;
            double progress_percent = (i + 1) * 100.0 / NUM_RUN_OPS; // Task2：输出整体完成百分比，方便在 console 中直接监视测试进度。
            double writes_per_sec = (i + 1) / elapsed.count(); // Task2：根据当前累计写入速度估算剩余测试时间。
            double eta_sec = (NUM_RUN_OPS - (i + 1)) / writes_per_sec; // Task2：输出 ETA，便于判断长时间压测还需要多久结束。

            std::cout << "  -> progress=" << progress_percent << "%"
                      << ", ops=" << (i + 1) << "/" << NUM_RUN_OPS
                      << ", elapsed=" << elapsed.count() << "s"
                      << ", eta~=" << eta_sec << "s"
                      << ", written~=" << written_gib << "GiB"
                      << ", window_P50=" << window_p50 << "us"
                      << ", window_P99=" << window_p99 << "us"
                      << ", L0/L1/L2=" << l0_files << "/" << l1_files << "/" << l2_files
                      << "\n" << std::flush; // Task2：强制刷新输出，确保 tee 或重定向时也能及时看到进度。

            csv_file << elapsed.count() << ","
                     << (i + 1) << ","
                     << written_gib << ","
                     << window_p50 << ","
                     << window_p99 << ","
                     << l0_files << ","
                     << l1_files << ","
                     << l2_files << "\n";
            window_latencies_us.clear();
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now(); // Task2：记录整个随机写压测结束时间。
    std::chrono::duration<double> run_duration = end_time - start_time;

    // ------------------------------------------------------------------------
    // 输出统计报告
    // ------------------------------------------------------------------------
    std::cout << "\n========== 测试报告 ==========\n";
    std::cout << "随机写入总耗时: " << run_duration.count() << " 秒\n"; // Task2：报告纯随机写阶段耗时。
    std::cout << "成功写入次数: " << latencies_us.size() << "\n"; // Task2：报告实际成功 Put 次数。
    std::cout << "写入吞吐: " << latencies_us.size() / run_duration.count() << " writes/sec\n"; // Task2：报告写吞吐，而不是混合读写 QPS。

    if (!latencies_us.empty()) { // Task2：排序后输出任务二要求的 P50/P90/P99/P99.9 写延迟。
        std::sort(latencies_us.begin(), latencies_us.end());
        std::cout << "P50 写延迟:   " << PercentileFromSorted(latencies_us, 0.50) << " us\n";
        std::cout << "P90 写延迟:   " << PercentileFromSorted(latencies_us, 0.90) << " us\n";
        std::cout << "P99 写延迟:   " << PercentileFromSorted(latencies_us, 0.99) << " us\n";
        std::cout << "P99.9 写延迟: " << PercentileFromSorted(latencies_us, 0.999) << " us\n";
    }

    std::string leveldb_stats; // Task2：输出 LevelDB 内部 compaction 统计，报告中可据此解释尾延迟波动。
    if (db->GetProperty("leveldb.stats", &leveldb_stats)) {
        std::cout << "\n========== LevelDB Compaction Stats ==========\n";
        std::cout << leveldb_stats << "\n";
    }

    std::cout << "窗口延迟 CSV 已输出到: " << CSV_PATH << "\n"; // Task2：提示 CSV 文件位置，可用于绘图分析。

    delete db;
    return 0;
}
