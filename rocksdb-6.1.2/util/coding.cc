//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/coding.h"

#include <algorithm>
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"

namespace rocksdb {

// conversion' conversion from 'type1' to 'type2', possible loss of data
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
/*
为什么要把整型（int）编码成变长整型（varint）呢？是为了尽可能的节约存储空间。
varint是一种紧凑的表示数字的方法，它用一个或多个字节来表示一个数字，值越小的数字使用越少的字节数。比如int32类型的数字，
一般需要4个字节。但是采用Varint，对于很小的int32类型的数字，则可以用1个字节来表示。当然凡事都有好的也有不好的一面，
采用varint表示法，大的数字则可能需要5个字节来表示。从统计的角度来说，一般不会所有消息中的数字都是大数，因此大多数情况下，
采用varint后，可以用更小的字节数来表示数字信息。varint中的每个字节的最高位（bit）有特殊含义，如果该位为1，表示后续的字节
也是这个数字的一部分，如果该位为0，则结束。其他的7位（bit）都表示数字。7位能表示的最大数是127，因此小于128的数字都可以用
一个字节表示。大于等于128的数字，比如说300，会用两个字节在内存中表示为：
低         高
1010 1100 0000 0010
实现过程如下：300的二进制为100101100，取低7位也就是010 1100放在内存低字节中，由于第二个字节也是数字的一部分，因此内存低字节
的最高位为1，则完整的内存低字节为1010 1100。300的高2位也就是10放到内存的高字节中，因为数字到该字节结束，因此该字节包括最高
位的其他6位都用0填充，则完整的内存高字节为0000 0010。正常情况下，int需要32位，varint用一个字节的最高位作为标识位，所以，一个
字节只能存储7位，如果整数特别大，可能需要5个字节才能存放（5*8-5（标志位）>32），下面if语句的第五个分支就是处理这种情况。
*/ //参考https://blog.csdn.net/caoshangpa/article/details/78815940
char* EncodeVarint32(char* dst, uint32_t v) {
  // Operate on characters as unsigneds
  unsigned char* ptr = reinterpret_cast<unsigned char*>(dst);
  static const int B = 128;
  if (v < (1 << 7)) {
    *(ptr++) = v;
  } else if (v < (1 << 14)) {
    *(ptr++) = v | B;
    *(ptr++) = v >> 7;
  } else if (v < (1 << 21)) {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = v >> 14;
  } else if (v < (1 << 28)) {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = (v >> 14) | B;
    *(ptr++) = v >> 21;
  } else {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = (v >> 14) | B;
    *(ptr++) = (v >> 21) | B;
    *(ptr++) = v >> 28;
  }
  return reinterpret_cast<char*>(ptr);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

//解码p获取对应的value值，和上面的编码过程对应
const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = *(reinterpret_cast<const unsigned char*>(p));
    p++;
    if (byte & 128) {
      // More bytes are present
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}

//解码p获取对应的value值，和上面的编码过程对应
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = *(reinterpret_cast<const unsigned char*>(p));
    p++;
    if (byte & 128) {
      // More bytes are present
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}

}  // namespace rocksdb
