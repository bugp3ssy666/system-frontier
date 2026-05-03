#ifndef LEVELDB_TASK3_KV_CACHE_H_
#define LEVELDB_TASK3_KV_CACHE_H_

#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

// 简单的应用层 LRU KV Cache。
//
// 缓存粒度是完整的 key/value，而不是 LevelDB 内部的数据块。
// 使用方式：
// 1. Get 时先查这个 cache。
// 2. 未命中再查 LevelDB。
// 3. LevelDB 查到后，把结果放回这个 cache。
class LruKvCache {
 public:
  struct Stats {
    size_t hits = 0;
    size_t misses = 0;
    size_t inserts = 0;
    size_t evictions = 0;
  };

  explicit LruKvCache(size_t capacity) : capacity_(capacity) {}

  bool Enabled() const { return capacity_ > 0; }

  bool Get(const std::string& key, std::string* value) {
    if (capacity_ == 0) {
      return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto it = table_.find(key);
    if (it == table_.end()) {
      stats_.misses++;
      return false;
    }

    // 命中后，把该节点移到链表头部，表示最近使用。
    items_.splice(items_.begin(), items_, it->second);
    *value = it->second->second;
    stats_.hits++;
    return true;
  }

  void Put(const std::string& key, const std::string& value) {
    if (capacity_ == 0) {
      return;
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto it = table_.find(key);
    if (it != table_.end()) {
      // 已存在：更新 value，并移动到链表头部。
      it->second->second = value;
      items_.splice(items_.begin(), items_, it->second);
      return;
    }

    // 新插入：放到链表头部。
    items_.push_front(std::make_pair(key, value));
    table_[key] = items_.begin();
    stats_.inserts++;

    // 超过容量时，删除链表尾部，也就是最久未使用的元素。
    if (table_.size() > capacity_) {
      const std::string old_key = items_.back().first;
      table_.erase(old_key);
      items_.pop_back();
      stats_.evictions++;
    }
  }

  void Erase(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = table_.find(key);
    if (it == table_.end()) {
      return;
    }

    items_.erase(it->second);
    table_.erase(it);
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    items_.clear();
    table_.clear();
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return table_.size();
  }

  size_t Capacity() const { return capacity_; }

  Stats GetStats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
  }

 private:
  typedef std::list<std::pair<std::string, std::string> > ListType;

  size_t capacity_;
  mutable std::mutex mu_;
  ListType items_;
  std::unordered_map<std::string, ListType::iterator> table_;
  Stats stats_;
};

#endif  // LEVELDB_TASK3_KV_CACHE_H_
