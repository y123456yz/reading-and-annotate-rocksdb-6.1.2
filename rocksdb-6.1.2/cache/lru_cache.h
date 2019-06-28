//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#pragma once

#include <string>

#include "cache/sharded_cache.h"

#include "port/port.h"
#include "util/autovector.h"

namespace rocksdb {

// LRU cache implementation

// An entry is a variable length heap-allocated structure.
// Entries are referenced by cache and/or by any external entity.
// The cache keeps all its entries in table. Some elements
// are also stored on LRU list.
//
// LRUHandle can be in these states:
// 1. Referenced externally AND in hash table.
//  In that case the entry is *not* in the LRU. (refs > 1 && in_cache == true)
// 2. Not referenced externally and in hash table. In that case the entry is
// in the LRU and can be freed. (refs == 1 && in_cache == true)
// 3. Referenced externally and not in hash table. In that case the entry is
// in not on LRU and not in table. (refs >= 1 && in_cache == false)
//
// All newly created LRUHandles are in state 1. If you call
// LRUCacheShard::Release
// on entry in state 1, it will go into state 2. To move from state 1 to
// state 3, either call LRUCacheShard::Erase or LRUCacheShard::Insert with the
// same key.
// To move from state 2 to state 1, use LRUCacheShard::Lookup.
// Before destruction, make sure that no handles are in state 1. This means
// that any successful LRUCacheShard::Lookup/LRUCacheShard::Insert have a
// matching
// RUCache::Release (to move into state 2) or LRUCacheShard::Erase (for state 3)

/*  LRUHandle为什么会被同时置于哈希表和双向链表之中？
注意看LookUp的实现，如果单纯使用链表，则仅能提供O(n)的查询效率，所以在查询时，利用了哈希表实现O(1)的查询。
那么，如果单纯使用哈希表呢？虽然可以实现O(1)的查询，但却无法更新缓存节点的访问时间。这是因为链表可以按照
固定的顺序被遍历，而哈希表中的节点无法提供固定的遍历顺序（考虑Resize前后）。那么，可不可以将访问时间记录
在Handle中，然后仅用哈希表，这样既可以实现O(1)的查询，又可以方便地更新缓存记录的访问时间，岂不美哉？但是，
如果没有按访问时间排序的链表，当触发缓存回收时，我们如何快速定位到哪些缓存记录要被回收呢？链表O(n)的查询效率、
哈希表不支持排序，两种数据结构单独都无法满足这里的需求。作者大神巧妙地将二者结合，取长补短，利用哈希表实现O(1)
的查询，利用链表维持对缓存记录按访问时间排序	

        查询	插入	删除	排序
链表	O(n)	O(1)	O(1)	支持
哈希表	O(1)	O(1)	O(1)	不支持

注1：哈希表实现O(1)操作的前提是：平均每哈希桶元素数 <= 1
注2：为了保持平均哈希桶元素数，必要时必须Resize，而Resize后，原有顺序将被打破
*/

//一个LRUHandle就是一个结点，这个结构体设计的巧妙之处在于，它既可以作为HashTable中的结点
//LRUHandleTable.list_成员
struct LRUHandle { //成员赋值见LRUCacheShard::Insert
  void* value;
  //删除器。当refs == 0时，调用deleter完成value对象释放。
  void (*deleter)(const Slice&, void* value);
  // 作为HashTable中的节点，指向hash值相同的节点（解决hash冲突采用链地址法），单向链表，用户解决相同hash的KV节点冲突，相同hash值的通过该单项链表管理起来
  //赋值参考LRUHandleTable::Insert  LRUHandleTable::Resize
  LRUHandle* next_hash; 

  //LRUHandle由hash桶和list一起管理，一个用于LRU淘汰用，一个用于快速查找，但是节点LRUHandle是共用的

  //LRU相关，链表头实际上为LRUCacheShard.lru_成员，可以参考https://blog.csdn.net/caoshangpa/article/details/78783749
  // 作为LRUCache中的节点，指向后继
  LRUHandle* next;
  // 作为LRUCache中的节点，指向前驱
  LRUHandle* prev;
   // 用户指定占用缓存的大小
  size_t charge;  // TODO(opt): Only allow uint32_t?
  size_t key_length;//key_length key长度,key数据其实地址key_data
  /* 1.为何要维护引用计数？
    在Insert时，引用计数初始化为2，一个是给LRUCache自身析构时用，一个是给外部调用Release或Erase时用。Insert时，返回
  的是新插入的结点，插入完成后，需要调用该结点Release或Erase方法将引用计数减1，那么此时该结点的引用计数就是1了。
  在LRUCache析构时，会先将结点的引用计数再减1，如果此时引用计数为0，则调用deleter，并将该结点彻底从内存中free。在
  Lookup时，如果查找接结点存在，此时引用计数会加1，也即是变成了3。此时，在用户持有该结点的期间，该缓存可能被删除（多
  种原因，如：超过缓存容量触发回收、具有相同key的新缓存插入、整个缓存被析构等），导致用户访问到非法内存，程序崩溃。
  因此，需要使用引用计数要加1来维护结点的生命周期。因为Lookup返回的是找到的结点，用户在查找完成后，要主动调用该结点
  的Release或Erase来使引用计数从新变成2。
  */
  // 引用计数
  uint32_t refs;     // a number of refs to this entry
                     // cache itself is counted as 1

  // Include the following flags:
  //   IN_CACHE:         whether this entry is referenced by the hash table.
  //   IS_HIGH_PRI:      whether this entry is high priority entry.
  //   IN_HIGH_PRI_POOL: whether this entry is in high-pri pool.
  //   HAS_HIT:          whether this entry has had any lookups (hits).
  enum Flags : uint8_t {
    IN_CACHE = (1 << 0),
    IS_HIGH_PRI = (1 << 1),
    IN_HIGH_PRI_POOL = (1 << 2),
    HAS_HIT = (1 << 3),
  };

  uint8_t flags;
  // 哈希值
  uint32_t hash;     // Hash of key(); used for fast sharding and comparisons
  //key_length key长度,key数据其实地址key_data
  char key_data[1];  // Beginning of key

  Slice key() const {
    // For cheaper lookups, we allow a temporary Handle object
    // to store a pointer to a key in "value".
    if (next == this) {
      return *(reinterpret_cast<Slice*>(value));
    } else {
      return Slice(key_data, key_length);
    }
  }

  bool InCache() const { return flags & IN_CACHE; }
  bool IsHighPri() const { return flags & IS_HIGH_PRI; }
  bool InHighPriPool() const { return flags & IN_HIGH_PRI_POOL; }
  bool HasHit() const { return flags & HAS_HIT; }

  void SetInCache(bool in_cache) {
    if (in_cache) {
      flags |= IN_CACHE;
    } else {
      flags &= ~IN_CACHE;
    }
  }

  void SetPriority(Cache::Priority priority) {
    if (priority == Cache::Priority::HIGH) {
      flags |= IS_HIGH_PRI;
    } else {
      flags &= ~IS_HIGH_PRI;
    }
  }

  void SetInHighPriPool(bool in_high_pri_pool) {
    if (in_high_pri_pool) {
      flags |= IN_HIGH_PRI_POOL;
    } else {
      flags &= ~IN_HIGH_PRI_POOL;
    }
  }

  void SetHit() { flags |= HAS_HIT; }

  void Free() {
    assert((refs == 1 && InCache()) || (refs == 0 && !InCache()));
    if (deleter) {
      (*deleter)(key(), value);
    }
    delete[] reinterpret_cast<char*>(this);
  }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.

//LRUCacheShard.table_成员
class LRUHandleTable {
 public:
  LRUHandleTable();
  ~LRUHandleTable();

  LRUHandle* Lookup(const Slice& key, uint32_t hash);
  LRUHandle* Insert(LRUHandle* h);
  LRUHandle* Remove(const Slice& key, uint32_t hash);

  template <typename T>
  void ApplyToAllCacheEntries(T func) {
    for (uint32_t i = 0; i < length_; i++) {
      LRUHandle* h = list_[i];
      while (h != nullptr) {
        auto n = h->next_hash;
        assert(h->InCache());
        func(h);
        h = n;
      }
    }
  }

 private:
  // Return a pointer to slot that points to a cache entry that
  // matches key/hash.  If there is no such cache entry, return a
  // pointer to the trailing slot in the corresponding linked list.
  LRUHandle** FindPointer(const Slice& key, uint32_t hash);

  void Resize();

  // The table consists of an array of buckets where each bucket is
  // a linked list of cache entries that hash into the bucket.
  //hash桶的链表成员赋值LRUHandleTable::Resize
   //哈希地址数组的二级指针
  LRUHandle** list_; 
  //哈希地址数组的长度
  uint32_t length_;  
  //哈希表中所有结点的总数
  uint32_t elems_;  
};

// A single shard of sharded cache.
//LRUCache.shards_成员
class ALIGN_AS(CACHE_LINE_SIZE) LRUCacheShard final : public CacheShard {
 public:
  LRUCacheShard(size_t capacity, bool strict_capacity_limit,
                double high_pri_pool_ratio, bool use_adaptive_mutex);
  virtual ~LRUCacheShard();

  // Separate from constructor so caller can easily make an array of LRUCache
  // if current usage is more than new capacity, the function will attempt to
  // free the needed space
  virtual void SetCapacity(size_t capacity) override;

  // Set the flag to reject insertion if cache if full.
  virtual void SetStrictCapacityLimit(bool strict_capacity_limit) override;

  // Set percentage of capacity reserved for high-pri cache entries.
  void SetHighPriorityPoolRatio(double high_pri_pool_ratio);

  // Like Cache methods, but with an extra "hash" parameter.
  virtual Status Insert(const Slice& key, uint32_t hash, void* value,
                        size_t charge,
                        void (*deleter)(const Slice& key, void* value),
                        Cache::Handle** handle,
                        Cache::Priority priority) override;
  virtual Cache::Handle* Lookup(const Slice& key, uint32_t hash) override;
  virtual bool Ref(Cache::Handle* handle) override;
  virtual bool Release(Cache::Handle* handle,
                       bool force_erase = false) override;
  virtual void Erase(const Slice& key, uint32_t hash) override;

  // Although in some platforms the update of size_t is atomic, to make sure
  // GetUsage() and GetPinnedUsage() work correctly under any platform, we'll
  // protect them with mutex_.

  virtual size_t GetUsage() const override;
  virtual size_t GetPinnedUsage() const override;

  virtual void ApplyToAllCacheEntries(void (*callback)(void*, size_t),
                                      bool thread_safe) override;

  virtual void EraseUnRefEntries() override;

  virtual std::string GetPrintableOptions() const override;

  void TEST_GetLRUList(LRUHandle** lru, LRUHandle** lru_low_pri);

  //  Retrieves number of elements in LRU, for unit test purpose only
  //  not threadsafe
  size_t TEST_GetLRUSize();

  //  Retrives high pri pool ratio
  double GetHighPriPoolRatio();

 private:
  void LRU_Remove(LRUHandle* e);
  void LRU_Insert(LRUHandle* e);

  // Overflow the last entry in high-pri pool to low-pri pool until size of
  // high-pri pool is no larger than the size specify by high_pri_pool_pct.
  void MaintainPoolSize();

  // Just reduce the reference count by 1.
  // Return true if last reference
  bool Unref(LRUHandle* e);

  // Free some space following strict LRU policy until enough space
  // to hold (usage_ + charge) is freed or the lru list is empty
  // This function is not thread safe - it needs to be executed while
  // holding the mutex_
  void EvictFromLRU(size_t charge, autovector<LRUHandle*>* deleted);

  // Initialized before use.
  size_t capacity_;

  // Memory size for entries in high-pri pool.
  size_t high_pri_pool_usage_;

  // Whether to reject insertion if cache reaches its full capacity.
  bool strict_capacity_limit_;

  // Ratio of capacity reserved for high priority cache entries.
  double high_pri_pool_ratio_;

  // High-pri pool size, equals to capacity * high_pri_pool_ratio.
  // Remember the value to avoid recomputing each time.
  double high_pri_pool_capacity_;

  // Dummy head of LRU list.
  // lru.prev is newest entry, lru.next is oldest entry.
  // LRU contains items which can be evicted, ie reference only by cache
  LRUHandle lru_;

  // Pointer to head of low-pri pool in LRU list.
  LRUHandle* lru_low_pri_;

  // ------------^^^^^^^^^^^^^-----------
  // Not frequently modified data members
  // ------------------------------------
  //
  // We separate data members that are updated frequently from the ones that
  // are not frequently updated so that they don't share the same cache line
  // which will lead into false cache sharing
  //
  // ------------------------------------
  // Frequently modified data members
  // ------------vvvvvvvvvvvvv-----------
  //一个LRUCacheShard类对应一个 LRU hash桶
  LRUHandleTable table_;

  // Memory size for entries residing in the cache
  size_t usage_;

  // Memory size for entries residing only in the LRU list
  size_t lru_usage_;

  // mutex_ protects the following state.
  // We don't count mutex_ as the cache's internal state so semantically we
  // don't mind mutex_ invoking the non-const actions.
  mutable port::Mutex mutex_;
};

//rocksdb中使用了一种基于LRUCache的缓存机制，用于缓存：
// 已打开的sstable文件对象和相关元数据；
// sstable中的dataBlock的内容；使得在发生读取热数据时，尽量在cache中命中，避免IO读取。

//rocksdb利用上述的cache结构来缓存数据。其中：

// cache：来缓存已经被打开的sstable文件句柄以及元数据（默认上限为500个）；
// bcache：来缓存被读过的sstable中dataBlock的数据（默认上限为8MB）;
// 
//当一个sstable文件需要被打开时，首先从cache中寻找是否已经存在相关的文件句柄，若存在则无需重复打开；若不存在，则从打开相关文件，并将（1）indexBlock数据，（2）metaIndexBlock数据等相关元数据进行预读。


//在rocksdb中所有KV数据都是存储在Memtable，Immutable Memtable和SSTable中
//LRUCache针对sstable文件的查找，memtable针对Memtable和Immutable Memtable


//LRUCache继承ShardedCache，ShardedCache继承Cache
//图解参考https://blog.csdn.net/caoshangpa/article/details/78960999
class LRUCache
#ifdef NDEBUG
    final
#endif
    : public ShardedCache {
 public:
  LRUCache(size_t capacity, int num_shard_bits, bool strict_capacity_limit,
           double high_pri_pool_ratio,
           std::shared_ptr<MemoryAllocator> memory_allocator = nullptr,
           bool use_adaptive_mutex = kDefaultToAdaptiveMutex);
  virtual ~LRUCache();
  virtual const char* Name() const override { return "LRUCache"; }
  virtual CacheShard* GetShard(int shard) override;
  virtual const CacheShard* GetShard(int shard) const override;
  virtual void* Value(Handle* handle) override;
  virtual size_t GetCharge(Handle* handle) const override;
  virtual uint32_t GetHash(Handle* handle) const override;
  virtual void DisownData() override;

  //  Retrieves number of elements in LRU, for unit test purpose only
  size_t TEST_GetLRUSize();
  //  Retrives high pri pool ratio
  double GetHighPriPoolRatio();

 private:
 
/*  
SharedLRUCache到底是什么呢？
    我们为什么需要它？这是因为levelDB是多线程的，每个线程访问缓冲区的时候都会将缓冲区锁住，为了多线程访问，尽可能快速，
减少锁开销，ShardedLRUCache内部有16个LRUCache，查找Key时首先计算key属于哪一个分片，分片的计算方法是取32位hash值的高4位，
然后在相应的LRUCache中进行查找，这样就大大减少了多线程的访问锁的开销。
*/
 //LRUCache::LRUCache
  LRUCacheShard* shards_ = nullptr;
  //多少个LRU hash桶
  int num_shards_ = 0;
};

}  // namespace rocksdb
