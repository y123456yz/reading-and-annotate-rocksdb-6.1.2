//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#include "db/dbformat.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <stdio.h>
#include "monitoring/perf_context_imp.h"
#include "port/port.h"
#include "util/coding.h"
#include "util/string_util.h"

namespace rocksdb {

// kValueTypeForSeek defines the ValueType that should be passed when
// constructing a ParsedInternalKey object for seeking to a particular
// sequence number (since we sort sequence numbers in decreasing order
// and the value type is embedded as the low 8 bits in the sequence
// number in internal keys, we need to use the highest-numbered
// ValueType, not the lowest).
const ValueType kValueTypeForSeek = kTypeBlobIndex;
const ValueType kValueTypeForSeekForPrev = kTypeDeletion;


/*
先将seq左移8位，然后和t进行或操作，相当于把t放到了seq的低8为。为什么seq要小于等于kMaxSequenceNumber呢。


因为kMaxSequenceNumber的值如下所示。
typedef uint64_t SequenceNumber;

// We leave eight bits empty at the bottom so a type and sequence#
// can be packed together into 64-bits.
//0x1ull:u-unsigned 无符号；l-long 长整型，ll就是64位整型。整个0x1ull代表的含义是无符号64位整型常量1，用16进制表示。
static const SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

用二进制表示就是：0000 0000 1111 1111 1111 1111 1111 1111。如果seq大于kMaxSequenceNumber，左移8位的话会移出界。
*/
uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
  assert(seq <= kMaxSequenceNumber);
  assert(IsExtendedValueType(t));
  return (seq << 8) | t;
}

EntryType GetEntryType(ValueType value_type) {
  switch (value_type) {
    case kTypeValue:
      return kEntryPut;
    case kTypeDeletion:
      return kEntryDelete;
    case kTypeSingleDeletion:
      return kEntrySingleDelete;
    case kTypeMerge:
      return kEntryMerge;
    case kTypeRangeDeletion:
      return kEntryRangeDeletion;
    case kTypeBlobIndex:
      return kEntryBlobIndex;
    default:
      return kEntryOther;
  }
}

bool ParseFullKey(const Slice& internal_key, FullKey* fkey) {
  ParsedInternalKey ikey;
  if (!ParseInternalKey(internal_key, &ikey)) {
    return false;
  }
  fkey->user_key = ikey.user_key;
  fkey->sequence = ikey.sequence;
  fkey->type = GetEntryType(ikey.type);
  return true;
}

void UnPackSequenceAndType(uint64_t packed, uint64_t* seq, ValueType* t) {
  *seq = packed >> 8;
  *t = static_cast<ValueType>(packed & 0xff);

  assert(*seq <= kMaxSequenceNumber);
  assert(IsExtendedValueType(*t));
}

//  Inline bool ParseInternalKey()将internal_key（Slice）解析出来为result
//  AppendInternalKey() 将key（ParsedInternalKey）序列化为result（Internel key）

//AppendInternalKey函数先把user_key添加到*result中，然后用PackSequenceAndType函数将sequence和type打包，并将打包的结果添加到*result中。
void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

void AppendInternalKeyFooter(std::string* result, SequenceNumber s,
                             ValueType t) {
  PutFixed64(result, PackSequenceAndType(s, t));
}

std::string ParsedInternalKey::DebugString(bool hex) const {
  char buf[50];
  snprintf(buf, sizeof(buf), "' seq:%" PRIu64 ", type:%d", sequence,
           static_cast<int>(type));
  std::string result = "'";
  result += user_key.ToString(hex);
  result += buf;
  return result;
}

std::string InternalKey::DebugString(bool hex) const {
  std::string result;
  ParsedInternalKey parsed;
  if (ParseInternalKey(rep_, &parsed)) {
    result = parsed.DebugString(hex);
  } else {
    result = "(bad)";
    result.append(EscapeString(rep_));
  }
  return result;
}

const char* InternalKeyComparator::Name() const { return name_.c_str(); }
/*
在RocksDB内部是如何组织的。在RocksDB中的不同版本的key是按照下面的逻辑进行排序:
increasing user key (according to user-supplied comparator)
decreasing sequence number
decreasing type (though sequence# should be enough to disambiguate)
*/
/*
1）首先比较user_key，如果user_key不相同，就直接返回比较结果，否则继续进行第二步。user_comparator_是用户指定的比较器，在InternalKeyComparator构造时传入。
2）在user_key相同的情况下，比较sequence_numer|value type然后返回结果(注意每个Internal Key的sequence_number是唯一的，因此不可能出现anum==bnum的情况)
*/
//查找的地方见FindGreaterOrEqual
int InternalKeyComparator::Compare(const ParsedInternalKey& a,
                                   const ParsedInternalKey& b) const {
  // Order by:
  //    increasing user key (according to user-supplied comparator)
  //    decreasing sequence number
  //    decreasing type (though sequence# should be enough to disambiguate)
  int r = user_comparator_.Compare(a.user_key, b.user_key);
  //user key相同，比较sequence和type
  //当Key相同时，按照seq的降序，如果seq相同则按照type的降序
  if (r == 0) {
    if (a.sequence > b.sequence) {
      r = -1;
    } else if (a.sequence < b.sequence) {
      r = +1;
    } else if (a.type > b.type) {
      r = -1;
    } else if (a.type < b.type) {
      r = +1;
    }
  }
  return r;
}

/*
1）该函数取出Internal Key中的user_key字段，根据用户指定的comparator找到短字符串并替换user_start。
此时user_start物理上是变短了，但是逻辑上却变大了，详见BytewiseComparatorImpl

2）如果user_start被替换了，就用新的user_start更新Internal Key，并使用最大的sequence number。否则start保持不变。
*/
void InternalKeyComparator::FindShortestSeparator(std::string* start,
                                                  const Slice& limit) const {
  // Attempt to shorten the user portion of the key
  Slice user_start = ExtractUserKey(*start);
  Slice user_limit = ExtractUserKey(limit);
  std::string tmp(user_start.data(), user_start.size());
  user_comparator_.FindShortestSeparator(&tmp, user_limit);
  if (tmp.size() <= user_start.size() &&
    user_comparator_.Compare(user_start, tmp) < 0) {
		// if user key在物理上长度变短了，但其逻辑值变大了.生产新的*start时，	
		// 使用最大的sequence number，以保证排在相同user key记录序列的第一个	
		
	    // User key has become shorter physically, but larger logically.
	    // Tack on the earliest possible number to the shortened user key.
	    PutFixed64(&tmp,
	               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
	    assert(this->Compare(*start, tmp) < 0);
	    assert(this->Compare(tmp, limit) < 0);
	    start->swap(tmp);
  }
}

void InternalKeyComparator::FindShortSuccessor(std::string* key) const {
  Slice user_key = ExtractUserKey(*key);
  std::string tmp(user_key.data(), user_key.size());
  user_comparator_.FindShortSuccessor(&tmp);
  if (tmp.size() <= user_key.size() &&
      user_comparator_.Compare(user_key, tmp) < 0) {
    // User key has become shorter physically, but larger logically.
    // Tack on the earliest possible number to the shortened user key.
    PutFixed64(&tmp,
               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    assert(this->Compare(*key, tmp) < 0);
    key->swap(tmp);
  }
}

LookupKey::LookupKey(const Slice& _user_key, SequenceNumber s) {
  size_t usize = _user_key.size();
  size_t needed = usize + 13;  // A conservative estimate
  char* dst;
  if (needed <= sizeof(space_)) {
    dst = space_;
  } else {
    dst = new char[needed];
  }

  //start_指向size部分
  start_ = dst;
  // NOTE: We don't support users keys of more than 2GB :)
  dst = EncodeVarint32(dst, static_cast<uint32_t>(usize + 8));

  //kstart_指向_user_key数据部分
  kstart_ = dst;
  memcpy(dst, _user_key.data(), usize);
  dst += usize;

  //紧跟后面的是SequenceNumber+type，type用kValueTypeForSeek
  EncodeFixed64(dst, PackSequenceAndType(s, kValueTypeForSeek));
  dst += 8;
  end_ = dst;
}

void IterKey::EnlargeBuffer(size_t key_size) {
  // If size is smaller than buffer size, continue using current buffer,
  // or the static allocated one, as default
  assert(key_size > buf_size_);
  // Need to enlarge the buffer.
  ResetBuffer();
  buf_ = new char[key_size];
  buf_size_ = key_size;
}
}  // namespace rocksdb
