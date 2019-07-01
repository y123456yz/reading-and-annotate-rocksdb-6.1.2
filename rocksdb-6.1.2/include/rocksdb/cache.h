// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A Cache is an interface that maps keys to values.  It has internal
// synchronization and may be safely accessed concurrently from
// multiple threads.  It may automatically evict entries to make room
// for new entries.  Values have a specified charge against the cache
// capacity.  For example, a cache where the values are variable
// length strings, may use the length of the string as the charge for
// the string.
//
// A builtin cache implementation with a least-recently-used eviction
// policy is provided.  Clients may use their own implementations if
// they want something more sophisticated (like scan-resistance, a
// custom eviction policy, variable cache sizing, etc.)

#pragma once

#include <stdint.h>
#include <memory>
#include <string>
#include "rocksdb/memory_allocator.h"
#include "rocksdb/slice.h"
#include "rocksdb/statistics.h"
#include "rocksdb/status.h"

namespace rocksdb {

class Cache;

extern const bool kDefaultToAdaptiveMutex;

struct LRUCacheOptions {
  // Capacity of the cache.
  size_t capacity = 0;

  // Cache is sharded into 2^num_shard_bits shards,
  // by hash of key. Refer to NewLRUCache for further
  // information.
  int num_shard_bits = -1;

  // If strict_capacity_limit is set,
  // insert to the cache will fail when cache is full.
  bool strict_capacity_limit = false;

  // Percentage of cache reserved for high priority entries.
  // If greater than zero, the LRU list will be split into a high-pri
  // list and a low-pri list. High-pri entries will be insert to the
  // tail of high-pri list, while low-pri entries will be first inserted to
  // the low-pri list (the midpoint). This is refered to as
  // midpoint insertion strategy to make entries never get hit in cache
  // age out faster.
  //
  // See also
  // BlockBasedTableOptions::cache_index_and_filter_blocks_with_high_priority.
  double high_pri_pool_ratio = 0.0;

  // If non-nullptr will use this allocator instead of system allocator when
  // allocating memory for cache blocks. Call this method before you start using
  // the cache!
  //
  // Caveat: when the cache is used as block cache, the memory allocator is
  // ignored when dealing with compression libraries that allocate memory
  // internally (currently only XPRESS).
  std::shared_ptr<MemoryAllocator> memory_allocator;

  // Whether to use adaptive mutexes for cache shards. Note that adaptive
  // mutexes need to be supported by the platform in order for this to have any
  // effect. The default value is true if RocksDB is compiled with
  // -DROCKSDB_DEFAULT_TO_ADAPTIVE_MUTEX, false otherwise.
  bool use_adaptive_mutex = kDefaultToAdaptiveMutex;

  LRUCacheOptions() {}
  LRUCacheOptions(size_t _capacity, int _num_shard_bits,
                  bool _strict_capacity_limit, double _high_pri_pool_ratio,
                  std::shared_ptr<MemoryAllocator> _memory_allocator = nullptr,
                  bool _use_adaptive_mutex = kDefaultToAdaptiveMutex)
      : capacity(_capacity),
        num_shard_bits(_num_shard_bits),
        strict_capacity_limit(_strict_capacity_limit),
        high_pri_pool_ratio(_high_pri_pool_ratio),
        memory_allocator(std::move(_memory_allocator)),
        use_adaptive_mutex(_use_adaptive_mutex) {}
};

// Create a new cache with a fixed size capacity. The cache is sharded
// to 2^num_shard_bits shards, by hash of the key. The total capacity
// is divided and evenly assigned to each shard. If strict_capacity_limit
// is set, insert to the cache will fail when cache is full. User can also
// set percentage of the cache reserves for high priority entries via
// high_pri_pool_pct.
// num_shard_bits = -1 means it is automatically determined: every shard
// will be at least 512KB and number of shard bits will not exceed 6.
extern std::shared_ptr<Cache> NewLRUCache(
    size_t capacity, int num_shard_bits = -1,
    bool strict_capacity_limit = false, double high_pri_pool_ratio = 0.0,
    std::shared_ptr<MemoryAllocator> memory_allocator = nullptr,
    bool use_adaptive_mutex = kDefaultToAdaptiveMutex);

extern std::shared_ptr<Cache> NewLRUCache(const LRUCacheOptions& cache_opts);

// Similar to NewLRUCache, but create a cache based on CLOCK algorithm with
// better concurrent performance in some cases. See util/clock_cache.cc for
// more detail.
//
// Return nullptr if it is not supported.
extern std::shared_ptr<Cache> NewClockCache(size_t capacity,
                                            int num_shard_bits = -1,
                                            bool strict_capacity_limit = false);

/*
为了加快查找速度，LevelDB在内存中采用Cache的方式，在table中采用bloom filter的方式，尽最大可能加快随机读操作。
Cache分为两种，分别是table cache和block cache。
1.table cache
    table cache缓存的是table的索引数据，类似于文件系统中对inode的缓存。table cache默认大小是1000，注意此处缓存
的是1000个table文件的索引信息，而不是1000个字节。table cache的大小由options.max_open_files确定，其最小值为20-10，
最大值为50000－10。
2.block cache
    block cache是缓存的block数据，block是table文件内组织数据的单位，也是从持久化存储中读取和写入的单位。由于table
是按照key有序分布的，因此一个block内的数据也是按照key紧邻排布的（有序依照使用者传入的比较函数，默认按照字典序），
类似于Linux中的page cache。block默认大小为4k，由LevelDB调用open函数打开数据库时时传入的options.block_size参数指定。
代码中限制的block最小大小为1k，最大大小为4M。对于频繁做scan操作的应用，可适当调大此参数，对大量小value随机读取的应用，
也可尝试调小该参数。block cache默认实现是一个8M大小的ShardedLRUCache，此参数由options.block_cache设定。当然也可根据应
用需求，提供自定义的缓存策略。注意，此处的大小是未压缩的block大小。
*/

//LRUCache继承ShardedCache，ShardedCache继承Cache
//可以参考https://blog.csdn.net/caoshangpa/article/details/78960999
//DBImpl.table_cache_
class Cache {
 public:
  // Depending on implementation, cache entries with high priority could be less
  // likely to get evicted than low priority entries.
  enum class Priority { HIGH, LOW };

  Cache(std::shared_ptr<MemoryAllocator> allocator = nullptr)
      : memory_allocator_(std::move(allocator)) {}

  // Destroys all existing entries by calling the "deleter"
  // function that was passed via the Insert() function.
  //
  // @See Insert
  virtual ~Cache() {}

  // Opaque handle to an entry stored in the cache.
  struct Handle {};

  // The type of the Cache
  virtual const char* Name() const = 0;

  // Insert a mapping from key->value into the cache and assign it
  // the specified charge against the total cache capacity.
  // If strict_capacity_limit is true and cache reaches its full capacity,
  // return Status::Incomplete.
  //
  // If handle is not nullptr, returns a handle that corresponds to the
  // mapping. The caller must call this->Release(handle) when the returned
  // mapping is no longer needed. In case of error caller is responsible to
  // cleanup the value (i.e. calling "deleter").
  //
  // If handle is nullptr, it is as if Release is called immediately after
  // insert. In case of error value will be cleanup.
  //
  // When the inserted entry is no longer needed, the key and
  // value will be passed to "deleter".
  // 插入键值对，并指定占用的缓存大小（charge），返回被插入的结点。
  // 如果插入的键值对用不到了，传给deleter函数。
  // 如果返回值用不到了，记得调用this->Release(handle)释放。
  virtual Status Insert(const Slice& key, void* value, size_t charge,
                        void (*deleter)(const Slice& key, void* value),
                        Handle** handle = nullptr,
                        Priority priority = Priority::LOW) = 0;

  // If the cache has no mapping for "key", returns nullptr.
  //
  // Else return a handle that corresponds to the mapping.  The caller
  // must call this->Release(handle) when the returned mapping is no
  // longer needed.
  // If stats is not nullptr, relative tickers could be used inside the
  // function.

  
  // 如果没找到结点，返回NULL。否则返回找到的结点。
  // 如果返回值用不到了，记得调用this->Release(handle)释放。
  virtual Handle* Lookup(const Slice& key, Statistics* stats = nullptr) = 0;

  // Increments the reference count for the handle if it refers to an entry in
  // the cache. Returns true if refcount was incremented; otherwise, returns
  // false.
  // REQUIRES: handle must have been returned by a method on *this.
  virtual bool Ref(Handle* handle) = 0;

  /**
   * Release a mapping returned by a previous Lookup(). A released entry might
   * still  remain in cache in case it is later looked up by others. If
   * force_erase is set then it also erase it from the cache if there is no
   * other reference to  it. Erasing it should call the deleter function that
   * was provided when the
   * entry was inserted.
   *
   * Returns true if the entry was also erased.
   */
  // REQUIRES: handle must not have been released yet.
  // REQUIRES: handle must have been returned by a method on *this.
  // 释放结点，注意不要重复释放。
  virtual bool Release(Handle* handle, bool force_erase = false) = 0;

  // Return the value encapsulated in a handle returned by a
  // successful Lookup().
  // REQUIRES: handle must not have been released yet.
  // REQUIRES: handle must have been returned by a method on *this.
  // 返回结点handle中的Value值，注意判断handle的有效性。
  virtual void* Value(Handle* handle) = 0;

  // If the cache contains entry for key, erase it.  Note that the
  // underlying entry will be kept around until all existing handles
  // to it have been released.

  // 删除包含key的结点
  virtual void Erase(const Slice& key) = 0;
  // Return a new numeric id.  May be used by multiple clients who are
  // sharding the same cache to partition the key space.  Typically the
  // client will allocate a new id at startup and prepend the id to
  // its cache keys.
  // 返回数字ID，用于处理多线程同时访问缓存时的同步
  virtual uint64_t NewId() = 0;

  // sets the maximum configured capacity of the cache. When the new
  // capacity is less than the old capacity and the existing usage is
  // greater than new capacity, the implementation will do its best job to
  // purge the released entries from the cache in order to lower the usage
  virtual void SetCapacity(size_t capacity) = 0;

  // Set whether to return error on insertion when cache reaches its full
  // capacity.
  virtual void SetStrictCapacityLimit(bool strict_capacity_limit) = 0;

  // Get the flag whether to return error on insertion when cache reaches its
  // full capacity.
  virtual bool HasStrictCapacityLimit() const = 0;

  // returns the maximum configured capacity of the cache
  virtual size_t GetCapacity() const = 0;

  // returns the memory size for the entries residing in the cache.
  virtual size_t GetUsage() const = 0;

  // returns the memory size for a specific entry in the cache.
  virtual size_t GetUsage(Handle* handle) const = 0;

  // returns the memory size for the entries in use by the system
  virtual size_t GetPinnedUsage() const = 0;

  // Call this on shutdown if you want to speed it up. Cache will disown
  // any underlying data and will not free it on delete. This call will leak
  // memory - call this only if you're shutting down the process.
  // Any attempts of using cache after this call will fail terribly.
  // Always delete the DB object before calling this method!
  virtual void DisownData(){
      // default implementation is noop
  };

  // Apply callback to all entries in the cache
  // If thread_safe is true, it will also lock the accesses. Otherwise, it will
  // access the cache without the lock held
  virtual void ApplyToAllCacheEntries(void (*callback)(void*, size_t),
                                      bool thread_safe) = 0;

  // Remove all entries.
  // Prerequisite: no entry is referenced.
  virtual void EraseUnRefEntries() = 0;

  virtual std::string GetPrintableOptions() const { return ""; }

  // Mark the last inserted object as being a raw data block. This will be used
  // in tests. The default implementation does nothing.
  virtual void TEST_mark_as_data_block(const Slice& /*key*/,
                                       size_t /*charge*/) {}

  MemoryAllocator* memory_allocator() const { return memory_allocator_.get(); }

 private:
  // No copying allowed
  Cache(const Cache&);
  Cache& operator=(const Cache&);

  std::shared_ptr<MemoryAllocator> memory_allocator_;
};

}  // namespace rocksdb
