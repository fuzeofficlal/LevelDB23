//
// Modernized for C++23.

#include "db/builder.h"

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "leveldb/table_builder.h"

namespace leveldb {

Result<void> BuildTable(std::string_view dbname, StdFileSystem* env, const Options<StdFileSystem>& options,
                        TableCache* table_cache, Iterator* iter, FileMetaData* meta) {
  meta->file_size = 0;
  iter->SeekToFirst();

  std::string fname = TableFileName(dbname, meta->number);
  if (iter->Valid()) {
    auto file_res = env->NewWritableFile(fname);
    if (!file_res) {
      return std::unexpected(file_res.error());
    }
    auto file = std::move(*file_res);

    TableBuilderOptions tb_options(options);
    TableBuilder<StdFileSystem::WritableFile> builder(tb_options, &file);
    meta->smallest.DecodeFrom(iter->key());
    std::string_view key;
    for (; iter->Valid(); iter->Next()) {
      key = iter->key();
      builder.Add(key, iter->value());
    }
    if (!key.empty()) {
      meta->largest.DecodeFrom(key);
    }

    auto finish_res = builder.Finish();
    if (finish_res) {
      meta->file_size = builder.FileSize();
      assert(meta->file_size > 0);
    } else {
      env->RemoveFile(fname);
      return std::unexpected(finish_res.error());
    }

    auto sync_res = file.Sync();
    if (!sync_res) {
      env->RemoveFile(fname);
      return std::unexpected(sync_res.error());
    }

    auto close_res = file.Close();
    if (!close_res) {
      env->RemoveFile(fname);
      return std::unexpected(close_res.error());
    }

    // Verify that the table is usable
    auto it = table_cache->NewIterator(ReadOptions(), meta->number, meta->file_size);
    if (!it->status()) {
      auto status = it->status();
      env->RemoveFile(fname);
      return std::unexpected(status.error());
    }
  }

  // Check for input iterator errors
  if (!iter->status()) {
    env->RemoveFile(fname);
    return std::unexpected(iter->status().error());
  }

  if (meta->file_size > 0) {
    // Keep it
  } else {
    env->RemoveFile(fname);
  }
  return {};
}

}  // namespace leveldb
