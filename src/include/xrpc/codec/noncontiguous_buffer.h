#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

// ============================================================================
// NoncontiguousBuffer — 零拷贝友好的链式 Buffer
// ============================================================================
// 设计思路：
//   内部用 deque<Block> 实现链式存储，每个 Block 持有独立内存块。
//   Append 时新建 Block，不需拷贝旧数据（区别于单一 std::string）。
//   Skip 时只移动 Block 内 offset，旧 Block 引用计数归零后自动释放。
//
// 当前版本：简化实现，后续可升级为 iovec 链 + 外部分配器。
// ============================================================================

class NoncontiguousBuffer {
 public:
  NoncontiguousBuffer() = default;

  // 从连续内存构造（会拷贝）
  explicit NoncontiguousBuffer(const char* data, size_t len) { Append(data, len); }
  explicit NoncontiguousBuffer(const std::string& s) : NoncontiguousBuffer(s.data(), s.size()) {}

  // ---- 容量 ----
  size_t ReadableBytes() const { return readable_; }

  // ---- 写入 ----
  void Append(const void* data, size_t len);
  void Append(const std::string& s) { Append(s.data(), s.size()); }

  // ---- 读取（不消耗数据） ----
  size_t Peek(void* buf, size_t len, size_t offset = 0) const;

  // ---- 消耗数据 ----
  void Skip(size_t len);

  // ---- 导出 ----
  std::string ToString() const;

 private:
  struct Block {
    std::vector<char> data;
    size_t offset = 0;  // 已读偏移（用于 Skip）
  };

  std::deque<Block> blocks_;
  size_t readable_ = 0;
};
