//
// Modernized for C++23.

#include <cstdio>
#include <string>

#include "leveldb/dumpfile.h"
#include "leveldb/std_file_system.h"
#include "leveldb/status.h"

namespace leveldb {
namespace {

// Replaced StdoutPrinter with a lambda callback

bool HandleDumpCommand(StdFileSystem* env, char** files, int num) {
  auto print_cb = [](std::string_view data) {
    fwrite(data.data(), 1, data.size(), stdout);
  };
  
  bool ok = true;
  for (int i = 0; i < num; i++) {
    auto s = DumpFile(env, files[i], print_cb);
    if (!s) {
      std::fprintf(stderr, "%s\n", s.error().ToString().c_str());
      ok = false;
    }
  }
  return ok;
}

}  // namespace
}  // namespace leveldb

static void Usage() {
  std::fprintf(
      stderr,
      "Usage: leveldbutil command...\n"
      "   dump files...         -- dump contents of specified files\n");
}

int main(int argc, char** argv) {
  leveldb::StdFileSystem env_impl;
  leveldb::StdFileSystem* env = &env_impl;
  bool ok = true;
  if (argc < 2) {
    Usage();
    ok = false;
  } else {
    std::string command = argv[1];
    if (command == "dump") {
      ok = leveldb::HandleDumpCommand(env, argv + 2, argc - 2);
    } else {
      Usage();
      ok = false;
    }
  }
  return (ok ? 0 : 1);
}
