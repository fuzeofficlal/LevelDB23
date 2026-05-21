//
// Modernized for C++23.

#pragma once

#include "leveldb/status.h"
#include "leveldb/std_file_system.h"
#include "leveldb/options.h"
#include <string_view>
#include <memory>

namespace leveldb {

struct FileMetaData;
class Iterator;
class TableCache;

Result<void> BuildTable(std::string_view dbname, StdFileSystem* env, const Options<StdFileSystem>& options,
                        TableCache* table_cache, Iterator* iter, FileMetaData* meta);

}  // namespace leveldb
