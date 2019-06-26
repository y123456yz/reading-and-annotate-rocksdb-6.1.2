//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/two_level_iterator.h"
#include "db/pinned_iterators_manager.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "table/block.h"
#include "table/format.h"
#include "util/arena.h"

namespace rocksdb {

namespace {

//NewTwoLevelIterator中new该类，可以参考https://blog.csdn.net/caoshangpa/article/details/79046942
//NewIterator用于创建Table的迭代器，此迭代器是一个双层迭代器
class TwoLevelIndexIterator : public InternalIteratorBase<BlockHandle> {
 public:
  explicit TwoLevelIndexIterator(
      TwoLevelIteratorState* state,
      InternalIteratorBase<BlockHandle>* first_level_iter);

  ~TwoLevelIndexIterator() override {
    first_level_iter_.DeleteIter(false /* is_arena_mode */);
    second_level_iter_.DeleteIter(false /* is_arena_mode */);
    delete state_;
  }

  void Seek(const Slice& target) override;
  void SeekForPrev(const Slice& target) override;
  void SeekToFirst() override;
  void SeekToLast() override;
  void Next() override;
  void Prev() override;

  bool Valid() const override { return second_level_iter_.Valid(); }
  Slice key() const override {
    assert(Valid());
    return second_level_iter_.key();
  }
  BlockHandle value() const override {
    assert(Valid());
    return second_level_iter_.value();
  }
  Status status() const override {
    if (!first_level_iter_.status().ok()) {
      assert(second_level_iter_.iter() == nullptr);
      return first_level_iter_.status();
    } else if (second_level_iter_.iter() != nullptr &&
               !second_level_iter_.status().ok()) {
      return second_level_iter_.status();
    } else {
      return status_;
    }
  }
  void SetPinnedItersMgr(
      PinnedIteratorsManager* /*pinned_iters_mgr*/) override {}
  bool IsKeyPinned() const override { return false; }
  bool IsValuePinned() const override { return false; }

 private:
  void SaveError(const Status& s) {
    if (status_.ok() && !s.ok()) status_ = s;
  }
  void SkipEmptyDataBlocksForward();
  void SkipEmptyDataBlocksBackward();
  void SetSecondLevelIterator(InternalIteratorBase<BlockHandle>* iter);
  void InitDataBlock();

  TwoLevelIteratorState* state_;
  //第一层迭代器，Index Block的block_data字段迭代器的代理
  IteratorWrapperBase<BlockHandle> first_level_iter_;
  //第二层迭代器，Data Block的block_data字段迭代器的代理
  IteratorWrapperBase<BlockHandle> second_level_iter_;  // May be nullptr
  Status status_;
  // If second_level_iter is non-nullptr, then "data_block_handle_" holds the
  // "index_value" passed to block_function_ to create the second_level_iter.
  BlockHandle data_block_handle_; //handle中间变量
};

TwoLevelIndexIterator::TwoLevelIndexIterator(
    TwoLevelIteratorState* state,
    InternalIteratorBase<BlockHandle>* first_level_iter)
    : state_(state), first_level_iter_(first_level_iter) {}

// Index Block的block_data字段中，每一条记录的key都满足：
// 大于上一个Data Block的所有key，并且小于后面所有Data Block的key
// 因为Seek是查找key>=target的第一条记录，所以当index_iter_找到时，
// 该index_inter_对应的data_iter_所管理的Data Block中所有记录的
// key都小于target，需要在下一个Data Block中seek，而下一个Data Block
// 中的第一条记录就满足key>=target
void TwoLevelIndexIterator::Seek(const Slice& target) {
  first_level_iter_.Seek(target);

  InitDataBlock();
   // data_iter_.Seek(target)必然会找不到，此时data_iter_.Valid()为false  
   // 然后调用SkipEmptyDataBlocksForward定位到下一个Data Block，并定位到  
   // 该Data Block的第一条记录，这条记录刚好就是要查找的那条记录
  if (second_level_iter_.iter() != nullptr) {
    second_level_iter_.Seek(target);
  }
  SkipEmptyDataBlocksForward();
}

void TwoLevelIndexIterator::SeekForPrev(const Slice& target) {
  first_level_iter_.Seek(target);
  InitDataBlock();
  if (second_level_iter_.iter() != nullptr) {
    second_level_iter_.SeekForPrev(target);
  }
  if (!Valid()) {
    if (!first_level_iter_.Valid() && first_level_iter_.status().ok()) {
      first_level_iter_.SeekToLast();
      InitDataBlock();
      if (second_level_iter_.iter() != nullptr) {
        second_level_iter_.SeekForPrev(target);
      }
    }
    SkipEmptyDataBlocksBackward();
  }
}


// 因为index_block_options.block_restart_interval = 1
// 所以这里是解析第一个Block Data的第一条记录
void TwoLevelIndexIterator::SeekToFirst() {
  first_level_iter_.SeekToFirst();
  InitDataBlock();
  if (second_level_iter_.iter() != nullptr) {
    second_level_iter_.SeekToFirst();
  }
  SkipEmptyDataBlocksForward();
}


// 因为index_block_options.block_restart_interval = 1
// 所以这里是解析最后一个Block Data的最后一条记录
void TwoLevelIndexIterator::SeekToLast() {
  first_level_iter_.SeekToLast();
  InitDataBlock();
  if (second_level_iter_.iter() != nullptr) {
    second_level_iter_.SeekToLast();
  }
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIndexIterator::Next() {
  assert(Valid());
  second_level_iter_.Next();
  SkipEmptyDataBlocksForward();
}

void TwoLevelIndexIterator::Prev() {
  assert(Valid());
  second_level_iter_.Prev();
  SkipEmptyDataBlocksBackward();
}

// 1.如果data_iter_.iter()为NULL，说明index_iter_.Valid()为为NULL时调用了  
//   SetDataIterator(NULL)，此时直接返回，因为没数据可读啦  
// 2.如果data_iter_.Valid()为false，说明当前Data Block的block_data字段读完啦  
//   开始读下一个Data Block的block_data字段（从block_data第一条记录开始读）
void TwoLevelIndexIterator::SkipEmptyDataBlocksForward() {
  while (second_level_iter_.iter() == nullptr ||
         (!second_level_iter_.Valid() && second_level_iter_.status().ok())) {
    // Move to next block
    if (!first_level_iter_.Valid()) {
      SetSecondLevelIterator(nullptr);
      return;
    }
    first_level_iter_.Next();
    InitDataBlock();
    if (second_level_iter_.iter() != nullptr) {
      second_level_iter_.SeekToFirst();
    }
  }
}

void TwoLevelIndexIterator::SkipEmptyDataBlocksBackward() {
  while (second_level_iter_.iter() == nullptr ||
         (!second_level_iter_.Valid() && second_level_iter_.status().ok())) {
    // Move to next block
    if (!first_level_iter_.Valid()) {
      SetSecondLevelIterator(nullptr);
      return;
    }
    first_level_iter_.Prev();
    InitDataBlock();
    if (second_level_iter_.iter() != nullptr) {
      second_level_iter_.SeekToLast();
    }
  }
}

void TwoLevelIndexIterator::SetSecondLevelIterator(
    InternalIteratorBase<BlockHandle>* iter) {
  InternalIteratorBase<BlockHandle>* old_iter = second_level_iter_.Set(iter);
  delete old_iter;
}

void TwoLevelIndexIterator::InitDataBlock() {
  if (!first_level_iter_.Valid()) {
    SetSecondLevelIterator(nullptr);
  } else {
    BlockHandle handle = first_level_iter_.value();
    if (second_level_iter_.iter() != nullptr &&
        !second_level_iter_.status().IsIncomplete() &&
        handle.offset() == data_block_handle_.offset()) {
       // 如果data_iter_已经创建了，什么都不用干，这可以防止InitDataBlock被多次调用
      // second_level_iter is already constructed with this iterator, so
      // no need to change anything
    } else {
    	// 创建Data Block中block_data字段的迭代器
      InternalIteratorBase<BlockHandle>* iter =
          state_->NewSecondaryIterator(handle);
	 // 将handle转化为data_block_handle_
      data_block_handle_ = handle;
	 // 将iter传给其代理data_inter_
      SetSecondLevelIterator(iter);
    }
  }
}

}  // namespace

//NewIterator用于创建Table的迭代器，此迭代器是一个双层迭代器
InternalIteratorBase<BlockHandle>* NewTwoLevelIterator(
    TwoLevelIteratorState* state,
    InternalIteratorBase<BlockHandle>* first_level_iter) {
  return new TwoLevelIndexIterator(state, first_level_iter);
}
}  // namespace rocksdb
