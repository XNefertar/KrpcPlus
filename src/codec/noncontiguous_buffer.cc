#include "xrpc/codec/noncontiguous_buffer.h"

#include <algorithm>
#include <cstring>

void NoncontiguousBuffer::Append(const void* data, size_t len) {
  if (len == 0) return;
  const char* ptr = static_cast<const char*>(data);

  // 尝试追加到最后一个 Block（避免碎片）
  if (!blocks_.empty()) {
    Block& last = blocks_.back();
    size_t remaining = last.data.size() - (last.offset + last.data.size() - last.offset);
    // 简化: 如果最后 block 还没被读过, 就地扩容
    if (last.offset == 0 && last.data.size() < 4096) {
      size_t old_size = last.data.size();
      last.data.resize(old_size + len);
      std::memcpy(last.data.data() + old_size, ptr, len);
      readable_ += len;
      return;
    }
  }

  // 新建 Block
  Block block;
  block.data.assign(ptr, ptr + len);
  block.offset = 0;
  blocks_.push_back(std::move(block));
  readable_ += len;
}

size_t NoncontiguousBuffer::Peek(void* buf, size_t len, size_t offset) const {
  if (offset >= readable_) return 0;
  size_t to_read = std::min(len, readable_ - offset);
  char* dst = static_cast<char*>(buf);
  size_t copied = 0;
  size_t skip = offset;

  for (const auto& block : blocks_) {
    size_t block_size = block.data.size() - block.offset;
    if (skip >= block_size) {
      skip -= block_size;
      continue;
    }
    size_t from_block = std::min(to_read - copied, block_size - skip);
    std::memcpy(dst + copied, block.data.data() + block.offset + skip, from_block);
    copied += from_block;
    skip = 0;
    if (copied >= to_read) break;
  }
  return copied;
}

void NoncontiguousBuffer::Skip(size_t len) {
  if (len >= readable_) {
    blocks_.clear();
    readable_ = 0;
    return;
  }
  readable_ -= len;
  while (len > 0 && !blocks_.empty()) {
    Block& front = blocks_.front();
    size_t block_avail = front.data.size() - front.offset;
    if (len >= block_avail) {
      len -= block_avail;
      blocks_.pop_front();
    } else {
      front.offset += len;
      len = 0;
    }
  }
}

std::string NoncontiguousBuffer::ToString() const {
  std::string result;
  result.resize(readable_);
  Peek(&result[0], readable_);
  return result;
}
