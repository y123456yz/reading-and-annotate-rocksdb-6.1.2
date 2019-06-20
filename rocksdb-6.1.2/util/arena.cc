//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/arena.h"
#ifdef ROCKSDB_MALLOC_USABLE_SIZE
#ifdef OS_FREEBSD
#include <malloc_np.h>
#else
#include <malloc.h>
#endif
#endif
#ifndef OS_WIN
#include <sys/mman.h>
#endif
#include <algorithm>
#include "port/port.h"
#include "rocksdb/env.h"
#include "util/logging.h"
#include "util/sync_point.h"

namespace rocksdb {

// MSVC complains that it is already defined since it is static in the header.
#ifndef _MSC_VER
const size_t Arena::kInlineSize;
#endif

const size_t Arena::kMinBlockSize = 4096;
const size_t Arena::kMaxBlockSize = 2u << 30;
static const int kAlignUnit = alignof(max_align_t);

size_t OptimizeBlockSize(size_t block_size) {
  // Make sure block_size is in optimal range
  block_size = std::max(Arena::kMinBlockSize, block_size);
  block_size = std::min(Arena::kMaxBlockSize, block_size);

  // make sure block_size is the multiple of kAlignUnit
  if (block_size % kAlignUnit != 0) {
    block_size = (1 + block_size / kAlignUnit) * kAlignUnit;
  }

  return block_size;
}

Arena::Arena(size_t block_size, AllocTracker* tracker, size_t huge_page_size)
    : kBlockSize(OptimizeBlockSize(block_size)), tracker_(tracker) {
  assert(kBlockSize >= kMinBlockSize && kBlockSize <= kMaxBlockSize &&
         kBlockSize % kAlignUnit == 0);
  TEST_SYNC_POINT_CALLBACK("Arena::Arena:0", const_cast<size_t*>(&kBlockSize));
  alloc_bytes_remaining_ = sizeof(inline_block_);
  blocks_memory_ += alloc_bytes_remaining_;
  aligned_alloc_ptr_ = inline_block_;
  unaligned_alloc_ptr_ = inline_block_ + alloc_bytes_remaining_;
#ifdef MAP_HUGETLB
  hugetlb_size_ = huge_page_size;
  if (hugetlb_size_ && kBlockSize > hugetlb_size_) {
    hugetlb_size_ = ((kBlockSize - 1U) / hugetlb_size_ + 1U) * hugetlb_size_;
  }
#else
  (void)huge_page_size;
#endif
  if (tracker_ != nullptr) {
    tracker_->Allocate(kInlineSize);
  }
}

Arena::~Arena() {
  if (tracker_ != nullptr) {
    assert(tracker_->is_freed());
    tracker_->FreeMem();
  }
  for (const auto& block : blocks_) {
    delete[] block;
  }

#ifdef MAP_HUGETLB
  for (const auto& mmap_info : huge_blocks_) {
    if (mmap_info.addr_ == nullptr) {
      continue;
    }
    auto ret = munmap(mmap_info.addr_, mmap_info.length_);
    if (ret != 0) {
      // TODO(sdong): Better handling
    }
  }
#endif
}

char* Arena::AllocateFallback(size_t bytes, bool aligned) {
  // 如果需求的内存大于内存块中剩余的内存，而且大于1K，则给这内存单独分配一块bytes大小的内存。  
  // 这样可以避免浪费过多的空间（因为如果bytes大于1K也从4K的内存块去取用，那么如果当前内存块中刚好剩余  
  // 1K，只能再新建一个4K的内存块，并且取用bytes。此时新建的内存块是当前内存块，后续操作都是基于当前内  
  // 存块的，那么原内存块中的1K空间就浪费了）
  if (bytes > kBlockSize / 4) {
    ++irregular_block_num;
    // Object is more than a quarter of our block size.  Allocate it separately
    // to avoid wasting too much space in leftover bytes.
    return AllocateNewBlock(bytes); //对于超过1k的请求，直接new一个指定大小的内存块并返回
  }

  // We waste the remaining space in the current block.
  size_t size = 0;
  char* block_head = nullptr;
#ifdef MAP_HUGETLB
  if (hugetlb_size_) {
    size = hugetlb_size_;
    block_head = AllocateFromHugePage(size);
  }
#endif
  //小于1K的请求，则申请一个新的4k内存块，从中分配一部分给用户。
  if (!block_head) {
    size = kBlockSize;
    block_head = AllocateNewBlock(size);
  }
  alloc_bytes_remaining_ = size - bytes;

  if (aligned) {
    aligned_alloc_ptr_ = block_head + bytes;
    unaligned_alloc_ptr_ = block_head + size;
    return block_head;
  } else {
    aligned_alloc_ptr_ = block_head;
    unaligned_alloc_ptr_ = block_head + size - bytes;
    return unaligned_alloc_ptr_;
  }
}

char* Arena::AllocateFromHugePage(size_t bytes) {
#ifdef MAP_HUGETLB
  if (hugetlb_size_ == 0) {
    return nullptr;
  }
  // Reserve space in `huge_blocks_` before calling `mmap`.
  // Use `emplace_back()` instead of `reserve()` to let std::vector manage its
  // own memory and do fewer reallocations.
  //
  // - If `emplace_back` throws, no memory leaks because we haven't called
  //   `mmap` yet.
  // - If `mmap` throws, no memory leaks because the vector will be cleaned up
  //   via RAII.
  huge_blocks_.emplace_back(nullptr /* addr */, 0 /* length */);

  void* addr = mmap(nullptr, bytes, (PROT_READ | PROT_WRITE),
                    (MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB), -1, 0);

  if (addr == MAP_FAILED) {
    return nullptr;
  }
  huge_blocks_.back() = MmapInfo(addr, bytes);
  blocks_memory_ += bytes;
  if (tracker_ != nullptr) {
    tracker_->Allocate(bytes);
  }
  return reinterpret_cast<char*>(addr);
#else
  (void)bytes;
  return nullptr;
#endif
}

// 提供了字节对齐内存分配，一般情况是4字节或8个字节对齐分配，
// 对齐内存的好处简单的说就是加速内存访问。
// 首先获取一个指针的大小const int align = sizeof(void*)，
// 很明显，在32位系统下是4 ,64位系统下是8 ，为了表述方便，我们假设是32位系统，即align ＝ 4,
// 然后将我们使用的char * 指针地址转换为一个无符号整型(reinterpret_cast<uintptr_t>(result):
// It is an unsigned int that is guaranteed to be the same size as a pointer.)，通过与操作来
// 获取size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align-1);当前指针模4
// 的值，有了这个值以后，我们就容易知道，还差 slop = align - current_mod多个字节，内存才是对齐的，
// 所以有了result = alloc_ptr + slop。那么获取bytes大小的内存，实际上需要的大小为needed = bytes + slop。

//内存对其原因参考https://blog.csdn.net/caoshangpa/article/details/78841715
char* Arena::AllocateAligned(size_t bytes, size_t huge_page_size,
                             Logger* logger) {
  assert((kAlignUnit & (kAlignUnit - 1)) ==
         0);  // Pointer size should be a power of 2

#ifdef MAP_HUGETLB
  if (huge_page_size > 0 && bytes > 0) {
    // Allocate from a huge page TBL table.
    assert(logger != nullptr);  // logger need to be passed in.
    size_t reserved_size =
        ((bytes - 1U) / huge_page_size + 1U) * huge_page_size;
    assert(reserved_size >= bytes);

    char* addr = AllocateFromHugePage(reserved_size);
    if (addr == nullptr) {
      ROCKS_LOG_WARN(logger,
                     "AllocateAligned fail to allocate huge TLB pages: %s",
                     strerror(errno));
      // fail back to malloc
    } else {
      return addr;
    }
  }
#else
  (void)huge_page_size;
  (void)logger;
#endif

  size_t current_mod =
      reinterpret_cast<uintptr_t>(aligned_alloc_ptr_) & (kAlignUnit - 1);
  size_t slop = (current_mod == 0 ? 0 : kAlignUnit - current_mod);
  size_t needed = bytes + slop;
  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = aligned_alloc_ptr_ + slop;
    aligned_alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // AllocateFallback always returns aligned memory
    result = AllocateFallback(bytes, true /* aligned */);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (kAlignUnit - 1)) == 0);
  return result;
}

// 分配新的内存块  Arena::~Arena中释放
char* Arena::AllocateNewBlock(size_t block_bytes) {
  // Reserve space in `blocks_` before allocating memory via new.
  // Use `emplace_back()` instead of `reserve()` to let std::vector manage its
  // own memory and do fewer reallocations.
  //
  // - If `emplace_back` throws, no memory leaks because we haven't called `new`
  //   yet.
  // - If `new` throws, no memory leaks because the vector will be cleaned up
  //   via RAII.
  blocks_.emplace_back(nullptr);

  char* block = new char[block_bytes];
  size_t allocated_size;
#ifdef ROCKSDB_MALLOC_USABLE_SIZE
  allocated_size = malloc_usable_size(block);
#ifndef NDEBUG
  // It's hard to predict what malloc_usable_size() returns.
  // A callback can allow users to change the costed size.
  std::pair<size_t*, size_t*> pair(&allocated_size, &block_bytes);
  TEST_SYNC_POINT_CALLBACK("Arena::AllocateNewBlock:0", &pair);
#endif  // NDEBUG
#else
  allocated_size = block_bytes;
#endif  // ROCKSDB_MALLOC_USABLE_SIZE
  blocks_memory_ += allocated_size;
  if (tracker_ != nullptr) {
    tracker_->Allocate(allocated_size);
  }
  blocks_.back() = block;
  return block;
}

}  // namespace rocksdb
