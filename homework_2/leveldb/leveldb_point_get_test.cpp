#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "kv_cache.h"

#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/options.h"

// 为了让 Block Cache 在本实验里真正生效，关闭 POSIX 只读 mmap。
// 当前仓库没有启用 Snappy，未压缩 SSTable 使用 mmap 时会绕过 block_cache。
#define private public
#include "util/env_posix_test_helper.h"
#undef private

class ZipfGenerator {
 private:
  std::mt19937 gen_;
  std::discrete_distribution<int> dist_;

 public:
  ZipfGenerator(int n, double s) : gen_(std::random_device{}()) {
    std::vector<double> weights(n);
    for (int i = 1; i <= n; ++i) {
      weights[i - 1] = 1.0 / std::pow(i, s);
    }
    dist_ = std::discrete_distribution<int>(weights.begin(), weights.end());
  }

  int Next() { return dist_(gen_) + 1; }
};

std::string FormatKey(int num) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "user_key_%07d", num);
  return std::string(buffer);
}

enum class CacheMode {
  kBlock,
  kKv,
};

void PrintUsage(const char* program) {
  std::cout << "用法: " << program << " --cache_mode=block|kv\n"
            << "  block: 纯 Block Cache\n"
            << "  kv:    纯 KV Cache\n";
}

const char* CacheModeName(CacheMode mode) {
  return mode == CacheMode::kBlock ? "纯 Block Cache" : "纯 KV Cache";
}

int main(int argc, char** argv) {
  // 两种 cache 情景共用同一组固定参数。
  const int NUM_LOAD_KEYS = 100000;
  const int NUM_GETS = 200000;
  const double ZIPF_THETA = 1.2;
  const size_t BLOCK_CACHE_MB = 64;
  const size_t KV_CACHE_ENTRIES = 10000;
  const std::string DATA_DIR = "./build/data/task3";
  const std::string DB_PATH = DATA_DIR + "/point_get_db";

  CacheMode cache_mode = CacheMode::kBlock;
  if (argc != 2) {
    PrintUsage(argv[0]);
    return 1;
  }

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

  std::cout << "========== LevelDB 任务三：高频随机读 Point Get ==========\n";
  std::filesystem::create_directories(DATA_DIR);
  std::cout << "缓存情景: " << CacheModeName(cache_mode) << "\n";
  std::cout << "Load keys: " << NUM_LOAD_KEYS << ", Gets: " << NUM_GETS
            << ", zipf_theta: " << ZIPF_THETA << "\n";

  std::cout << "[1/4] 正在生成 Load 阶段的随机打乱数据集...\n";
  std::vector<int> load_keys(NUM_LOAD_KEYS);
  std::iota(load_keys.begin(), load_keys.end(), 1);
  std::mt19937 random_engine(std::random_device{}());
  std::shuffle(load_keys.begin(), load_keys.end(), random_engine);

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

  std::cout << "[3/4] 开始 Load 阶段 (随机插入 " << NUM_LOAD_KEYS
            << " 条数据)...\n";
  auto start_time = std::chrono::high_resolution_clock::now();

  leveldb::WriteOptions write_opts;
  std::string dummy_value(100, 'x');
  for (int i = 0; i < NUM_LOAD_KEYS; ++i) {
    status = db->Put(write_opts, FormatKey(load_keys[i]), dummy_value);
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
  db->CompactRange(nullptr, nullptr);

  std::cout << "[4/4] 开始 Point Get 阶段 (Zipf 分布)...\n";
  ZipfGenerator zipf(NUM_LOAD_KEYS, ZIPF_THETA);

  leveldb::ReadOptions read_opts;
  read_opts.fill_cache = (cache_mode == CacheMode::kBlock);
  std::cout << "  -> read_opts.fill_cache="
            << (read_opts.fill_cache ? "true" : "false") << "\n";

  int found_count = 0;
  int not_found_count = 0;
  int read_error_count = 0;
  std::string read_value;

  start_time = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < NUM_GETS; ++i) {
    std::string key = FormatKey(zipf.Next());

    if (kv_cache.Get(key, &read_value)) {
      found_count++;
      continue;
    }

    status = db->Get(read_opts, key, &read_value);
    if (status.ok()) {
      kv_cache.Put(key, read_value);
      found_count++;
    } else if (status.IsNotFound()) {
      not_found_count++;
    } else {
      read_error_count++;
    }
  }
  end_time = std::chrono::high_resolution_clock::now();

  std::chrono::duration<double> run_duration = end_time - start_time;
  std::cout << "\n========== Point Get 测试报告 ==========\n";
  std::cout << "Get 总耗时: " << run_duration.count() << " 秒\n";
  std::cout << "Get QPS: " << NUM_GETS / run_duration.count() << " gets/sec\n";
  std::cout << "成功读取: " << found_count << ", 未找到: " << not_found_count
            << ", 读错误: " << read_error_count << "\n";

  if (kv_cache.Enabled()) {
    LruKvCache::Stats stats = kv_cache.GetStats();
    size_t lookups = stats.hits + stats.misses;
    double hit_rate =
        lookups == 0 ? 0.0 : static_cast<double>(stats.hits) / lookups;
    std::cout << "KV Cache: size=" << kv_cache.Size() << "/"
              << kv_cache.Capacity() << ", hits=" << stats.hits
              << ", misses=" << stats.misses << ", hit_rate=" << hit_rate
              << ", inserts=" << stats.inserts
              << ", evictions=" << stats.evictions << "\n";
  }

  std::cout << "Block Cache total charge: " << block_cache->TotalCharge()
            << " bytes\n";

  delete db;
  return read_error_count == 0 ? 0 : 1;
}
