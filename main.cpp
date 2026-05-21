#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include "db/db_impl.h"
#include "db/filename.h"
#include "leveldb/status.h"
#include "leveldb/cache.h"
#include "leveldb/dumpfile.h"
#include "db/write_batch_internal.h"
#include "util/coding.h"
#include "util/hash.h"
#include "util/crc32c.h"
#include "util/arena.h"
#include "util/random.h"
#include "util/hash.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/no_destructor.h"
#include "util/compression.h"
#include "leveldb/std_file_system.h"
#include "leveldb/options.h"
#include "db/log_writer.h"
#include "db/log_reader.h"
#include "table/block.h"
#include "table/block_builder.h"
#include "leveldb/table_builder.h"
#include "leveldb/table.h"
#include "leveldb/filter_policy.h"
#include "leveldb/comparator.h"
#include "leveldb/comparator.h"
#include "leveldb/table_builder.h"
#include "leveldb/table.h"
#include "leveldb/filter_policy.h"
#include "leveldb/db.h"
#include "db/memtable.h"
#include "db/dbformat.h"
#include "db/version_edit.h"
#include <cstring>
#include <mutex>

// ============================================================================
// 示例函数：展示 Result<T> 和 Result<void> 的统一用法
// ============================================================================

// 有返回值的函数 → Result<T>
leveldb::Result<int> Divide(int a, int b) {
  if (b == 0) {
    return leveldb::Status::InvalidArgumentErr("Division by zero");
  }
  return a / b;
}

// 有返回值 + 双消息
leveldb::Result<std::string> LookupUser(int id) {
  if (id <= 0) {
    return leveldb::Status::InvalidArgumentErr("bad user id");
  }
  if (id > 1000) {
    return leveldb::Status::NotFoundErr("user not found",
                                        "id=" + std::to_string(id));
  }
  return "User_" + std::to_string(id);
}

// 无返回值的函数 → Result<void>
leveldb::Result<void> ValidateConfig(bool valid) {
  if (!valid) {
    return leveldb::Status::CorruptionErr("config file corrupted");
  }
  return {};  // 成功，零开销
}

// Result<void> 链式：模拟多步 I/O 操作
leveldb::Result<void> FlushToDisk(bool disk_ok) {
  if (!disk_ok) {
    return leveldb::Status::IOErrorErr("disk full");
  }
  return {};
}

leveldb::Result<void> SyncLog(bool sync_ok) {
  if (!sync_ok) {
    return leveldb::Status::IOErrorErr("sync failed");
  }
  return {};
}

// ============================================================================
// main — 逐一演示每种使用模式
// ============================================================================
int main() {
  std::cout << "===== Result<T> 基本用法 =====\n";
  {
    auto r = Divide(10, 3);
    if (r) {
      std::cout << "10 / 3 = " << *r << "\n";
    }

    auto r2 = Divide(10, 0);
    if (!r2) {
      std::cout << "Error: " << r2.error().ToString() << "\n";
    }
  }

  std::cout << "\n===== Result<T> Monadic 链式调用 =====\n";
  {
    // transform: 成功时变换值 (T → U)
    // and_then:  成功时执行可能失败的操作 (T → Result<U>)
    // or_else:   失败时恢复 (Status → Result<T>)
    auto result = Divide(100, 5)
                      .transform([](int v) { return v * 2; })
                      .and_then([](int v) -> leveldb::Result<std::string> {
                        if (v > 100)
                          return leveldb::Status::InvalidArgumentErr(
                              "too large");
                        return "answer=" + std::to_string(v);
                      });

    if (result) {
      std::cout << "Chain succeeded: " << *result << "\n";
    }

    // 失败链：or_else 恢复
    auto recovered = Divide(1, 0)
                         .or_else([](leveldb::Status e) -> leveldb::Result<int> {
                           std::cout << "Recovering from: " << e.ToString() << "\n";
                           return 0;  // 用默认值恢复
                         });

    std::cout << "Recovered value: " << *recovered << "\n";
  }

  std::cout << "\n===== Result<void> 基本用法 =====\n";
  {
    auto r1 = ValidateConfig(true);
    std::cout << "ValidateConfig(true):  "
              << (r1 ? "OK" : r1.error().ToString()) << "\n";

    auto r2 = ValidateConfig(false);
    std::cout << "ValidateConfig(false): "
              << (r2 ? "OK" : r2.error().ToString()) << "\n";
  }

  std::cout << "\n===== Result<void> 链式调用 =====\n";
  {
    // void 操作也能 chain — 模拟: 验证 → 刷盘 → 同步
    auto pipeline = ValidateConfig(true)
                        .and_then([]() { return FlushToDisk(true); })
                        .and_then([]() { return SyncLog(true); });
    std::cout << "Pipeline (all ok):  "
              << (pipeline ? "OK" : pipeline.error().ToString()) << "\n";

    // 中间步骤失败 → 后续自动跳过
    auto pipeline2 = ValidateConfig(true)
                         .and_then([]() { return FlushToDisk(false); })  // 这里失败
                         .and_then([]() { return SyncLog(true); });       // 不会执行
    std::cout << "Pipeline (disk err): "
              << (pipeline2 ? "OK" : pipeline2.error().ToString()) << "\n";
  }

  std::cout << "\n===== LookupUser 双消息示例 =====\n";
  {
    auto u1 = LookupUser(42);
    if (u1) std::cout << "Found: " << *u1 << "\n";

    auto u2 = LookupUser(9999);
    if (!u2) std::cout << "Error: " << u2.error().ToString() << "\n";
  }

  // ========================================================================
  // Coding 模块演示
  // ========================================================================

  std::cout << "\n===== Fixed32/64 编码解码 =====\n";
  {
    std::string buf;
    leveldb::PutFixed32(buf, 12345);
    leveldb::PutFixed64(buf, 9876543210ULL);

    // 从 buffer 中读回
    uint32_t v32 = leveldb::DecodeFixed32(buf.data());
    uint64_t v64 = leveldb::DecodeFixed64(buf.data() + 4);
    std::cout << "Fixed32: " << v32 << " (expect 12345)\n";
    std::cout << "Fixed64: " << v64 << " (expect 9876543210)\n";
  }

  std::cout << "\n===== Varint32/64 编码解码 =====\n";
  {
    std::string buf;
    leveldb::PutVarint32(buf, 0);
    leveldb::PutVarint32(buf, 127);        // 1 byte
    leveldb::PutVarint32(buf, 128);        // 2 bytes
    leveldb::PutVarint32(buf, 16384);      // 3 bytes
    leveldb::PutVarint64(buf, UINT64_MAX); // 10 bytes

    std::string_view input(buf);

    auto a = leveldb::GetVarint32(input);
    auto b = leveldb::GetVarint32(input);
    auto c = leveldb::GetVarint32(input);
    auto d = leveldb::GetVarint32(input);
    auto e = leveldb::GetVarint64(input);

    std::cout << "Varint32: " << *a << " (expect 0)\n";
    std::cout << "Varint32: " << *b << " (expect 127)\n";
    std::cout << "Varint32: " << *c << " (expect 128)\n";
    std::cout << "Varint32: " << *d << " (expect 16384)\n";
    std::cout << "Varint64: " << *e << " (expect 18446744073709551615)\n";
    std::cout << "Remaining bytes: " << input.size() << " (expect 0)\n";
  }

  std::cout << "\n===== VarintLength =====\n";
  {
    static_assert(leveldb::VarintLength(0) == 1);
    static_assert(leveldb::VarintLength(127) == 1);
    static_assert(leveldb::VarintLength(128) == 2);
    static_assert(leveldb::VarintLength(UINT64_MAX) == 10);
    std::cout << "All static_assert passed (compile-time verified)\n";
  }

  std::cout << "\n===== 长度前缀字符串 =====\n";
  {
    std::string buf;
    leveldb::PutLengthPrefixedSlice(buf, "hello");
    leveldb::PutLengthPrefixedSlice(buf, "world!!");

    std::string_view input(buf);

    auto s1 = leveldb::GetLengthPrefixedSlice(input);
    auto s2 = leveldb::GetLengthPrefixedSlice(input);
    auto s3 = leveldb::GetLengthPrefixedSlice(input);  // 无数据了

    std::cout << "Slice 1: " << *s1 << " (expect hello)\n";
    std::cout << "Slice 2: " << *s2 << " (expect world!!)\n";
    std::cout << "Slice 3: " << (s3 ? "unexpected" : "nullopt") << " (expect nullopt)\n";
  }

  std::cout << "\n===== 畸形输入处理 =====\n";
  {
    // 空输入
    std::string_view empty;
    auto r = leveldb::GetVarint32(empty);
    std::cout << "Empty input: " << (r ? "unexpected" : "nullopt") << " (expect nullopt)\n";

    // 截断的 varint（只有 continuation bytes，没有终止 byte）
    std::string bad = {static_cast<char>(0x80), static_cast<char>(0x80)};
    std::string_view bad_input(bad);
    auto r2 = leveldb::GetVarint32(bad_input);
    std::cout << "Truncated varint: " << (r2 ? "unexpected" : "nullopt") << " (expect nullopt)\n";
  }

  // ========================================================================
  // Hash 模块演示
  // ========================================================================

  std::cout << "\n===== Hash 函数验证 =====\n";
  {
    // 用原始 hash_test.cc 中的预期值来验证正确性
    const uint8_t data1[1] = {0x62};
    const uint8_t data2[2] = {0xc3, 0x97};
    const uint8_t data3[3] = {0xe2, 0x99, 0xa5};
    const uint8_t data4[4] = {0xe1, 0x80, 0xb9, 0x32};
    const uint8_t data5[48] = {
        0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    auto to_sv = [](const uint8_t* p, size_t n) -> std::string_view {
      return {reinterpret_cast<const char*>(p), n};
    };

    // 空输入
    auto h0 = leveldb::Hash({}, 0xbc9f1d34);
    std::cout << "Hash({}, seed):   0x" << std::hex << h0
              << (h0 == 0xbc9f1d34 ? "  ✓" : "  ✗") << "\n";

    // 1 字节
    auto h1 = leveldb::Hash(to_sv(data1, 1), 0xbc9f1d34);
    std::cout << "Hash(1 byte):     0x" << h1
              << (h1 == 0xef1345c4 ? "  ✓" : "  ✗") << "\n";

    // 2 字节
    auto h2 = leveldb::Hash(to_sv(data2, 2), 0xbc9f1d34);
    std::cout << "Hash(2 bytes):    0x" << h2
              << (h2 == 0x5b663814 ? "  ✓" : "  ✗") << "\n";

    // 3 字节
    auto h3 = leveldb::Hash(to_sv(data3, 3), 0xbc9f1d34);
    std::cout << "Hash(3 bytes):    0x" << h3
              << (h3 == 0x323c078f ? "  ✓" : "  ✗") << "\n";

    // 4 字节（正好一个 DecodeFixed32 路径）
    auto h4 = leveldb::Hash(to_sv(data4, 4), 0xbc9f1d34);
    std::cout << "Hash(4 bytes):    0x" << h4
              << (h4 == 0xed21633a ? "  ✓" : "  ✗") << "\n";

    // 48 字节（多轮 + 无余数）
    auto h5 = leveldb::Hash(to_sv(data5, 48), 0x12345678);
    std::cout << "Hash(48 bytes):   0x" << h5
              << (h5 == 0xf333dabb ? "  ✓" : "  ✗") << "\n";

    std::cout << std::dec;  // 恢复十进制输出
  }

  // ========================================================================
  // CRC32C 模块演示
  // ========================================================================

  std::cout << "\n===== CRC32C 校验验证 =====\n";
  {
    // RFC 3720 section B.4 标准测试向量
    char buf[32];

    std::memset(buf, 0, sizeof(buf));
    auto c1 = leveldb::crc32c::Value({buf, sizeof(buf)});
    std::cout << "CRC32C(zeros):    0x" << std::hex << c1
              << (c1 == 0x8a9136aa ? "  ✓" : "  ✗") << "\n";

    std::memset(buf, 0xff, sizeof(buf));
    auto c2 = leveldb::crc32c::Value({buf, sizeof(buf)});
    std::cout << "CRC32C(0xff):     0x" << c2
              << (c2 == 0x62a8ab43 ? "  ✓" : "  ✗") << "\n";

    for (int i = 0; i < 32; i++) buf[i] = static_cast<char>(i);
    auto c3 = leveldb::crc32c::Value({buf, sizeof(buf)});
    std::cout << "CRC32C(0..31):    0x" << c3
              << (c3 == 0x46dd794e ? "  ✓" : "  ✗") << "\n";

    for (int i = 0; i < 32; i++) buf[i] = static_cast<char>(31 - i);
    auto c4 = leveldb::crc32c::Value({buf, sizeof(buf)});
    std::cout << "CRC32C(31..0):    0x" << c4
              << (c4 == 0x113fdb5c ? "  ✓" : "  ✗") << "\n";

    // Extend 测试
    auto full = leveldb::crc32c::Value("hello world");
    auto half = leveldb::crc32c::Extend(leveldb::crc32c::Value("hello "), "world");
    std::cout << "CRC32C Extend:    " << (full == half ? "✓" : "✗") << "\n";

    // Mask/Unmask 测试 (constexpr)
    auto crc = leveldb::crc32c::Value("foo");
    auto masked = leveldb::crc32c::Mask(crc);
    std::cout << "Mask/Unmask:      "
              << (crc != masked && crc == leveldb::crc32c::Unmask(masked) ? "✓" : "✗")
              << "\n";

    std::cout << std::dec;
  }

  // ========================================================================
  // Random 模块演示
  // ========================================================================

  std::cout << "\n===== Random 随机数验证 =====\n";
  {
    leveldb::Random rng(42);
    bool range_ok = true;
    for (int i = 0; i < 1000; ++i) {
      uint32_t v = rng.Uniform(100);
      if (v >= 100) { range_ok = false; break; }
    }
    std::cout << "Uniform(100) range: " << (range_ok ? "✓" : "✗") << "\n";

    int count = 0;
    for (int i = 0; i < 10000; ++i) {
      if (rng.OneIn(10)) ++count;
    }
    // 期望约 1000，允许 [500, 1500]
    std::cout << "OneIn(10) hits:     " << count
              << (count >= 500 && count <= 1500 ? "  ✓" : "  ✗") << "\n";
  }

  // ========================================================================
  // Arena 模块演示
  // ========================================================================

  std::cout << "\n===== Arena 分配器验证 =====\n";
  {
    leveldb::Arena arena;
    char* p1 = arena.Allocate(100);
    char* p2 = arena.Allocate(200);
    char* p3 = arena.AllocateAligned(64);

    std::cout << "Allocate(100):      " << (p1 != nullptr ? "✓" : "✗") << "\n";
    std::cout << "Allocate(200):      " << (p2 != nullptr ? "✓" : "✗") << "\n";
    std::cout << "AllocateAligned(64): " << (p3 != nullptr ? "✓" : "✗")
              << " (aligned: " << ((reinterpret_cast<uintptr_t>(p3) % 8) == 0 ? "✓" : "✗")
              << ")\n";
    std::cout << "MemoryUsage:        " << arena.MemoryUsage() << " bytes\n";
  }

  // ========================================================================
  // MutexLock 模块演示
  // ========================================================================

  std::cout << "\n===== MutexLock 验证 =====\n";
  {
    std::mutex mu;
    { leveldb::MutexLock l(mu); }  // 编译即验证
    std::cout << "MutexLock alias:    ✓ (compiles as std::scoped_lock)\n";
  }

  // ========================================================================
  // NoDestructor 模块演示
  // ========================================================================

  std::cout << "\n===== NoDestructor 验证 =====\n";
  {
    static leveldb::NoDestructor<std::string> singleton("immortal");
    std::cout << "NoDestructor<string>: " << *singleton.get()
              << ((*singleton.get() == "immortal") ? "  ✓" : "  ✗") << "\n";
  }

  // ========================================================================
  // Logging 模块演示
  // ========================================================================

  std::cout << "\n===== Logging 工具验证 =====\n";
  {
    auto s = leveldb::NumberToString(12345678);
    std::cout << "NumberToString:     " << s
              << (s == "12345678" ? "  ✓" : "  ✗") << "\n";

    auto esc = leveldb::EscapeString("hello\x01world");
    std::cout << "EscapeString:       " << esc << "\n";

    // ConsumeDecimalNumber — 成功
    std::string_view input1 = "12345abc";
    auto num1 = leveldb::ConsumeDecimalNumber(input1);
    std::cout << "ConsumeDecimal:     " << (num1 && *num1 == 12345 ? "✓" : "✗")
              << " remaining=\"" << input1 << "\"\n";

    // ConsumeDecimalNumber — 溢出
    std::string_view input2 = "99999999999999999999";
    auto num2 = leveldb::ConsumeDecimalNumber(input2);
    std::cout << "Overflow detect:    " << (!num2 ? "✓" : "✗") << "\n";

    // ConsumeDecimalNumber — 无数字
    std::string_view input3 = "abc";
    auto num3 = leveldb::ConsumeDecimalNumber(input3);
    std::cout << "No digits detect:   " << (!num3 ? "✓" : "✗") << "\n";
  }

  // ========================================================================
  // StdFileSystem & Options 妯″潡婕旂ず
  // ========================================================================

  std::cout << "\n===== StdFileSystem 楠岃瘉 =====\n";
  {
    leveldb::StdFileSystem fs;
    auto test_dir_res = fs.GetTestDirectory();
    if (test_dir_res) {
      auto test_dir = *test_dir_res;
      std::cout << "Test directory:     " << test_dir.string() << " 鉁?\n";

      auto file_path = test_dir / "test_file.txt";
      auto write_res = fs.NewWritableFile(file_path);
      if (write_res) {
        auto& file = *write_res;
        file.Append("Hello StdFileSystem!");
        file.Close();
        std::cout << "WritableFile:       鉁?\n";

        auto read_res = fs.NewSequentialFile(file_path);
        if (read_res) {
          auto& rfile = *read_res;
          char scratch[100];
          auto data = rfile.Read(100, scratch);
          std::cout << "SequentialFile read:" << (data && *data == "Hello StdFileSystem!" ? " 鉁?" : " 鉁?") << "\n";
        } else {
          std::cout << "SequentialFile err: " << read_res.error().ToString() << "\n";
        }
        
        fs.RemoveFile(file_path);

        // 测试 LockFile / UnlockFile
        auto lock_path = test_dir / "LOCK";
        auto lock_res = fs.LockFile(lock_path);
        if (lock_res) {
          std::cout << "LockFile:           ✓\n";
          auto second_lock_res = fs.LockFile(lock_path);
          if (!second_lock_res) {
            std::cout << "LockFile exclusive: ✓\n";
          } else {
            std::cout << "LockFile exclusive: failed (expected error)\n";
            fs.UnlockFile(std::move(*second_lock_res));
          }
          fs.UnlockFile(std::move(*lock_res));
          std::cout << "UnlockFile:         ✓\n";
        } else {
          std::cout << "LockFile err:       " << lock_res.error().ToString() << " ✗\n";
        }
        fs.RemoveFile(lock_path);
      }
    }

    // 娴嬭瘯 Options 妯℃澘
    leveldb::Options<leveldb::StdFileSystem> options;
    options.env = &fs;
    options.create_if_missing = true;
    std::cout << "Options template:   鉁?\n";
  }

  // ========================================================================
  // WAL (Write-Ahead Log) Module Test
  // ========================================================================

  std::cout << "\n===== WAL Log Writer / Reader 验证 =====\n";
  {
    leveldb::StdFileSystem fs;
    auto test_dir_res = fs.GetTestDirectory();
    if (test_dir_res) {
      auto test_dir = *test_dir_res;
      auto file_path = test_dir / "test_wal.log";

      // 1. Write records
      auto write_res = fs.NewWritableFile(file_path);
      if (write_res) {
        leveldb::log::Writer<leveldb::StdFileSystem::WritableFile> writer(&write_res.value());
        writer.AddRecord("hello");
        writer.AddRecord("world");

        std::string big_record(leveldb::log::kBlockSize + 100, 'x'); // forces fragmentation
        writer.AddRecord(big_record);
        write_res.value().Close();
        std::cout << "WAL Writer:         ✓\n";
      }

      // 2. Read records
      auto read_res = fs.NewSequentialFile(file_path);
      if (read_res) {
        int corruptions = 0;
        leveldb::log::Reporter reporter = [&corruptions](size_t bytes, const leveldb::Status& s) {
          corruptions++;
          std::cout << "  Corruption: " << s.ToString() << " (" << bytes << " bytes)\n";
        };

        leveldb::log::Reader<leveldb::StdFileSystem::SequentialFile> reader(
            &read_res.value(), reporter, true, 0);

        std::string scratch;
        auto rec1 = reader.ReadRecord(scratch);
        std::cout << "WAL Reader rec1:    " << (rec1 && *rec1 == "hello" ? "✓" : "✗") << " (len=" << (rec1 ? rec1->size() : 0) << ")\n";
        
        auto rec2 = reader.ReadRecord(scratch);
        std::cout << "WAL Reader rec2:    " << (rec2 && *rec2 == "world" ? "✓" : "✗") << " (len=" << (rec2 ? rec2->size() : 0) << ")\n";
        
        auto rec3 = reader.ReadRecord(scratch);
        std::cout << "WAL Reader rec3:    " << (rec3 && rec3->size() == leveldb::log::kBlockSize + 100 ? "✓" : "✗") << "\n";
        
        auto rec4 = reader.ReadRecord(scratch); // should be nullopt
    std::cout << "WAL Reader EOF:     " << (!rec4 ? "✓" : "✗") << "\n";
        
        std::cout << "WAL Corruptions:    " << corruptions << " (expect 0)\n";
      }

      fs.RemoveFile(file_path);
    }
    
    std::cout << "\n===== MemTable / SkipList 验证 =====\n";
    {
      leveldb::InternalKeyComparator cmp(leveldb::BytewiseComparator());
      auto memtable = std::make_shared<leveldb::MemTable>(cmp);
      
      // Add some entries
      memtable->Add(100, leveldb::kTypeValue, "key1", "value1");
      memtable->Add(101, leveldb::kTypeValue, "key2", "value2");
      memtable->Add(102, leveldb::kTypeDeletion, "key2", ""); // delete key2
      memtable->Add(103, leveldb::kTypeValue, "key3", "value3");

      // Test Get
      auto test_get = [&](std::string_view k, leveldb::SequenceNumber seq) {
        leveldb::LookupKey lkey(k, seq);
        auto res = memtable->Get(lkey);
        if (res) {
          if (*res) std::cout << "Get(" << k << "@" << seq << "): Found: " << **res << "\n";
          else std::cout << "Get(" << k << "@" << seq << "): Not Found\n";
        } else {
          std::cout << "Get(" << k << "@" << seq << "): Error: " << res.error().ToString() << "\n";
        }
      };

      test_get("key1", 200); // Should find value1
      test_get("key2", 200); // Should be deleted
      test_get("key2", 100); // Should be not found or what? Wait, LookupKey sequence is 100, so it looks for <= 100. It doesn't exist at <=100! (Added at 101)
      test_get("key3", 200); // Should find value3

      // Test Iterator
      auto iter = memtable->NewIterator();
      iter->SeekToFirst();
      int count = 0;
      std::cout << "Iterator Output:\n";
      while (iter->Valid()) {
        auto parsed = leveldb::ParseInternalKey(iter->key());
        if (parsed) {
          std::cout << "  " << parsed->DebugString() << " -> " << iter->value() << "\n";
        }
        iter->Next();
        count++;
      }
      if (count == 4) {
        std::cout << "MemTable Iterator: ✓ (count=4)\n";
      } else {
        std::cout << "MemTable Iterator: Failed (count=" << count << ")\n";
      }
    }

    std::cout << "\n===== VersionEdit 编解码验证 =====\n";
    {
      leveldb::VersionEdit edit;
      edit.SetComparatorName("foo");
      edit.SetLogNumber(1234);
      edit.SetNextFile(4321);
      edit.SetLastSequence(8888);
      
      leveldb::InternalKey k1("key1", 100, leveldb::kTypeValue);
      leveldb::InternalKey k2("key2", 200, leveldb::kTypeDeletion);
      
      edit.AddFile(2, 555, 9999, k1, k2);
      edit.RemoveFile(3, 777);
      
      std::string encoded;
      edit.EncodeTo(encoded);
      
      leveldb::VersionEdit decoded_edit;
      auto st = decoded_edit.DecodeFrom(encoded);
      if (st) {
        std::cout << "VersionEdit Decode: ✓\n";
        std::string debug = decoded_edit.DebugString();
        if (debug.find("foo") != std::string::npos && 
            debug.find("1234") != std::string::npos &&
            debug.find("4321") != std::string::npos &&
            debug.find("555") != std::string::npos) {
           std::cout << "VersionEdit Content: ✓\n";
        } else {
           std::cout << "VersionEdit Content: ✗\n";
        }
      } else {
        std::cout << "VersionEdit Decode: Error " << st.error().ToString() << "\n";
      }
    }
    
    std::cout << "\n===== DBImpl 集成验证 =====\n";
    {
      leveldb::Options<leveldb::StdFileSystem> options;
      options.create_if_missing = true;
      options.env = &fs; // From earlier in main.cpp
      options.comparator = leveldb::BytewiseComparator();
      
      leveldb::DestroyDB("testdb", options);
      auto db_res = leveldb::DB::Open(options, "testdb");
      if (db_res) {
        std::cout << "DB::Open: ✓\n";
        auto db = std::move(*db_res);
        
        leveldb::WriteOptions wopt;
        auto put_res = db->Put(wopt, "hello", "world");
        if (put_res) {
          std::cout << "DB::Put: ✓\n";
        } else {
          std::cout << "DB::Put Error: " << put_res.error().ToString() << "\n";
        }
        
        leveldb::ReadOptions ropt;
        auto get_res = db->Get(ropt, "hello");
        if (get_res) {
          if (*get_res) {
            std::cout << "DB::Get('hello'): " << **get_res << " ✓\n";
          } else {
             std::cout << "DB::Get('hello'): Not Found ✗\n";
          }
        } else {
          std::cout << "DB::Get Error: " << get_res.error().ToString() << "\n";
        }
        
        // Test missing key
        auto get_miss = db->Get(ropt, "missing");
        if (!get_miss && get_miss.error().IsNotFound()) {
          std::cout << "DB::Get missing key: NotFound ✓\n";
        }

        // ==========================================
        // 1. WriteBatch 验证
        // ==========================================
        leveldb::WriteBatch batch;
        batch.Put("batch1", "val1");
        batch.Put("batch2", "val2");
        batch.Delete("batch1");
        auto batch_st = db->Write(wopt, &batch);
        bool batch_ok = false;
        if (batch_st) {
          auto get_b1 = db->Get(ropt, "batch1");
          auto get_b2 = db->Get(ropt, "batch2");
          if (!get_b1 && get_b1.error().IsNotFound() && get_b2 && *get_b2 && **get_b2 == "val2") {
            batch_ok = true;
          }
        }
        std::cout << "DB::WriteBatch:       " << (batch_ok ? "✓" : "✗") << "\n";

        // ==========================================
        // 2. Snapshot / MVCC 验证
        // ==========================================
        db->Put(wopt, "snapkey", "v1");
        auto snapshot = db->GetSnapshot();
        db->Put(wopt, "snapkey", "v2");
        db->Put(wopt, "newkey", "v_new");

        leveldb::ReadOptions snap_ropt;
        snap_ropt.snapshot = snapshot.get();

        auto get_snap = db->Get(snap_ropt, "snapkey");
        auto get_snap_new = db->Get(snap_ropt, "newkey");
        auto get_no_snap = db->Get(ropt, "snapkey");
        auto get_no_snap_new = db->Get(ropt, "newkey");

        bool snap_ok = false;
        if (get_snap && *get_snap && **get_snap == "v1" &&
            !get_snap_new && get_snap_new.error().IsNotFound() &&
            get_no_snap && *get_no_snap && **get_no_snap == "v2" &&
            get_no_snap_new && *get_no_snap_new && **get_no_snap_new == "v_new") {
          snap_ok = true;
        }
        std::cout << "DB::Snapshot (MVCC):  " << (snap_ok ? "✓" : "✗") << "\n";

        // ==========================================
        // 3. 多线程并发读写验证
        // ==========================================
        std::vector<std::thread> threads;
        std::atomic<bool> threads_ok{true};
        for (int t = 0; t < 4; ++t) {
          threads.emplace_back([t, &db, &threads_ok]() {
            leveldb::WriteOptions wopt;
            leveldb::ReadOptions ropt;
            std::string prefix = "thread_" + std::to_string(t) + "_";
            for (int i = 0; i < 50; ++i) {
              std::string key = prefix + std::to_string(i);
              std::string val = "value_" + std::to_string(i * 10);
              if (!db->Put(wopt, key, val)) {
                threads_ok = false;
              }
              auto get_res = db->Get(ropt, key);
              if (!get_res || !*get_res || **get_res != val) {
                threads_ok = false;
              }
            }
          });
        }
        for (auto& th : threads) {
          th.join();
        }
        std::cout << "DB::Concurrent Threads: " << (threads_ok ? "✓" : "✗") << "\n";

        // ==========================================
        // 4. CompactRange, GetApproximateSizes 与 Iterator 逆向遍历验证
        // ==========================================
        db->Put(wopt, "comp_a", "val_a");
        db->Put(wopt, "comp_b", "val_b");
        db->Put(wopt, "comp_c", "val_c");
        
        // 测试 CompactRange
        db->CompactRange(nullptr, nullptr); // Compact everything
        std::cout << "DB::CompactRange:     ✓\n";

        // 测试 GetApproximateSizes
        leveldb::Range range("comp_a", "comp_d");
        uint64_t sizes[1] = {0};
        db->GetApproximateSizes(&range, 1, sizes);
        std::cout << "DB::GetApproximateSizes: ✓ (size=" << sizes[0] << ")\n";

        // 测试 Iterator 逆向遍历 (Seek & Prev)
        std::unique_ptr<leveldb::Iterator> iter(db->NewIterator(ropt));
        iter->Seek("comp_c");
        bool iter_rev_ok = false;
        if (iter->Valid() && iter->key() == "comp_c") {
          iter->Prev();
          if (iter->Valid() && iter->key() == "comp_b") {
            iter->Prev();
            if (iter->Valid() && iter->key() == "comp_a") {
              iter_rev_ok = true;
            }
          }
        }
        std::cout << "DB::Iterator Reverse Scan: " << (iter_rev_ok ? "✓" : "✗") << "\n";
      } else {
        std::cout << "DB::Open Error: " << db_res.error().ToString() << "\n";
      }
    }
  }

  // ========================================================================
  // Block Builder / Block 验证
  // ========================================================================

  std::cout << "\n===== Block Builder / Block 验证 =====\n";
  {
    leveldb::BlockBuilderOptions builder_options;
    builder_options.comparator = leveldb::BytewiseComparator();
    builder_options.block_restart_interval = 2; // trigger restart logic

    leveldb::BlockBuilder builder(builder_options);
    builder.Add("apple", "red");
    builder.Add("apricot", "orange");
    builder.Add("banana", "yellow");
    builder.Add("blueberry", "blue");

    std::string_view block_data = builder.Finish();

    leveldb::BlockContents contents;
    contents.data = block_data;
    contents.cachable = false;
    contents.heap_data = nullptr;

    leveldb::Block block(std::move(contents));
    auto iter = block.NewIterator(builder_options.comparator);

    iter->SeekToFirst();
    std::cout << "Block first:        " << (iter->Valid() && iter->key() == "apple" ? "✓" : "✗") << "\n";

    iter->Seek("b"); // should find "banana"
    std::cout << "Block seek 'b':     " << (iter->Valid() && iter->key() == "banana" ? "✓" : "✗") << "\n";

    iter->Seek("blueberry");
    std::cout << "Block seek 'bl':    " << (iter->Valid() && iter->key() == "blueberry" ? "✓" : "✗") << "\n";

    iter->Next(); // EOF
    std::cout << "Block EOF:          " << (!iter->Valid() ? "✓" : "✗") << "\n";
  }

  // ========================================================================
  // SSTable (TableBuilder / Table) 验证
  // ========================================================================

  std::cout << "\n===== SSTable Builder / Table 验证 =====\n";
  {
    leveldb::StdFileSystem fs;
    auto test_dir_res = fs.GetTestDirectory();
    if (test_dir_res) {
      auto test_dir = *test_dir_res;
      auto file_path = test_dir / "test_sstable.ldb";
      const leveldb::FilterPolicy* fp = leveldb::NewBloomFilterPolicy(10);

      // 构建 SSTable
      auto write_res = fs.NewWritableFile(file_path);
      if (write_res) {
        leveldb::TableBuilderOptions tb_options;
        tb_options.comparator = leveldb::BytewiseComparator();
        tb_options.block_size = 64; // force multiple blocks
        tb_options.filter_policy = fp; // enable bloom filter

        leveldb::TableBuilder builder(tb_options, &write_res.value());

        // 添加 100 个条目
        for (int i = 0; i < 100; i++) {
          std::string key = "key" + std::string(3 - std::to_string(i).length(), '0') + std::to_string(i);
          std::string value = "value_for_" + key;
          builder.Add(key, value);
        }

        auto s = builder.Finish();
        std::cout << "TableBuilder Finish: " << (s ? "✓" : s.error().ToString()) << "\n";
        std::cout << "Table FileSize:      " << builder.FileSize() << " bytes\n";
        write_res.value().Close();
      }

      // 读取 SSTable
      auto read_res = fs.NewRandomAccessFile(file_path);
      if (read_res) {
        leveldb::TableOptions table_options;
        table_options.comparator = leveldb::BytewiseComparator();
        table_options.filter_policy = fp;

        uint64_t file_size = 0;
        if (auto s = fs.GetFileSize(file_path); s) {
          file_size = *s;
        }

        auto table_res = leveldb::Table<leveldb::StdFileSystem::RandomAccessFile>::Open(
            table_options, &read_res.value(), file_size);
        
        if (table_res) {
          auto& table = *table_res;
          std::cout << "Table Open:          ✓\n";

          leveldb::ReadOptions ropts;
          
          // 测试 InternalGet
          int get_count = 0;
          table->InternalGet(ropts, "key050", [&get_count](std::string_view k, std::string_view v) {
            if (k == "key050" && v == "value_for_key050") get_count++;
          });
          std::cout << "Table InternalGet:   " << (get_count == 1 ? "✓" : "✗") << "\n";

          // 测试不存在的 key (应该被 filter 拦截，或者直接找不到)
          int not_found_count = 0;
          table->InternalGet(ropts, "key_not_exist", [&not_found_count](std::string_view k, std::string_view v) {
            not_found_count++;
          });
          std::cout << "Table Get(missing):  " << (not_found_count == 0 ? "✓" : "✗") << "\n";

          // 测试迭代器
          auto iter = table->NewIterator(ropts);
          iter->SeekToFirst();
          int count = 0;
          bool valid_order = true;
          std::string last_k = "";
          while (iter->Valid()) {
            if (last_k != "" && leveldb::BytewiseComparator()->Compare(iter->key(), last_k) <= 0) {
              valid_order = false;
            }
            last_k = std::string(iter->key());
            count++;
            iter->Next();
          }
          std::cout << "Table Iterator Scan: " << (count == 100 && valid_order ? "✓" : "✗") << " (count=" << count << ")\n";
        } else {
          std::cout << "Table Open failed:   " << table_res.error().ToString() << "\n";
        }
      }

      fs.RemoveFile(file_path);
      delete fp;
    }
  }

  std::cout << "\n===== Cache (LRUCache) 验证 =====\n";
  {
    std::unique_ptr<leveldb::Cache> cache(leveldb::NewLRUCache(1000));
    int val1 = 100;
    int val2 = 200;
    bool deleted1 = false;
    bool deleted2 = false;

    auto deleter1 = [&deleted1](std::string_view key, void* value) {
      deleted1 = true;
    };
    auto deleter2 = [&deleted2](std::string_view key, void* value) {
      deleted2 = true;
    };

    // Test Insert and Lookup
    {
      auto h1 = cache->Insert("key1", &val1, 1, deleter1);
      auto h2 = cache->Insert("key2", &val2, 1, deleter2);

      auto lookup1 = cache->Lookup("key1");
      std::cout << "Cache Lookup key1:   " << (lookup1 && *static_cast<int*>(cache->Value(lookup1.get())) == 100 ? "✓" : "✗") << "\n";
    }

    // After h1 and h2 go out of scope, their refs are released.
    // If we erase key1, deleter1 should be called because there are no remaining handles.
    cache->Erase("key1");
    std::cout << "Cache Erase key1:    " << (deleted1 ? "✓" : "✗") << "\n";

    // Prune test
    cache->Prune();
    std::cout << "Cache TotalCharge:   " << (cache->TotalCharge() == 0 ? "✓" : "✗") << "\n";
  }

  std::cout << "\n===== Bloom Filter Policy 验证 =====\n";
  {
    std::unique_ptr<const leveldb::FilterPolicy> policy(leveldb::NewBloomFilterPolicy(10));
    std::cout << "Bloom Filter Name:   " << policy->Name() << " (expect leveldb.BuiltinBloomFilter2)\n";

    std::vector<std::string_view> keys = {"hello", "world", "bloom"};
    std::string filter;
    policy->CreateFilter(keys, filter);

    std::cout << "Bloom KeyMayMatch:   "
              << (policy->KeyMayMatch("hello", filter) ? "✓" : "✗") << "\n";
    std::cout << "Bloom KeyMayMatch 2: "
              << (policy->KeyMayMatch("world", filter) ? "✓" : "✗") << "\n";
    std::cout << "Bloom KeyMayMatch 3: "
              << (!policy->KeyMayMatch("missing", filter) ? "✓" : "✗") << "\n";
  }

  std::cout << "\n===== DumpFile Utility 验证 =====\n";
  {
    leveldb::StdFileSystem fs;
    auto test_dir_res = fs.GetTestDirectory();
    if (test_dir_res) {
      auto test_dir = *test_dir_res;
      // We will create a small log file to dump
      auto log_path = test_dir / "000001.log";
      auto write_res = fs.NewWritableFile(log_path);
      if (write_res) {
        leveldb::log::Writer<leveldb::StdFileSystem::WritableFile> writer(&write_res.value());
        leveldb::WriteBatch batch;
        batch.Put("a", "1");
        batch.Delete("b");
        writer.AddRecord(leveldb::WriteBatchInternal::Contents(&batch));
        write_res.value().Close();

        std::string out;
        auto dump_res = leveldb::DumpFile(&fs, log_path, [&out](std::string_view s) {
          out.append(s);
        });

        std::cout << "DumpFile Log:        " 
                  << (dump_res && out.find("put 'a' '1'") != std::string::npos ? "✓" : "✗") << "\n";
      }
      fs.RemoveFile(log_path);
    }
  }

  // ========================================================================
  // FileName 验证
  // ========================================================================
  std::cout << "\n===== FileName 验证 =====\n";
  {
    struct TestCase {
      std::string_view fname;
      uint64_t number;
      leveldb::FileType type;
    };
    std::vector<TestCase> cases = {
        {"100.log", 100, leveldb::kLogFile},
        {"0.log", 0, leveldb::kLogFile},
        {"0.sst", 0, leveldb::kTableFile},
        {"0.ldb", 0, leveldb::kTableFile},
        {"CURRENT", 0, leveldb::kCurrentFile},
        {"LOCK", 0, leveldb::kDBLockFile},
        {"MANIFEST-2", 2, leveldb::kDescriptorFile},
        {"MANIFEST-7", 7, leveldb::kDescriptorFile},
        {"LOG", 0, leveldb::kInfoLogFile},
        {"LOG.old", 0, leveldb::kInfoLogFile},
        {"18446744073709551615.log", 18446744073709551615ull, leveldb::kLogFile},
    };
    bool ok = true;
    for (const auto& tc : cases) {
      auto res = leveldb::ParseFileName(tc.fname);
      if (!res) {
        std::cout << "ParseFileName failed for: " << tc.fname << "\n";
        ok = false;
      } else if (res->number != tc.number || res->type != tc.type) {
        std::cout << "ParseFileName mismatch for: " << tc.fname << " expected number=" << tc.number << ", got=" << res->number << "\n";
        ok = false;
      }
    }

    std::vector<std::string_view> errors = {
        "",
        "foo",
        "foo-dx-100.log",
        ".log",
        "manifest",
        "CURREN",
        "CURRENTX",
        "MANIFES",
        "MANIFEST",
        "MANIFEST-",
        "XMANIFEST-3",
        "MANIFEST-3x",
        "LOC",
        "LOCKx",
        "LO",
        "LOGx",
        "18446744073709551616.log",
        "184467440737095516150.log",
        "100",
        "100.",
        "100.lop"
    };
    for (const auto& err : errors) {
      auto res = leveldb::ParseFileName(err);
      if (res) {
        std::cout << "ParseFileName should have failed but succeeded for: " << err << "\n";
        ok = false;
      }
    }
    std::cout << "ParseFileName:        " << (ok ? "✓" : "✗") << "\n";
  }

  // ========================================================================
  // Read-Triggered Auto Compaction 验证
  // ========================================================================
  std::cout << "\n===== Read-Triggered Auto Compaction 验证 =====\n";
  {
    leveldb::StdFileSystem fs;
    auto test_dir_res = fs.GetTestDirectory();
    if (test_dir_res) {
      auto test_dir = *test_dir_res;
      auto dbname = test_dir / "autocompact_test_db";
      leveldb::DestroyDB(dbname.string(), leveldb::Options<leveldb::StdFileSystem>());

      leveldb::Options<leveldb::StdFileSystem> options;
      options.create_if_missing = true;
      options.comparator = leveldb::BytewiseComparator();
      options.compression = leveldb::CompressionType::kNoCompression;
      
      auto open_res = leveldb::DB::Open(options, dbname.string());
      if (open_res) {
        std::unique_ptr<leveldb::DB> db = std::move(*open_res);
        leveldb::DBImpl* dbi = reinterpret_cast<leveldb::DBImpl*>(db.get());
        
        dbi->Put(leveldb::WriteOptions(), "key", "small_value");
        dbi->TEST_CompactMemTable();
        
        std::string val(100 * 1024, 'a');
        dbi->Put(leveldb::WriteOptions(), "key", val);
        dbi->TEST_CompactMemTable();
        
        auto get_prop = [](leveldb::DBImpl* dbi, std::string_view prop) -> std::string {
          auto res = dbi->GetProperty(prop);
          if (res && *res) {
            return **res;
          }
          return "null";
        };

        auto num_l0 = get_prop(dbi, "leveldb.num-files-at-level0");
        auto num_l1 = get_prop(dbi, "leveldb.num-files-at-level1");
        std::cout << "SSTables initial:\n" << get_prop(dbi, "leveldb.sstables") << "\n";
        std::cout << "Initial L0 files:     " << num_l0 << " (expect 0)\n";
        std::cout << "Initial L1 files:     " << num_l1 << " (expect 1)\n";

        {
          std::unique_ptr<leveldb::Iterator> iter(db->NewIterator(leveldb::ReadOptions()));
          for (int i = 0; i < 2000; ++i) {
            iter->Seek("key");
          }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        num_l0 = get_prop(dbi, "leveldb.num-files-at-level0");
        num_l1 = get_prop(dbi, "leveldb.num-files-at-level1");
        std::cout << "SSTables after auto compaction:\n" << get_prop(dbi, "leveldb.sstables") << "\n";
        std::cout << "After auto compaction L1: " << num_l1 << " (expect 0)\n";
        
        bool ok = (num_l1 == "0");
        std::cout << "Auto Compaction:      " << (ok ? "✓" : "✗") << "\n";

        db.reset();
      }
      leveldb::DestroyDB(dbname.string(), leveldb::Options<leveldb::StdFileSystem>());
    }
  }

  return 0;
}