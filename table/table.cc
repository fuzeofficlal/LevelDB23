//
// Modernized for C++23.

#pragma once

#include "leveldb/cache.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/block.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "db/co_task.h"
#include "db/async_executor.h"

namespace leveldb {

template <CRandomAccessFile SrcFile>
struct Table<SrcFile>::Rep {
  ~Rep() {
    delete filter;
    delete[] filter_data;
  }

  TableOptions options;
  Result<void> status;
  SrcFile* file;
  uint64_t cache_id;
  FilterBlockReader* filter;
  const char* filter_data;

  BlockHandle metaindex_handle;  // Handle to metaindex_block: saved from footer
  std::unique_ptr<Block> index_block;
};

template <CRandomAccessFile SrcFile>
Result<std::unique_ptr<Table<SrcFile>>> Table<SrcFile>::Open(
    const TableOptions& options, SrcFile* file, uint64_t size) {
  if (size < Footer::kEncodedLength) {
    return Status::CorruptionErr("file is too short to be an sstable");
  }

  char footer_space[Footer::kEncodedLength];
  std::string_view footer_input;
  auto read_res = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength, footer_space);
  if (!read_res) return std::unexpected(read_res.error());
  footer_input = *read_res;

  Footer footer;
  auto dec_res = footer.DecodeFrom(footer_input);
  if (!dec_res) return std::unexpected(dec_res.error());

  // Read the index block
  ReadOptions opt;
  if (options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  auto index_block_res = ReadBlock(file, opt, footer.index_handle());
  if (!index_block_res) return std::unexpected(index_block_res.error());

  // We've successfully read the footer and the index block: we're ready to serve requests.
  auto index_block = std::make_unique<Block>(std::move(*index_block_res));
  Rep* rep = new Rep;
  rep->options = options;
  rep->file = file;
  rep->metaindex_handle = footer.metaindex_handle();
  rep->index_block = std::move(index_block);
  rep->cache_id = (options.block_cache ? options.block_cache->NewId() : 0);
  rep->filter_data = nullptr;
  rep->filter = nullptr;
  
  auto table = std::unique_ptr<Table<SrcFile>>(new Table(rep));
  table->ReadMeta(footer);

  return table;
}

template <CRandomAccessFile SrcFile>
void Table<SrcFile>::ReadMeta(const Footer& footer) {
  if (rep_->options.filter_policy == nullptr) {
    return;  // Do not need any metadata
  }

  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  auto contents = ReadBlock(rep_->file, opt, footer.metaindex_handle());
  if (!contents) {
    // Do not propagate errors since meta info is not needed for operation
    return;
  }
  Block meta(std::move(*contents));

  auto iter = meta.NewIterator(BytewiseComparator());
  std::string key = "filter.";
  key.append(rep_->options.filter_policy->Name());
  iter->Seek(key);
  if (iter->Valid() && iter->key() == key) {
    ReadFilter(iter->value());
  }
}

template <CRandomAccessFile SrcFile>
void Table<SrcFile>::ReadFilter(std::string_view filter_handle_value) {
  std::string_view v = filter_handle_value;
  BlockHandle filter_handle;
  if (!filter_handle.DecodeFrom(v)) {
    return;
  }

  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  auto block_res = ReadBlock(rep_->file, opt, filter_handle);
  if (!block_res) {
    return;
  }
  auto& block = *block_res;
  if (block.heap_data) {
    rep_->filter_data = block.heap_data.release();  // Will need to delete later
    rep_->filter = new FilterBlockReader(rep_->options.filter_policy, std::string_view(rep_->filter_data, block.data.size()));
  } else {
    rep_->filter = new FilterBlockReader(rep_->options.filter_policy, block.data);
  }
}

template <CRandomAccessFile SrcFile>
Table<SrcFile>::~Table() { delete rep_; }

template <CRandomAccessFile SrcFile>
std::unique_ptr<Iterator> Table<SrcFile>::BlockReader(
    const Table<SrcFile>* table, const ReadOptions& options, std::string_view index_value) {
  Cache* block_cache = table->rep_->options.block_cache;
  std::unique_ptr<Block> block;
  Cache::CacheHandle cache_handle;

  BlockHandle handle;
  std::string_view input = index_value;
  auto s = handle.DecodeFrom(input);

  if (s) {
    if (block_cache != nullptr) {
      char cache_key_buffer[16];
      EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
      EncodeFixed64(cache_key_buffer + 8, handle.offset());
      std::string_view key(cache_key_buffer, sizeof(cache_key_buffer));
      cache_handle = block_cache->Lookup(key);
      if (cache_handle) {
        block.reset(reinterpret_cast<Block*>(block_cache->Value(cache_handle.get())));
      } else {
        auto contents = ReadBlock(table->rep_->file, options, handle);
        if (contents) {
          block = std::make_unique<Block>(std::move(*contents));
          if (contents->cachable && options.fill_cache) {
            Block* raw_block = block.release();
            cache_handle = block_cache->Insert(key, raw_block, raw_block->size(),
                                               [](std::string_view, void* value) {
                                                 delete reinterpret_cast<Block*>(value);
                                               });
            block.reset(raw_block); 
          }
        } else {
          s = std::unexpected(contents.error());
        }
      }
    } else {
      auto contents = ReadBlock(table->rep_->file, options, handle);
      if (contents) {
        block = std::make_unique<Block>(std::move(*contents));
      } else {
        s = std::unexpected(contents.error());
      }
    }
  }

  std::unique_ptr<Iterator> iter;
  if (block != nullptr) {
    iter = block->NewIterator(table->rep_->options.comparator);
    if (!cache_handle) {
      Block* raw_block = block.release();
      iter->RegisterCleanup([raw_block]() { delete raw_block; });
    } else {
      block.release();
      iter->RegisterCleanup([h = std::move(cache_handle)]() {});
    }
  } else {
    iter.reset(NewErrorIterator(s));
  }
  return iter;
}

template <CRandomAccessFile SrcFile>
std::unique_ptr<Iterator> Table<SrcFile>::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(
      rep_->index_block->NewIterator(rep_->options.comparator),
      [this](const ReadOptions& opts, std::string_view index_value) {
        return BlockReader(this, opts, index_value);
      },
      options);
}

template <CRandomAccessFile SrcFile>
Result<void> Table<SrcFile>::InternalGet(const ReadOptions& options, std::string_view k,
                                         std::move_only_function<void(std::string_view, std::string_view)> handle_result) {
  Result<void> s;
  auto iiter = rep_->index_block->NewIterator(rep_->options.comparator);
  iiter->Seek(k);
  if (iiter->Valid()) {
    std::string_view handle_value = iiter->value();
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
    std::string_view hv_copy = handle_value;
    if (filter != nullptr && handle.DecodeFrom(hv_copy) &&
        !filter->KeyMayMatch(handle.offset(), k)) {
      // Not found
    } else {
      auto block_iter = BlockReader(this, options, handle_value);
      block_iter->Seek(k);
      if (block_iter->Valid()) {
        handle_result(block_iter->key(), block_iter->value());
      }
      s = block_iter->status();
    }
  }
  if (s) {
    s = iiter->status();
  }
  return s;
}

template <CRandomAccessFile SrcFile>
bool Table<SrcFile>::InternalGetFast(const ReadOptions& options, std::string_view key,
                                     std::move_only_function<void(std::string_view, std::string_view)> handle_result) {
  auto iiter = rep_->index_block->NewIterator(rep_->options.comparator);
  iiter->Seek(key);
  bool completed = false;
  if (iiter->Valid()) {
    std::string_view handle_value = iiter->value();
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
    std::string_view hv_copy = handle_value;
    if (filter != nullptr && handle.DecodeFrom(hv_copy) &&
        !filter->KeyMayMatch(handle.offset(), key)) {
      // Key definitely not in this table. Fast path complete.
      completed = true;
    } else {
      Cache* block_cache = rep_->options.block_cache;
      if (block_cache != nullptr && handle.DecodeFrom(handle_value)) {
        char cache_key_buffer[16];
        EncodeFixed64(cache_key_buffer, rep_->cache_id);
        EncodeFixed64(cache_key_buffer + 8, handle.offset());
        std::string_view key_sv(cache_key_buffer, sizeof(cache_key_buffer));
        auto cache_handle = block_cache->Lookup(key_sv);
        if (cache_handle) {
          Block* block = reinterpret_cast<Block*>(block_cache->Value(cache_handle.get()));
          auto block_iter = block->NewIterator(rep_->options.comparator);
          block_iter->Seek(key);
          if (block_iter->Valid()) {
            handle_result(block_iter->key(), block_iter->value());
          }
          completed = true;
        }
      }
    }
  } else {
    // Index block indicates key is past any data, so definitely not found.
    completed = true;
  }
  return completed;
}

template <CRandomAccessFile SrcFile>
uint64_t Table<SrcFile>::ApproximateOffsetOf(std::string_view key) const {
  auto index_iter = rep_->index_block->NewIterator(rep_->options.comparator);
  index_iter->Seek(key);
  uint64_t result;
  if (index_iter->Valid()) {
    BlockHandle handle;
    std::string_view input = index_iter->value();
    auto s = handle.DecodeFrom(input);
    if (s) {
      result = handle.offset();
    } else {
      result = rep_->metaindex_handle.offset();
    }
  } else {
    result = rep_->metaindex_handle.offset();
  }
  return result;
}

template <CRandomAccessFile SrcFile>
Task<Result<void>> Table<SrcFile>::InternalGetAsync(
    const ReadOptions& options, std::string_view key,
    std::move_only_function<void(std::string_view, std::string_view)> handle_result,
    AsyncExecutor* executor) {
  Result<void> s;
  auto iiter = rep_->index_block->NewIterator(rep_->options.comparator);
  iiter->Seek(key);
  if (iiter->Valid()) {
    std::string_view handle_value = iiter->value();
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
    std::string_view hv_copy = handle_value;
    if (filter != nullptr && handle.DecodeFrom(hv_copy) &&
        !filter->KeyMayMatch(handle.offset(), key)) {
      // Not found
    } else {
      auto block_iter_res = co_await BlockReaderAsync(this, options, handle_value, executor);
      if (block_iter_res) {
        auto& block_iter = *block_iter_res;
        block_iter->Seek(key);
        if (block_iter->Valid()) {
          handle_result(block_iter->key(), block_iter->value());
        }
        s = block_iter->status();
      } else {
        s = std::unexpected(block_iter_res.error());
      }
    }
  }
  if (s) {
    s = iiter->status();
  }
  co_return s;
}

template <CRandomAccessFile SrcFile>
Task<Result<std::unique_ptr<Iterator>>> Table<SrcFile>::BlockReaderAsync(
    const Table<SrcFile>* table, const ReadOptions& options, std::string_view index_value,
    AsyncExecutor* executor) {
  Cache* block_cache = table->rep_->options.block_cache;
  std::unique_ptr<Block> block;
  Cache::CacheHandle cache_handle;

  BlockHandle handle;
  std::string_view input = index_value;
  auto s = handle.DecodeFrom(input);

  if (s) {
    if (block_cache != nullptr) {
      char cache_key_buffer[16];
      EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
      EncodeFixed64(cache_key_buffer + 8, handle.offset());
      std::string_view key(cache_key_buffer, sizeof(cache_key_buffer));
      cache_handle = block_cache->Lookup(key);
      if (cache_handle) {
        block.reset(reinterpret_cast<Block*>(block_cache->Value(cache_handle.get())));
      } else {
        auto contents_res = co_await executor->submit([table, options, handle]() {
          return ReadBlock(table->rep_->file, options, handle);
        });
        if (contents_res) {
          block = std::make_unique<Block>(std::move(*contents_res));
          if (contents_res->cachable && options.fill_cache) {
            Block* raw_block = block.release();
            cache_handle = block_cache->Insert(key, raw_block, raw_block->size(),
                                               [](std::string_view, void* value) {
                                                 delete reinterpret_cast<Block*>(value);
                                               });
            block.reset(raw_block); 
          }
        } else {
          s = std::unexpected(contents_res.error());
        }
      }
    } else {
      auto contents_res = co_await executor->submit([table, options, handle]() {
        return ReadBlock(table->rep_->file, options, handle);
      });
      if (contents_res) {
        block = std::make_unique<Block>(std::move(*contents_res));
      } else {
        s = std::unexpected(contents_res.error());
      }
    }
  }

  std::unique_ptr<Iterator> iter;
  if (block != nullptr) {
    iter = block->NewIterator(table->rep_->options.comparator);
    if (!cache_handle) {
      Block* raw_block = block.release();
      iter->RegisterCleanup([raw_block]() { delete raw_block; });
    } else {
      block.release();
      iter->RegisterCleanup([h = std::move(cache_handle)]() {});
    }
  } else {
    iter.reset(NewErrorIterator(s));
  }
  co_return iter;
}

}  // namespace leveldb
