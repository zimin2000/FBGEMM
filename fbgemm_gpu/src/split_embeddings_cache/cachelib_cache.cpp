/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "fbgemm_gpu/split_embeddings_cache/cachelib_cache.h"
#include "fbgemm_gpu/split_embeddings_cache/kv_db_cpp_utils.h"
#include "fbgemm_gpu/utils/dispatch_macros.h"

namespace l2_cache {

using Cache = facebook::cachelib::LruAllocator;

// this is a general predictor for weights data type, might not be general
// enough for all the cases
at::ScalarType bytes_to_dtype(int num_bytes) {
  switch (num_bytes) {
    case 1:
      return at::kByte;
    case 2:
      return at::kHalf;
    case 4:
      return at::kFloat;
    case 8:
      return at::kDouble;
    default:
      throw std::runtime_error("Unsupported dtype");
  }
}

CacheLibCache::CacheLibCache(const CacheConfig& cache_config)
    : cache_config_(cache_config),
      cache_(initializeCacheLib(cache_config_)),
      admin_(createCacheAdmin(*cache_)) {
  for (size_t i = 0; i < cache_config_.num_shards; i++) {
    pool_ids_.push_back(cache_->addPool(
        fmt::format("shard_{}", i),
        cache_->getCacheMemoryStats().ramCacheSize / cache_config_.num_shards));
  }
}

std::unique_ptr<Cache> CacheLibCache::initializeCacheLib(
    const CacheConfig& config) {
  auto eviction_cb =
      [this](const facebook::cachelib::LruAllocator::RemoveCbData& data) {
        FBGEMM_DISPATCH_FLOAT_HALF_AND_BYTE(
            evicted_weights_ptr_->scalar_type(), "l2_eviction_handling", [&] {
              if (data.context ==
                  facebook::cachelib::RemoveContext::kEviction) {
                auto indices_data_ptr =
                    evicted_indices_ptr_->data_ptr<int64_t>();
                auto weights_data_ptr =
                    evicted_weights_ptr_->data_ptr<scalar_t>();
                auto row_id = eviction_row_id++;
                auto weight_dim = evicted_weights_ptr_->size(1);
                const auto key_ptr =
                    reinterpret_cast<const int64_t*>(data.item.getKey().data());
                indices_data_ptr[row_id] = *key_ptr;

                std::copy(
                    reinterpret_cast<const scalar_t*>(data.item.getMemory()),
                    reinterpret_cast<const scalar_t*>(data.item.getMemory()) +
                        weight_dim,
                    &weights_data_ptr[row_id * weight_dim]); // dst_start
              }
            });
      };
  Cache::Config cacheLibConfig;
  cacheLibConfig.setCacheSize(static_cast<uint64_t>(config.cache_size_bytes))
      .setRemoveCallback(eviction_cb)
      .setCacheName("TBEL2Cache")
      .setAccessConfig({25 /* bucket power */, 10 /* lock power */})
      .setFullCoredump(false)
      .validate();
  return std::make_unique<Cache>(cacheLibConfig);
}

std::unique_ptr<facebook::cachelib::CacheAdmin> CacheLibCache::createCacheAdmin(
    Cache& cache) {
  facebook::cachelib::CacheAdmin::Config adminConfig;
  adminConfig.oncall = "mvai";
  return std::make_unique<facebook::cachelib::CacheAdmin>(
      cache, std::move(adminConfig));
}

std::optional<void*> CacheLibCache::get(int64_t key) {
  auto key_str =
      folly::StringPiece(reinterpret_cast<const char*>(&key), sizeof(int64_t));
  auto item = cache_->find(key_str);
  if (!item) {
    return std::nullopt;
  }
  return const_cast<void*>(item->getMemory());
}

size_t CacheLibCache::get_shard_id(int64_t key) {
  return kv_db_utils::hash_shard(key, pool_ids_.size());
}

facebook::cachelib::PoolId CacheLibCache::get_pool_id(int64_t key) {
  return pool_ids_[get_shard_id(key)];
}

bool CacheLibCache::put(int64_t key, const at::Tensor& data) {
  auto key_str =
      folly::StringPiece(reinterpret_cast<const char*>(&key), sizeof(int64_t));
  auto item = cache_->allocate(get_pool_id(key), key_str, data.nbytes());
  if (!item) {
    XLOG(ERR) << fmt::format("Failed to allocate item {} in cache, skip", key);
    return false;
  }
  std::memcpy(item->getMemory(), data.data_ptr(), data.nbytes());
  cache_->insertOrReplace(std::move(item));
  return true;
}

std::tuple<at::Tensor, at::Tensor, at::Tensor> CacheLibCache::get_all_items() {
  int total_num_items = 0;
  for (auto& pool_id : pool_ids_) {
    total_num_items += cache_->getPoolStats(pool_id).numItems();
  }
  auto weight_dim = cache_config_.max_D_;
  auto weights_dtype =
      bytes_to_dtype(cache_config_.item_size_bytes / weight_dim);
  auto indices = at::empty(
      total_num_items, at::TensorOptions().dtype(at::kLong).device(at::kCPU));
  auto weights = at::empty(
      {total_num_items, weight_dim},
      at::TensorOptions().dtype(weights_dtype).device(at::kCPU));
  FBGEMM_DISPATCH_FLOAT_HALF_AND_BYTE(
      weights.scalar_type(), "get_all_items", [&] {
        auto indices_data_ptr = indices.data_ptr<int64_t>();
        auto weights_data_ptr = weights.data_ptr<scalar_t>();
        int64_t item_idx = 0;
        for (auto itr = cache_->begin(); itr != cache_->end(); ++itr) {
          const auto key_ptr =
              reinterpret_cast<const int64_t*>(itr->getKey().data());
          indices_data_ptr[item_idx] = *key_ptr;
          std::copy(
              reinterpret_cast<const scalar_t*>(itr->getMemory()),
              reinterpret_cast<const scalar_t*>(itr->getMemory()) + weight_dim,
              &weights_data_ptr[item_idx * weight_dim]); // dst_start
          item_idx++;
        }
        CHECK_EQ(total_num_items, item_idx);
      });
  return std::make_tuple(
      indices,
      weights,
      at::tensor(
          {total_num_items},
          at::TensorOptions().dtype(at::kLong).device(at::kCPU)));
}

void CacheLibCache::init_tensor_for_l2_eviction(
    const at::Tensor& indices,
    const at::Tensor& weights,
    const at::Tensor& count) {
  auto num_lookups = count.item<long>();
  evicted_indices_ptr_ = std::make_shared<at::Tensor>(
      at::ones(
          num_lookups,
          at::TensorOptions().device(indices.device()).dtype(indices.dtype())) *
      -1);
  evicted_weights_ptr_ = std::make_shared<at::Tensor>(at::empty(
      {num_lookups, weights.size(1)},
      at::TensorOptions().device(weights.device()).dtype(weights.dtype())));
}

void CacheLibCache::reset_eviction_states() {
  eviction_row_id = 0;
}

folly::Optional<std::pair<at::Tensor, at::Tensor>>
CacheLibCache::get_evicted_indices_and_weights() {
  if (evicted_indices_ptr_) {
    assert(evicted_weights_ptr_ != nullptr);
    return std::make_pair(*evicted_indices_ptr_, *evicted_weights_ptr_);
  } else {
    return folly::none;
  }
}

std::vector<int64_t> CacheLibCache::get_cache_usage() {
  std::vector<int64_t> cache_mem_stats(2, 0); // freeBytes, capacity
  cache_mem_stats[1] = cache_config_.cache_size_bytes;
  for (auto& pool_id : pool_ids_) {
    auto pool_stats = cache_->getPoolStats(pool_id);
    cache_mem_stats[0] += pool_stats.freeMemoryBytes();
  }
  return cache_mem_stats;
}

} // namespace l2_cache
