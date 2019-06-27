// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <string>

namespace rocksdb {

class Slice;

// A Comparator object provides a total order across slices that are
// used as keys in an sstable or a database.  A Comparator implementation
// must be thread-safe since rocksdb may invoke its methods concurrently
// from multiple threads.
/*
以下类继承该类
C.cc (db):struct rocksdb_comparator_t : public Comparator {
Column_family_test.cc (db):class TestComparator : public Comparator {
Comparator.cc (util):class BytewiseComparatorImpl : public Comparator {
Comparator_db_test.cc (db):class DoubleComparator : public Comparator {
Comparator_db_test.cc (db):class HashComparator : public Comparator {
Comparator_db_test.cc (db):class TwoStrComparator : public Comparator {
Dbformat.h (db):    : public Comparator {
Db_compaction_test.cc (db):  class ShortKeyComparator : public Comparator {
Db_sanity_test.cc (tools):  class NewComparator : public Comparator {
Db_test.cc (db):  class NewComparator : public Comparator {
Db_test.cc (db):  class NumberComparator : public Comparator {
File_indexer_test.cc (db):class IntComparator : public Comparator {
Prefix_test.cc (db):class TestKeyComparator : public Comparator {
Table_test.cc (table):class ReverseKeyComparator : public Comparator {
Testutil.cc (util):class Uint64ComparatorImpl : public Comparator {
Testutil.h (util):class SimpleSuffixReverseComparator : public Comparator {
Transaction_test.cc (utilities\transactions):class ThreeBytewiseComparator : public Comparator {
User_comparator_wrapper.h (util):class UserComparatorWrapper final : public Comparator {
InternalKeyComparator
BytewiseComparatorImpl
*/
//实现类如上
class Comparator {
 public:
  virtual ~Comparator() {}

  // Three-way comparison.  Returns value:
  //   < 0 iff "a" < "b",
  //   == 0 iff "a" == "b",
  //   > 0 iff "a" > "b"
  virtual int Compare(const Slice& a, const Slice& b) const = 0;

  // Compares two slices for equality. The following invariant should always
  // hold (and is the default implementation):
  //   Equal(a, b) iff Compare(a, b) == 0
  // Overwrite only if equality comparisons can be done more efficiently than
  // three-way comparisons.
  virtual bool Equal(const Slice& a, const Slice& b) const {
    return Compare(a, b) == 0;
  }

  // The name of the comparator.  Used to check for comparator
  // mismatches (i.e., a DB created with one comparator is
  // accessed using a different comparator.
  //
  // The client of this package should switch to a new name whenever
  // the comparator implementation changes in a way that will cause
  // the relative ordering of any two keys to change.
  //
  // Names starting with "rocksdb." are reserved and should not be used
  // by any clients of this package.
  //获取comparator的名字
  virtual const char* Name() const = 0;

  // Advanced functions: these are used to reduce the space requirements
  // for internal data structures like index blocks.

  // If *start < limit, changes *start to a short string in [start,limit).
  // Simple comparator implementations may return with *start unchanged,
  // i.e., an implementation of this method that does nothing is correct.
  /*
  这两个函数作用是减少像index blocks这样的内部数据结构占用的空间。  
  如果*start < limit，就在[start,limit)中找到一个短字符串，并赋给*start返回。  
  当然返回的*start可能没变化（*start==limit），此时这个函数相当于啥都没干，这也是正确的。
  */
  virtual void FindShortestSeparator(std::string* start,
                                     const Slice& limit) const = 0;

  // Changes *key to a short string >= *key.
  // Simple comparator implementations may return with *key unchanged,
  // i.e., an implementation of this method that does nothing is correct.
  virtual void FindShortSuccessor(std::string* key) const = 0;

  // if it is a wrapped comparator, may return the root one.
  // return itself it is not wrapped.
  virtual const Comparator* GetRootComparator() const { return this; }

  // given two keys, determine if t is the successor of s
  virtual bool IsSameLengthImmediateSuccessor(const Slice& /*s*/,
                                              const Slice& /*t*/) const {
    return false;
  }

  // return true if two keys with different byte sequences can be regarded
  // as equal by this comparator.
  // The major use case is to determine if DataBlockHashIndex is compatible
  // with the customized comparator.
  virtual bool CanKeysWithDifferentByteContentsBeEqual() const { return true; }
};

// Return a builtin comparator that uses lexicographic byte-wise
// ordering.  The result remains the property of this module and
// must not be deleted.
extern const Comparator* BytewiseComparator();

// Return a builtin comparator that uses reverse lexicographic byte-wise
// ordering.
extern const Comparator* ReverseBytewiseComparator();

}  // namespace rocksdb
