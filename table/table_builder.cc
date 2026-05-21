//
// Modernized for C++23.

#pragma once

#include <cassert>

#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {

template <CWritableFile DestFile>
struct TableBuilder<DestFile>::Rep {
  Rep(const TableBuilderOptions& opt, DestFile* f)
      : options(opt),
        file(f),
        offset(0),
        data_block(BlockBuilderOptions{
            .block_restart_interval = opt.block_restart_interval,
            .comparator = opt.comparator}),
        index_block(BlockBuilderOptions{
            .block_restart_interval = 1,
            .comparator = opt.comparator}),
        num_entries(0),
        closed(false),
        filter_block(opt.filter_policy == nullptr
                         ? nullptr
                         : new FilterBlockBuilder(opt.filter_policy)),
        pending_index_entry(false) {}

  ~Rep() { delete filter_block; }

  TableBuilderOptions options;
  DestFile* file;
  uint64_t offset;
  Result<void> status;
  BlockBuilder data_block;
  BlockBuilder index_block;
  std::string last_key;
  int64_t num_entries;
  bool closed;
  FilterBlockBuilder* filter_block;
  bool pending_index_entry;
  BlockHandle pending_handle;
  std::string compressed_output;
};

template <CWritableFile DestFile>
TableBuilder<DestFile>::TableBuilder(const TableBuilderOptions& options, DestFile* file)
    : rep_(new Rep(options, file)) {
  if (rep_->filter_block != nullptr) {
    rep_->filter_block->StartBlock(0);
  }
}

template <CWritableFile DestFile>
TableBuilder<DestFile>::~TableBuilder() {
  assert(rep_->closed);  // Catch errors where caller forgot to call Finish()
  delete rep_;
}

template <CWritableFile DestFile>
Result<void> TableBuilder<DestFile>::ChangeOptions(const TableBuilderOptions& options) {
  if (options.comparator != rep_->options.comparator) {
    return Status::InvalidArgumentErr("changing comparator while building table");
  }
  rep_->options = options;
  return {};
}

template <CWritableFile DestFile>
void TableBuilder<DestFile>::Add(std::string_view key, std::string_view value) {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->num_entries > 0) {
    assert(r->options.comparator->Compare(key, r->last_key) > 0);
  }

  if (r->pending_index_entry) {
    assert(r->data_block.empty());
    r->options.comparator->FindShortestSeparator(r->last_key, key);
    std::string handle_encoding;
    r->pending_handle.EncodeTo(handle_encoding);
    r->index_block.Add(r->last_key, handle_encoding);
    r->pending_index_entry = false;
  }

  if (r->filter_block != nullptr) {
    r->filter_block->AddKey(key);
  }

  r->last_key.assign(key.data(), key.size());
  r->num_entries++;
  r->data_block.Add(key, value);

  const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
  if (estimated_block_size >= r->options.block_size) {
    Flush();
  }
}

template <CWritableFile DestFile>
void TableBuilder<DestFile>::Flush() {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->data_block.empty()) return;
  assert(!r->pending_index_entry);
  WriteBlock(&r->data_block, &r->pending_handle);
  if (ok()) {
    r->pending_index_entry = true;
    r->status = r->file->Flush();
  }
  if (r->filter_block != nullptr) {
    r->filter_block->StartBlock(r->offset);
  }
}

template <CWritableFile DestFile>
void TableBuilder<DestFile>::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  assert(ok());
  Rep* r = rep_;
  std::string_view raw = block->Finish();

  std::string_view block_contents;
  CompressionType type = r->options.compression;

  // Minimal compression stub; real implementation should plug Snappy/Zstd
  type = CompressionType::kNoCompression;
  block_contents = raw;

  WriteRawBlock(block_contents, type, handle);
  r->compressed_output.clear();
  block->Reset();
}

template <CWritableFile DestFile>
void TableBuilder<DestFile>::WriteRawBlock(std::string_view block_contents,
                                           CompressionType type, BlockHandle* handle) {
  Rep* r = rep_;
  handle->set_offset(r->offset);
  handle->set_size(block_contents.size());
  r->status = r->file->Append(block_contents);
  if (r->status) {
    char trailer[kBlockTrailerSize];
    trailer[0] = static_cast<char>(type);
    uint32_t crc = crc32c::Value(block_contents);
    crc = crc32c::Extend(crc, std::string_view(trailer, 1));
    EncodeFixed32(trailer + 1, crc32c::Mask(crc));
    r->status = r->file->Append(std::string_view(trailer, kBlockTrailerSize));
    if (r->status) {
      r->offset += block_contents.size() + kBlockTrailerSize;
    }
  }
}

template <CWritableFile DestFile>
Result<void> TableBuilder<DestFile>::status() const { return rep_->status; }

template <CWritableFile DestFile>
Result<void> TableBuilder<DestFile>::Finish() {
  Rep* r = rep_;
  Flush();
  assert(!r->closed);
  r->closed = true;

  BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;

  if (ok() && r->filter_block != nullptr) {
    WriteRawBlock(r->filter_block->Finish(), CompressionType::kNoCompression,
                  &filter_block_handle);
  }

  if (ok()) {
    BlockBuilder meta_index_block(BlockBuilderOptions{
      .block_restart_interval = r->options.block_restart_interval,
      .comparator = r->options.comparator
    });
    if (r->filter_block != nullptr) {
      std::string key = "filter.";
      key.append(r->options.filter_policy->Name());
      std::string handle_encoding;
      filter_block_handle.EncodeTo(handle_encoding);
      meta_index_block.Add(key, handle_encoding);
    }
    WriteBlock(&meta_index_block, &metaindex_block_handle);
  }

  if (ok()) {
    if (r->pending_index_entry) {
      r->options.comparator->FindShortSuccessor(r->last_key);
      std::string handle_encoding;
      r->pending_handle.EncodeTo(handle_encoding);
      r->index_block.Add(r->last_key, handle_encoding);
      r->pending_index_entry = false;
    }
    WriteBlock(&r->index_block, &index_block_handle);
  }

  if (ok()) {
    Footer footer;
    footer.set_metaindex_handle(metaindex_block_handle);
    footer.set_index_handle(index_block_handle);
    std::string footer_encoding;
    footer.EncodeTo(footer_encoding);
    r->status = r->file->Append(footer_encoding);
    if (r->status) {
      r->offset += footer_encoding.size();
    }
  }
  return r->status;
}

template <CWritableFile DestFile>
void TableBuilder<DestFile>::Abandon() {
  Rep* r = rep_;
  assert(!r->closed);
  r->closed = true;
}

template <CWritableFile DestFile>
uint64_t TableBuilder<DestFile>::NumEntries() const { return rep_->num_entries; }

template <CWritableFile DestFile>
uint64_t TableBuilder<DestFile>::FileSize() const { return rep_->offset; }

}  // namespace leveldb
