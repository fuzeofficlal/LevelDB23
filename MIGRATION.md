# LevelDB C++23 Modernization Migration Guide

This document summarizes the core points, structural improvements, and specific API changes introduced during the modernization of LevelDB to **C++23**.

---

## 1. Migration Goals
The objective of this project was to modernize LevelDB from legacy C++98/11 to modern **C++23**, removing custom OS abstractions, custom resource managers, and manual pointer-based error handling in favor of native C++23 standard features, resulting in safer, cleaner, and more performant code.

---

## 2. Core Modernization Points

### 2.1 Error Handling (`std::expected` / `Result<T>`)
* **Legacy Approach**: Custom `leveldb::Status` class representing errors. Methods returned `Status` and accepted output pointers for values.
* **Modernized Approach**: C++23 `std::expected` (aliased as `Result<T>` in `include/leveldb/status.h`). Methods now return either the expected value or a `leveldb::Status` error, allowing for functional monadic chaining (`and_then`, `or_else`).

### 2.2 File System Abstraction (`std::filesystem`)
* **Legacy Approach**: Custom `leveldb::Env` class with OS-specific implementations for file operations (`env_posix.cc`, `env_windows.cc`).
* **Modernized Approach**: Unified file system access using `std::filesystem` (`util/std_file_system.h` and `util/std_file_system.cc`). Removes OS-specific code forks, compiling cross-platform via the standard C++ library.

### 2.3 Automatic Resource & Lifetime Pinned Management
* **Legacy Approach**: Manual resource releasing. For example, Snapshots required explicit `db->ReleaseSnapshot(snap)` to prevent resource leaks.
* **Modernized Approach**: Smart pointers (`std::shared_ptr`, `std::unique_ptr`) are utilized for automatic lifetime management:
  * **Snapshots**: `GetSnapshot()` now returns `std::shared_ptr<const Snapshot>`. Releasing the snapshot is handled automatically when the shared pointer goes out of scope.
  * **Iterator Lifetimes**: Pinned the active version (`std::shared_ptr<Version>`) and memtables (`std::shared_ptr<MemTable>`) to the `Iterator` via modern lambda cleanup registration. This guarantees that background compaction does not free files currently in use by an active iterator, preventing use-after-free segfaults.

### 2.4 Multithreading & Synchronization
* **Legacy Approach**: Custom mutexes and lock wrappers.
* **Modernized Approach**: Leverages native standard concurrency features (`std::mutex`, `std::unique_lock`, `std::scoped_lock`, and `std::atomic`).

---

## 3. Specific API Changes (Before vs After)

### 3.1 Error Handling & Method Signatures
```cpp
// ===== Legacy C++ =====
leveldb::Status s;
std::string value;
s = db->Get(leveldb::ReadOptions(), "key", &value);
if (s.ok()) {
    // success
}

// ===== Modern C++23 =====
leveldb::ReadOptions ropt;
auto res = db->Get(ropt, "key");
if (res) {
    if (*res) {
        std::string_view val = **res; // success
    } else {
        // key not found
    }
} else {
    leveldb::Status error = res.error(); // handle error
}
```

### 3.2 File Operations
```cpp
// ===== Legacy C++ =====
leveldb::SequentialFile* file;
leveldb::Status s = leveldb::Env::Default()->NewSequentialFile("path/to/file", &file);
if (s.ok()) {
    char scratch[100];
    leveldb::Slice result;
    s = file->Read(100, &result, scratch);
    delete file;
}

// ===== Modern C++23 =====
leveldb::StdFileSystem fs;
auto res = fs.NewSequentialFile("path/to/file");
if (res) {
    std::unique_ptr<leveldb::SequentialFile> file = std::move(*res);
    char scratch[100];
    auto read_res = file->Read(100, scratch);
    if (read_res) {
        std::string_view result = *read_res;
    }
}
```

### 3.3 Snapshot Management
```cpp
// ===== Legacy C++ =====
const leveldb::Snapshot* snapshot = db->GetSnapshot();
leveldb::ReadOptions ropt;
ropt.snapshot = snapshot;
// ... read operations ...
db->ReleaseSnapshot(snapshot);

// ===== Modern C++23 =====
std::shared_ptr<const leveldb::Snapshot> snapshot = db->GetSnapshot();
leveldb::ReadOptions ropt;
ropt.snapshot = snapshot.get();
// ... read operations ...
// snapshot is automatically released when "snapshot" goes out of scope!
```

---

## 4. Compilation & Verification
The build system uses standard CMake:
```bash
# Compile and build the modernized codebase
cmake -B build
cmake --build build

# Run the unit tests
./build/LevelDB_23

# Run the C API compatibility tests
./build/c_test
```
