//
// Modernized for C++23.

#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "leveldb/std_file_system.h"
#include "leveldb/status.h"

namespace leveldb {

// Dump the contents of the file named by fname in text format.
// Makes a sequence of append_cb calls; each call is passed
// the newline-terminated text corresponding to a single item found
// in the file.
//
// Returns a non-OK result if fname does not name a leveldb storage
// file, or if the file cannot be read.
Result<void> DumpFile(StdFileSystem* env, const std::filesystem::path& fname,
                      std::function<void(std::string_view)> append_cb);

}  // namespace leveldb
