#include <iostream>
#include <vector>
#include <string>
#include <numeric>     // 用于 std::iota
#include <random>      // 用于随机数生成
#include <algorithm>   // 用于 std::shuffle
#include <chrono>      // 用于高精度计时
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>

#include "kv_cache.h"

// 引入 LevelDB 头文件
#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/options.h"

// 任务三要观察 LevelDB Block Cache。当前仓库构建没有 Snappy，POSIX Env
// 对未压缩 SSTable 默认使用 mmap，LevelDB 会避免对 mmap 数据再进 block_cache。
// 测试 helper 可以在 Env::Default() 初始化前关闭只读 mmap，让 block_cache 生效。
#define private public
#include "util/env_posix_test_helper.h"
#undef private

// ============================================================================
// 工具类：Zipf 分布生成器 (用于模拟真实的“二八定律”热点访问)
// ============================================================================
class ZipfGenerator {
 private:
  std::mt19937 gen_;
  std::discrete_distribution<int> dist_;

 public:
  // 构造函数：预计算概率分布
  ZipfGenerator(int n, double s = 0.99) : gen_(std::random_device{}()) {
    std::vector<double> weights(n);
    for (int i = 1; i <= n; ++i) {
      weights[i - 1] = 1.0 / std::pow(i, s);  // Zipf 公式
    }
    dist_ = std::discrete_distribution<int>(weights.begin(), weights.end());
  }

  // 生成下一个随机 Key 的编号 (范围 1 到 N)
  int Next() { return dist_(gen_) + 1; }
};

// ============================================================================
// 辅助函数：将整数格式化为定长字符串 Key (例如: "user_key_0001234")
// ============================================================================
std::string FormatKey(int num) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "user_key_%07d", num);
  return std::string(buffer);
}

// Cache 相关
enum class CacheMode {
  kBlock,
  kKv,
};

void PrintUsage(const char* program) {
  std::cout
      << "用法: " << program << " --cache_mode=block|kv\n"
      << "  block: 纯 Block Cache\n"
      << "  kv:    纯 KV Cache\n";
}

const char* CacheModeName(CacheMode mode) {
  return mode == CacheMode::kBlock ? "纯 Block Cache" : "纯 KV Cache";
}

int main(int argc, char** argv) {
  // ------------------------------------------------------------------------
  // [固定配置参数] 两种 cache 情景都使用同一组参数，保证控制变量一致。
  // ------------------------------------------------------------------------
  const int NUM_LOAD_KEYS = 100000;
  const int NUM_RUN_OPS = 200000;
  const double READ_RATIO = 0.80;
  const double ZIPF_THETA = 1.2;
  const size_t BLOCK_CACHE_MB = 64;
  const size_t KV_CACHE_ENTRIES = 10000;
  const std::string DATA_DIR = "./build/data/task3";
  const std::string DB_PATH = DATA_DIR + "/mixed_rw_db";

  CacheMode cache_mode = CacheMode::kBlock;
  if (argc != 2) {
    PrintUsage(argv[0]);
    return 1;
  }

  // KV Cache vs Block Cache
  std::string arg = argv[1];
  if (arg == "--cache_mode=block") {
    cache_mode = CacheMode::kBlock;
  } else if (arg == "--cache_mode=kv") {
    cache_mode = CacheMode::kKv;
  } else {
    std::cerr << "未知参数: " << arg << "\n";
    PrintUsage(argv[0]);
    return 1;
  }

  std::cout << "========== LevelDB 任务三：KV Cache vs Block Cache ==========\n";
  std::filesystem::create_directories(DATA_DIR);
  std::cout << "缓存情景: " << CacheModeName(cache_mode) << "\n";
  std::cout << "Load keys: " << NUM_LOAD_KEYS << ", Run ops: " << NUM_RUN_OPS
            << ", read_ratio: " << READ_RATIO
            << ", zipf_theta: " << ZIPF_THETA << "\n";

  // ------------------------------------------------------------------------
  // 第一步：准备 Load 阶段的随机数据集
  // ------------------------------------------------------------------------
  std::cout << "[1/4] 正在生成 Load 阶段的随机打乱数据集...\n";
  std::vector<int> load_keys(NUM_LOAD_KEYS);
  std::iota(load_keys.begin(), load_keys.end(), 1);

  std::mt19937 random_engine(std::random_device{}());
  std::shuffle(load_keys.begin(), load_keys.end(), random_engine);

  // ------------------------------------------------------------------------
  // 第二步：初始化并打开 LevelDB
  // ------------------------------------------------------------------------
  std::cout << "[2/4] 正在初始化 LevelDB...\n";
  leveldb::EnvPosixTestHelper::SetReadOnlyMMapLimit(0);
  std::cout << "  -> POSIX read-only mmap: disabled for benchmark\n";

  leveldb::Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 4 * 1024 * 1024;

  std::unique_ptr<leveldb::Cache> block_cache;
  if (cache_mode == CacheMode::kBlock) {
    block_cache.reset(leveldb::NewLRUCache(BLOCK_CACHE_MB * 1024ULL * 1024ULL));
    options.block_cache = block_cache.get();
    std::cout << "  -> LevelDB Block Cache: enabled, capacity="
              << BLOCK_CACHE_MB << "MB\n";
    std::cout << "  -> Application KV Cache: disabled\n";
  } else {
    block_cache.reset(leveldb::NewLRUCache(0));
    options.block_cache = block_cache.get();
    std::cout << "  -> LevelDB Block Cache: disabled via NewLRUCache(0)\n";
    std::cout << "  -> Application KV Cache: enabled, capacity="
              << KV_CACHE_ENTRIES << " entries\n";
  }

  leveldb::Status destroy_status = leveldb::DestroyDB(DB_PATH, options);
  if (!destroy_status.ok() && !destroy_status.IsNotFound()) {
    std::cerr << "清理旧测试 DB 失败: " << destroy_status.ToString() << "\n";
    return 1;
  }

  leveldb::DB* db = nullptr;
  leveldb::Status status = leveldb::DB::Open(options, DB_PATH, &db);
  if (!status.ok()) {
    std::cerr << "LevelDB 打开失败: " << status.ToString() << "\n";
    return 1;
  }

  LruKvCache kv_cache(cache_mode == CacheMode::kKv ? KV_CACHE_ENTRIES : 0);

  // ------------------------------------------------------------------------
  // 第三步：执行 Load 阶段 (初始化数据库)
  // ------------------------------------------------------------------------
  std::cout << "[3/4] 开始 Load 阶段 (随机插入 " << NUM_LOAD_KEYS
            << " 条数据)...\n";
  auto start_time = std::chrono::high_resolution_clock::now();

  leveldb::WriteOptions write_opts;
  std::string dummy_value(100, 'x');

  for (int i = 0; i < NUM_LOAD_KEYS; ++i) {
    std::string key = FormatKey(load_keys[i]);
    status = db->Put(write_opts, key, dummy_value);
    if (!status.ok()) {
      std::cerr << "Load 写入失败: " << status.ToString() << "\n";
      delete db;
      return 1;
    }
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> load_duration = end_time - start_time;
  std::cout << "  -> Load 完成！耗时: " << load_duration.count() << " 秒\n";

  std::cout << "  -> 正在执行 CompactRange，确保初始数据进入 SSTable...\n";
  auto compact_start = std::chrono::high_resolution_clock::now();
  db->CompactRange(nullptr, nullptr);
  auto compact_end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> compact_duration = compact_end - compact_start;
  std::cout << "  -> CompactRange 完成！耗时: " << compact_duration.count()
            << " 秒\n";

  // ------------------------------------------------------------------------
  // 第四步：执行 Run 阶段
  // ------------------------------------------------------------------------
  std::cout << "[4/4] 开始 Run 阶段 (Zipf 分布, " << NUM_RUN_OPS
            << " 次操作)...\n";
  ZipfGenerator zipf(NUM_LOAD_KEYS, ZIPF_THETA);

  std::uniform_real_distribution<double> op_dist(0.0, 1.0);

  int get_count = 0;
  int put_count = 0;
  int not_found_count = 0;
  int read_error_count = 0;
  int write_error_count = 0;

  leveldb::ReadOptions read_opts;
  read_opts.fill_cache = (cache_mode == CacheMode::kBlock);

  std::cout << "  -> read_opts.fill_cache="
            << (read_opts.fill_cache ? "true" : "false") << "\n";

  std::string read_value;

  start_time = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < NUM_RUN_OPS; ++i) {
    std::string target_key = FormatKey(zipf.Next());
    double dice = op_dist(random_engine);

    if (dice < READ_RATIO) {
      if (kv_cache.Get(target_key, &read_value)) {
        ++get_count;
        continue;
      }

      status = db->Get(read_opts, target_key, &read_value);
      if (status.ok()) {
        kv_cache.Put(target_key, read_value);
        ++get_count;
      } else if (status.IsNotFound()) {
        ++not_found_count;
      } else {
        ++read_error_count;
      }
    } else {
      std::string new_value = "updated_value_" + std::to_string(i);
      status = db->Put(write_opts, target_key, new_value);
      if (status.ok()) {
        kv_cache.Put(target_key, new_value);
        ++put_count;
      } else {
        ++write_error_count;
      }
    }
  }

  end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> run_duration = end_time - start_time;
  const int successful_ops = get_count + put_count;

  // ------------------------------------------------------------------------
  // 输出统计报告
  // ------------------------------------------------------------------------
  std::cout << "\n========== 测试报告 ==========\n";
  std::cout << "Run 阶段总耗时: " << run_duration.count() << " 秒\n";
  std::cout << "成功操作数 QPS: " << successful_ops / run_duration.count()
            << " ops/sec\n";
  std::cout << "总请求数 QPS: " << NUM_RUN_OPS / run_duration.count()
            << " ops/sec\n";
  std::cout << "操作分布: 成功读取 " << get_count << " 次, 更新写入 "
            << put_count << " 次\n";

  if (kv_cache.Enabled()) {
    const LruKvCache::Stats stats = kv_cache.GetStats();
    const size_t lookups = stats.hits + stats.misses;
    const double hit_rate =
        lookups == 0 ? 0.0 : static_cast<double>(stats.hits) / lookups;
    std::cout << "KV Cache: size=" << kv_cache.Size() << "/"
              << kv_cache.Capacity() << ", hits=" << stats.hits
              << ", misses=" << stats.misses << ", hit_rate=" << hit_rate
              << ", inserts=" << stats.inserts
              << ", evictions=" << stats.evictions << "\n";
  }

  if (block_cache != nullptr) {
    std::cout << "Block Cache total charge: " << block_cache->TotalCharge()
              << " bytes\n";
  }

  if (not_found_count > 0) {
    std::cout << "警告: 有 " << not_found_count << " 次查询未找到数据\n";
  }
  if (read_error_count > 0 || write_error_count > 0) {
    std::cout << "错误统计: read_errors=" << read_error_count
              << ", write_errors=" << write_error_count << "\n";
  }

  delete db;
  return (read_error_count == 0 && write_error_count == 0) ? 0 : 1;
}
