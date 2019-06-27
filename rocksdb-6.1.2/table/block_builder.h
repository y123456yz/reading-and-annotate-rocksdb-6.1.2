//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once
#include <vector>

#include <stdint.h>
#include "rocksdb/slice.h"
#include "rocksdb/table.h"
#include "table/data_block_hash_index.h"

namespace rocksdb {
/* 一条记录的KV格式如下
//例如last key="abcxxxxx"  key为"abcssss"，则"abc可以共用"， 
//shared=3，key后面的"ssss"四个字符串就不能共用，non_shared=4


// An entry for a particular key-value pair has the form:
//     shared_bytes: varint32
//     unshared_bytes: varint32
//     value_length: varint32
//     key_delta: char[unshared_bytes]
//     value: char[value_length]
// shared_bytes == 0 for restart points.
//
// The trailer of the block has the form:
//     restarts: uint32[num_restarts]
//     num_restarts: uint32
// restarts[i] contains the offset within the block of the ith restart point.

*/

//block data的管理是读写分离的，读取后的遍历查询操作由Block类实现，block data的构建则由BlockBuilder类实现
//图表记录详见https://blog.csdn.net/caoshangpa/article/details/78977743
class BlockBuilder {
 public:
  BlockBuilder(const BlockBuilder&) = delete;
  void operator=(const BlockBuilder&) = delete;

  explicit BlockBuilder(int block_restart_interval,
                        bool use_delta_encoding = true,
                        bool use_value_delta_encoding = false,
                        BlockBasedTableOptions::DataBlockIndexType index_type =
                            BlockBasedTableOptions::kDataBlockBinarySearch,
                        double data_block_hash_table_util_ratio = 0.75);

  // Reset the contents as if the BlockBuilder was just constructed.
  void Reset();// 重置BlockBuilder

  // REQUIRES: Finish() has not been called since the last call to Reset().
  // REQUIRES: key is larger than any previously added key
  
  // Add的调用应该在Reset之后，在Finish之前。
  // Add只添加KV对（一条记录）,重启点信息部分由Finish添加。
  // 每次调用Add时，key应该越来越大。
  void Add(const Slice& key, const Slice& value,
           const Slice* const delta_value = nullptr);

  // Finish building the block and return a slice that refers to the
  // block contents.  The returned slice will remain valid for the
  // lifetime of this builder or until Reset() is called.
  // 组建block data完成，返回block data
  Slice Finish();

  // Returns an estimate of the current (uncompressed) size of the block
  // we are building.
  // 估算当前block data的大小
  inline size_t CurrentSizeEstimate() const {
    return estimate_ + (data_block_hash_index_builder_.Valid()
                            ? data_block_hash_index_builder_.EstimateSize()
                            : 0);
  }

  // Returns an estimated block size after appending key and value.
  size_t EstimateSizeAfterKV(const Slice& key, const Slice& value) const;

  // Return true iff no entries have been added since the last Reset()
  // 是否已经开始组建block data
  bool empty() const { return buffer_.empty(); }

 private:
  //block_restart_interval表示当前重启点（其实也是一条记录）和上个重启点之间间隔了多少条记录
  const int block_restart_interval_;
  // TODO(myabandeh): put it into a separate IndexBlockBuilder
  const bool use_delta_encoding_;
  // Refer to BlockIter::DecodeCurrentValue for format of delta encoded values
  const bool use_value_delta_encoding_;

  /* 一个问题，既然通过Comparator可以极大的节省key的存储空间，那为什么又要使用重启点机制来额外占用一下空间呢？
  这是因为如果最开头的记录数据损坏，其后的所有记录都将无法恢复。为了降低这个风险，引入了重启点，每隔固定
  条数记录会强制加入一个重启点，这个位置的Entry会完整的记录自己的Key。
  */

  // 用于存放block data 
  // block的内容
  std::string buffer_;              // Destination buffer
  // 用于存放重启点的位置信息  restarts_[i]存储的是block的第i个重启点的偏移。
  //很明显第一个k/v对，总是第一个重启点，也就是restarts[0] = 0;
  std::vector<uint32_t> restarts_;  // Restart points
  // buffer大小 +重启点数组长度 + 重启点长度(uint32)  
  size_t estimate_;
  // 从上个重启点遍历到下个重启点时的计数
  int counter_;    // Number of entries emitted since restart
  // 是否调用了Finish
  bool finished_;  // Has Finish() been called?
  // 记录最后Add的key 
  // 记录最后添加的key  
  std::string last_key_;
  DataBlockHashIndexBuilder data_block_hash_index_builder_;
};

}  // namespace rocksdb
