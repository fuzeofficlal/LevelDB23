# LevelDB-23 🚀

[![C++ Standard](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![License](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)
[![Build Status](https://img.shields.io/badge/Build-Passing-brightgreen.svg)]()

A modernized, high-performance, cross-platform port of Google's **LevelDB** key-value store, redesigned from the ground up to leverage the modern capabilities of **C++23**.

---

## 🌟 Key Features

* **C++23 Standard Primitives**: Built entirely on top of the C++23 Standard Template Library (STL). Legacy custom classes for OS abstractions, threading, and logging are replaced with native equivalents.
* **Monadic Error Handling**: Replaces the custom `leveldb::Status` return patterns with C++23 `std::expected` (aliased as `Result<T>`), allowing fluent monadic pipelines via `.and_then()` and `.or_else()`.
* **Standard File System (`std::filesystem`)**: Employs standard STL directory utilities rather than OS-specific Posix/Windows file wrappers, guaranteeing true cross-platform builds out of the box.
* **Modern Memory Management**: Automatic resource tracking via `std::shared_ptr` and `std::unique_ptr`. Fast, secure, leak-free snapshots and automatic iterator lifetime pinning to prevent use-after-free conditions.
* **Thread Safety**: Built using C++23 standard synchronization tools (`std::mutex`, `std::scoped_lock`, and atomic variables).
* **Core Compatibility**: Retains complete compatibility with original LevelDB data structures, disk layout, Murmur-like hashing, prefix block compression, Bloom filters, and WAL formats.

---

## 🛠 Quick Start

### 1. Opening a Database
```cpp
#include "db/db_impl.h"
#include "util/std_file_system.h"
#include <iostream>

int main() {
    leveldb::Options<leveldb::StdFileSystem> options;
    options.create_if_missing = true;

    auto db_res = leveldb::DB::Open(options, "my_db");
    if (!db_res) {
        std::cerr << "Failed to open DB: " << db_res.error().ToString() << "\n";
        return 1;
    }
    
    auto db = std::move(*db_res);
    std::cout << "Database opened successfully!\n";
}
```

### 2. Basic Put and Get Operations
```cpp
leveldb::WriteOptions wopt;
db->Put(wopt, "hello", "world");

leveldb::ReadOptions ropt;
auto get_res = db->Get(ropt, "hello");

if (get_res) {
    if (*get_res) {
        std::cout << "Value: " << **get_res << "\n"; // Outputs: world
    } else {
        std::cout << "Key not found\n";
    }
} else {
    std::cerr << "Read error: " << get_res.error().ToString() << "\n";
}
```

### 3. Atomic Writes using WriteBatch
```cpp
leveldb::WriteBatch batch;
batch.Put("key1", "val1");
batch.Put("key2", "val2");
batch.Delete("key1");

auto status = db->Write(leveldb::WriteOptions(), &batch);
if (status) {
    std::cout << "Batch written successfully!\n";
}
```

### 4. Automatic Snapshot Isolation (MVCC)
```cpp
db->Put(wopt, "snapkey", "v1");

// Create snapshot (automatic resource tracking via shared_ptr)
auto snapshot = db->GetSnapshot(); 

// Overwrite values
db->Put(wopt, "snapkey", "v2");

// Read using snapshot
leveldb::ReadOptions snap_ropt;
snap_ropt.snapshot = snapshot.get();
auto res_snap = db->Get(snap_ropt, "snapkey"); // Yields: "v1"

// Read normally
auto res_normal = db->Get(leveldb::ReadOptions(), "snapkey"); // Yields: "v2"
```

---

## 🏗 Build & Test

LevelDB-23 uses CMake for code generation and builds.

### Compilation
```bash
# Configure the project
cmake -B build

# Build the targets
cmake --build build
```

### Run Tests
```bash
# Run the core C++23 integration test suite
./build/LevelDB_23

# Run the C API compatibility tests
./build/c_test
```

---

## 📂 Codebase Directory Structure

* **`db/`**: Core database implementation (`DBImpl`, Version Set, MemTable, Logging, LogWriter/Reader, Iterators).
* **`include/leveldb/`**: Public header APIs exposing DB options, comparator algorithms, cache handlers, filter policies, and C-bindings (`c.h`).
* **`table/`**: SSTable structure builder, merger, two-level iterators, and block compression formatting.
* **`util/`**: Modern STL file system access wrapper (`std_file_system.cc`), varint serialization utilities, hash engines, and bloom filter metrics.

---

## 📄 License
This project is governed by the modern 3-clause BSD License. See the `LICENSE` file for contrib details.
