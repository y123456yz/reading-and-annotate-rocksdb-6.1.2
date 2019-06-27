//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Log format information shared by reader and writer.
// See ../doc/log_format.txt for more detail.

#pragma once
namespace rocksdb {
namespace log {

//rocksdb把日志文件切分成了大小为32KB的连续block块，block由连续的log record组成，log record的格式为：
//  4      2       1        
//CRC32 | LEN | LOG TYPE | DATA

//由于一条logrecord长度最短为7，如果一个block的剩余空间<=6byte，那么将被填充为空字符串，
//另外长度为7的log record是不包括任何用户数据的。
enum RecordType {
  // Zero is reserved for preallocated files
  kZeroType = 0,
  //FULL类型表明该log record包含了完整的record；
  kFullType = 1,

  //record可能内容很多，超过了block的可用大小，就需要分成几条log record，
  //第一条类型为FIRST，中间的为MIDDLE，最后一条为LAST。
  //例子，考虑到如下序列的user records：
  //  A: length 1000
  //  B: length 97270
  //  C: length 8000
  //A作为FULL类型的record存储在第一个block中；B将被拆分成3条log record，分别存储在第1、2、3个block中，
  // For fragments
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,

  // For recycled log files
  kRecyclableFullType = 5,
  kRecyclableFirstType = 6,
  kRecyclableMiddleType = 7,
  kRecyclableLastType = 8,
};
static const int kMaxRecordType = kRecyclableLastType;

//rocksdb把日志文件切分成了大小为32KB的连续block块，block由连续的log record组成，log record的格式为：
//  4      2       1        
//CRC32 | LEN | LOG TYPE | DATA
static const unsigned int kBlockSize = 32768; //32KB

// Header is checksum (4 bytes), length (2 bytes), type (1 byte)
static const int kHeaderSize = 4 + 2 + 1;

// Recyclable header is checksum (4 bytes), length (2 bytes), type (1 byte),
// log number (4 bytes).
static const int kRecyclableHeaderSize = 4 + 2 + 1 + 4;

}  // namespace log
}  // namespace rocksdb
