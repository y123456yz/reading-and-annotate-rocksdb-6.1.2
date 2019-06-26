//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <stdint.h>
#include <string>
#include <utility>
#include <vector>
#include "db/dbformat.h"
#include "db/table_properties_collector.h"
#include "options/cf_options.h"
#include "rocksdb/options.h"
#include "rocksdb/table_properties.h"
#include "util/file_reader_writer.h"

namespace rocksdb {

class Slice;
class Status;

struct TableReaderOptions {
  // @param skip_filters Disables loading/accessing the filter block
  TableReaderOptions(const ImmutableCFOptions& _ioptions,
                     const SliceTransform* _prefix_extractor,
                     const EnvOptions& _env_options,
                     const InternalKeyComparator& _internal_comparator,
                     bool _skip_filters = false, bool _immortal = false,
                     int _level = -1)
      : TableReaderOptions(_ioptions, _prefix_extractor, _env_options,
                           _internal_comparator, _skip_filters, _immortal,
                           _level, 0 /* _largest_seqno */) {}

  // @param skip_filters Disables loading/accessing the filter block
  TableReaderOptions(const ImmutableCFOptions& _ioptions,
                     const SliceTransform* _prefix_extractor,
                     const EnvOptions& _env_options,
                     const InternalKeyComparator& _internal_comparator,
                     bool _skip_filters, bool _immortal, int _level,
                     SequenceNumber _largest_seqno)
      : ioptions(_ioptions),
        prefix_extractor(_prefix_extractor),
        env_options(_env_options),
        internal_comparator(_internal_comparator),
        skip_filters(_skip_filters),
        immortal(_immortal),
        level(_level),
        largest_seqno(_largest_seqno) {}

  const ImmutableCFOptions& ioptions;
  const SliceTransform* prefix_extractor;
  const EnvOptions& env_options;
  const InternalKeyComparator& internal_comparator;
  // This is only used for BlockBasedTable (reader)
  bool skip_filters;
  // Whether the table will be valid as long as the DB is open
  bool immortal;
  // what level this table/file is on, -1 for "not set, don't know"
  int level;
  // largest seqno in the table
  SequenceNumber largest_seqno;
};

struct TableBuilderOptions {
  TableBuilderOptions(
      const ImmutableCFOptions& _ioptions, const MutableCFOptions& _moptions,
      const InternalKeyComparator& _internal_comparator,
      const std::vector<std::unique_ptr<IntTblPropCollectorFactory>>*
          _int_tbl_prop_collector_factories,
      CompressionType _compression_type, uint64_t _sample_for_compression,
      const CompressionOptions& _compression_opts, bool _skip_filters,
      const std::string& _column_family_name, int _level,
      const uint64_t _creation_time = 0, const int64_t _oldest_key_time = 0,
      const uint64_t _target_file_size = 0)
      : ioptions(_ioptions),
        moptions(_moptions),
        internal_comparator(_internal_comparator),
        int_tbl_prop_collector_factories(_int_tbl_prop_collector_factories),
        compression_type(_compression_type),
        sample_for_compression(_sample_for_compression),
        compression_opts(_compression_opts),
        skip_filters(_skip_filters),
        column_family_name(_column_family_name),
        level(_level),
        creation_time(_creation_time),
        oldest_key_time(_oldest_key_time),
        target_file_size(_target_file_size) {}
  const ImmutableCFOptions& ioptions;
  const MutableCFOptions& moptions;
  const InternalKeyComparator& internal_comparator;
  const std::vector<std::unique_ptr<IntTblPropCollectorFactory>>*
      int_tbl_prop_collector_factories;
  CompressionType compression_type;
  uint64_t sample_for_compression;
  const CompressionOptions& compression_opts;
  bool skip_filters;  // only used by BlockBasedTableBuilder
  const std::string& column_family_name;
  int level; // what level this table/file is on, -1 for "not set, don't know"
  const uint64_t creation_time;
  const int64_t oldest_key_time;
  const uint64_t target_file_size;
};

// TableBuilder provides the interface used to build a Table
// (an immutable and sorted map from keys to values).
//
// Multiple threads can invoke const methods on a TableBuilder without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same TableBuilder must use
// external synchronization.
/*
Table也叫SSTable（Sorted String Table），是数据在.sst文件中的存储形式,rocksdb通过TableBuilder类来构建每一个.sst文件。Table的逻辑结构如下所示，
包括存储数据的Block，存储索引信息的Block，存储Filter的Block：

Footer：为于Table尾部，记录指向Metaindex Block的Handle和指向Index Block的Handle。需要说明的是Table中
所有的Handle是通过偏移量Offset以及Size一同来表示的，用来指明所指向的Block位置。Footer是SST文件解析开始的地方，
通过Footer中记录的这两个关键元信息Block的位置，可以方便的开启之后的解析工作。另外Footer种还记录了用于验证文
件是否为合法SST文件的常数值Magicnum。

Index Block：记录Data Block位置信息的Block，其中的每一条Entry指向一个Data Block，其Key值为所指向的Data Block最
后一条数据的Key，Value为指向该Data Block位置的Handle。

Metaindex Block：与Index Block类似，由一组Handle组成，不同的是这里的Handle指向的Meta Block。

Data Block：以Key-Value的方式存储实际数据，其中Key定义为：
 DataBlock Key := UserKey + SequenceNum + Type
 Type := kDelete or kValue
 对比Memtable中的Key，可以发现Data Block中的Key并没有拼接UserKey的长度在UserKey前，这是由于物理结构
 中已经有了Key的长度信息。

Meta Block：比较特殊的Block，用来存储元信息，目前rocksdb使用的仅有对布隆过滤器的存储。写入Data Block的数据会
同时更新对应Meta Block中的过滤器。读取数据时也会首先经过布隆过滤器（Bloom Filter）过滤BloomFilter——大规模数据处理利器(https://blog.csdn.net/caoshangpa/article/details/79049678)。
Meta Block的物理结构也与其他Block有所不同：
[filter 0] 
[filter 1]  
[filter 2]  
...  
[filter N-1]  
[offset of filter 0] : 4 bytes  
[offset of filter 1] : 4 bytes  
[offset of filter 2] : 4 bytes  
...  
[offset of filter N-1] : 4 bytes  
[offset of beginning of offset array] : 4 bytes  
lg(base) : 1 byte
其中每个filter节对应一段Key Range，落在某个Key Range的Key需要到对应的filter节中查找自己的过滤信息，base指定这个Range的大小。
*/
//Rep中不仅接管了各种Block的生成细节，而且还会记录生成Block需要的一些统计信息。因此我们可以认为，TableBuilder只不过是对Block的一层浅封装，真正做事情的是Rep。
//BlockBasedTableBuilder继承该类
class TableBuilder {
 public:
  // REQUIRES: Either Finish() or Abandon() has been called.
  virtual ~TableBuilder() {}

  // Add key,value to the table being constructed.
  // REQUIRES: key is after any previously added key according to comparator.
  // REQUIRES: Finish(), Abandon() have not been called
  //TableBuilder中的Add函数本质上不过是对Rep中BlockBuilder的Add函数的调用。例如BlockBasedTableBuilder::Add
  virtual void Add(const Slice& key, const Slice& value) = 0;

  // Return non-ok iff some error has been detected.
  virtual Status status() const = 0;

  // Finish building the table.
  // REQUIRES: Finish(), Abandon() have not been called
  virtual Status Finish() = 0;

  // Indicate that the contents of this builder should be abandoned.
  // If the caller is not going to call Finish(), it must call Abandon()
  // before destroying this builder.
  // REQUIRES: Finish(), Abandon() have not been called
  virtual void Abandon() = 0;

  // Number of calls to Add() so far.
  virtual uint64_t NumEntries() const = 0;

  // Size of the file generated so far.  If invoked after a successful
  // Finish() call, returns the size of the final generated file.
  virtual uint64_t FileSize() const = 0;

  // If the user defined table properties collector suggest the file to
  // be further compacted.
  virtual bool NeedCompact() const { return false; }

  // Returns table properties
  virtual TableProperties GetTableProperties() const = 0;
};

}  // namespace rocksdb
