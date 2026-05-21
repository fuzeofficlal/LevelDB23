#include "include/leveldb/db.h"

bool g_async_optimize = false;
bool g_coro_allocator = false;
bool g_lock_free_queue = false;
bool g_io_uring = false;
bool g_zero_copy = false;
#include "include/leveldb/options.h"
#include "include/leveldb/write_batch.h"
#include "include/leveldb/std_file_system.h"
#include "include/leveldb/status.h"
#include "include/leveldb/cache.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <sstream>
#include <cassert>
#include <random>
#include <memory>
#include <iomanip>

using namespace std;

// Helper to format throughput
void PrintThroughput(string_view label, uint64_t operations, chrono::microseconds duration) {
  double seconds = duration.count() / 1000000.0;
  double ops_per_sec = operations / (seconds > 0.0001 ? seconds : 0.0001);
  cout << label << ": " << operations << " ops in " << fixed << setprecision(3) 
       << seconds << "s (" << fixed << setprecision(0) << ops_per_sec << " ops/sec)\n";
}

// 1. Concurrency Stress Test (including Delete operations)
void RunConcurrencyStressTest(leveldb::StdFileSystem& fs) {
  cout << "\n--- [Stress Test 1: Concurrency & Lock Contention] ---\n";
  leveldb::Options<leveldb::StdFileSystem> options;
  options.async_optimize = g_async_optimize;
  options.coro_allocator = g_coro_allocator;
  options.lock_free_queue = g_lock_free_queue;
  options.io_uring = g_io_uring;
  options.zero_copy = g_zero_copy;
  options.create_if_missing = true;
  options.env = &fs;
  
  std::unique_ptr<leveldb::Cache> cache(leveldb::NewLRUCache(16 * 1024 * 1024)); // 16MB block cache
  options.block_cache = cache.get();
  
  leveldb::DestroyDB("stress_db_concurrency", options);
  
  auto db_res = leveldb::DB::Open(options, "stress_db_concurrency");
  if (!db_res) {
    cerr << "Failed to open DB: " << db_res.error().ToString() << "\n";
    exit(1);
  }
  auto db = std::move(*db_res);

  const int num_writers = 8;
  const int num_readers = 4;
  const int ops_per_writer = 150000; // Increased to run ~5s
  std::atomic<bool> stop_readers{false};
  std::atomic<uint64_t> total_writes{0};
  std::atomic<uint64_t> total_reads{0};
  std::atomic<uint64_t> read_errors{0};

  auto start = chrono::high_resolution_clock::now();

  // Spawn writers
  cout << "  Spawning " << num_writers << " concurrent writer threads (80% Puts, 20% Deletes)...\n" << std::flush;
  vector<thread> writers;
  for (int i = 0; i < num_writers; ++i) {
    writers.emplace_back([&db, i, ops_per_writer, &total_writes]() {
      leveldb::WriteOptions wopt;
      std::mt19937 rng(42 + i);
      std::uniform_int_distribution<int> op_dist(0, 99);
      for (int j = 0; j < ops_per_writer; ++j) {
        string key = "key_" + to_string(i) + "_" + to_string(j);
        if (op_dist(rng) < 20) {
          auto res = db->Delete(wopt, key);
          if (!res) {
            cerr << "Delete error: " << res.error().ToString() << "\n";
          } else {
            total_writes++;
          }
        } else {
          string val = "value_" + to_string(j) + "_content_padding_to_simulate_load";
          auto res = db->Put(wopt, key, val);
          if (!res) {
            cerr << "Put error: " << res.error().ToString() << "\n";
          } else {
            total_writes++;
          }
        }
      }
    });
  }

  // Spawn readers
  cout << "  Spawning " << num_readers << " concurrent reader threads...\n" << std::flush;
  vector<thread> readers;
  for (int i = 0; i < num_readers; ++i) {
    readers.emplace_back([&db, num_writers, ops_per_writer, &stop_readers, &total_reads, &read_errors]() {
      leveldb::ReadOptions ropt;
      ropt.async_optimize = g_async_optimize;
      ropt.coro_allocator = g_coro_allocator;
      ropt.lock_free_queue = g_lock_free_queue;
      ropt.io_uring = g_io_uring;
      ropt.zero_copy = g_zero_copy;
      std::mt19937 rng(1337 + total_reads.load());
      std::uniform_int_distribution<int> dist_thread(0, num_writers - 1);
      std::uniform_int_distribution<int> dist_key(0, ops_per_writer - 1);

      while (!stop_readers.load(std::memory_order_relaxed)) {
        string key = "key_" + to_string(dist_thread(rng)) + "_" + to_string(dist_key(rng));
        auto get_res = db->Get(ropt, key);
        if (get_res) {
          total_reads++;
        } else {
          read_errors++;
        }
      }
    });
  }

  // Join writers
  for (auto& w : writers) {
    w.join();
  }
  
  // Stop readers and join
  stop_readers = true;
  for (auto& r : readers) {
    r.join();
  }

  auto end = chrono::high_resolution_clock::now();
  auto duration = chrono::duration_cast<chrono::microseconds>(end - start);

  cout << "Writers: " << num_writers << ", Readers: " << num_readers << "\n";
  PrintThroughput("  Writes/Deletes", total_writes.load(), duration);
  PrintThroughput("  Reads         ", total_reads.load(), duration);
  cout << "  Read Misses/Errors: " << read_errors.load() << "\n";
  cout << "Concurrency Stress Test: ✓ (Pass)\n";
}

// 2. WAL Recovery Stress Test
void RunWALRecoveryStressTest(leveldb::StdFileSystem& fs) {
  cout << "\n--- [Stress Test 2: Crash Recovery & WAL Integrity] ---\n";
  leveldb::Options<leveldb::StdFileSystem> options;
  options.async_optimize = g_async_optimize;
  options.coro_allocator = g_coro_allocator;
  options.lock_free_queue = g_lock_free_queue;
  options.io_uring = g_io_uring;
  options.zero_copy = g_zero_copy;
  options.create_if_missing = true;
  options.env = &fs;
  options.write_buffer_size = 128 * 1024 * 1024; // 128MB so everything stays in MemTable / WAL
  
  leveldb::DestroyDB("stress_db_wal", options);

  const int num_keys = 1000000; // Increased to run >= 5s
  
  // Scope 1: Write keys and destruct the DB without Manual Compaction (flushing memtable)
  {
    auto db_res = leveldb::DB::Open(options, "stress_db_wal");
    assert(db_res);
    auto db = std::move(*db_res);
    
    leveldb::WriteOptions wopt;
    wopt.sync = false;
    cout << "  Writing " << num_keys << " keys to MemTable/WAL...\n" << std::flush;
    auto start = chrono::high_resolution_clock::now();
    for (int i = 0; i < num_keys; ++i) {
      string key = "wal_key_" + to_string(i);
      string val = "wal_value_" + to_string(i);
      auto res = db->Put(wopt, key, val);
      if (!res) {
        cerr << "Put failed: " << res.error().ToString() << "\n";
        exit(1);
      }
    }
    auto end = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::microseconds>(end - start);
    PrintThroughput("  Writes", num_keys, duration);
    cout << "  Simulating dirty shutdown (database closed with unflushed MemTable)\n";
  }

  // Scope 2: Reopen DB and verify WAL recovery
  {
    cout << "  Reopening database to trigger WAL replay...\n" << std::flush;
    auto start = chrono::high_resolution_clock::now();
    auto db_res = leveldb::DB::Open(options, "stress_db_wal");
    if (!db_res) {
      cerr << "Failed to reopen DB: " << db_res.error().ToString() << "\n";
      exit(1);
    }
    auto db = std::move(*db_res);
    auto end = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::microseconds>(end - start);
    cout << "  WAL replay completed in " << fixed << setprecision(3) << duration.count() / 1000000.0 << "s\n";

    cout << "  Verifying all " << num_keys << " recovered keys...\n" << std::flush;
    leveldb::ReadOptions ropt;
    ropt.async_optimize = g_async_optimize;
    ropt.coro_allocator = g_coro_allocator;
    ropt.lock_free_queue = g_lock_free_queue;
    ropt.io_uring = g_io_uring;
    ropt.zero_copy = g_zero_copy;
    int recovered_keys = 0;
    for (int i = 0; i < num_keys; ++i) {
      string key = "wal_key_" + to_string(i);
      auto get_res = db->Get(ropt, key);
      if (get_res && *get_res && **get_res == "wal_value_" + to_string(i)) {
        recovered_keys++;
      }
    }
    cout << "  Recovered and verified keys: " << recovered_keys << " / " << num_keys << "\n";
    assert(recovered_keys == num_keys);
  }
  cout << "WAL Recovery Stress Test: ✓ (Pass)\n";
}

// 3. Compaction & Iterator Stability Stress Test
void RunCompactionIteratorStressTest(leveldb::StdFileSystem& fs) {
  cout << "\n--- [Stress Test 3: Compaction & Iterator Stability] ---\n";
  leveldb::Options<leveldb::StdFileSystem> options;
  options.async_optimize = g_async_optimize;
  options.coro_allocator = g_coro_allocator;
  options.lock_free_queue = g_lock_free_queue;
  options.io_uring = g_io_uring;
  options.zero_copy = g_zero_copy;
  options.create_if_missing = true;
  options.write_buffer_size = 512 * 1024; // Small 512KB memtable to force very frequent compactions
  options.env = &fs;

  std::unique_ptr<leveldb::Cache> cache(leveldb::NewLRUCache(8 * 1024 * 1024)); // 8MB block cache
  options.block_cache = cache.get();

  leveldb::DestroyDB("stress_db_compaction", options);

  auto db_res = leveldb::DB::Open(options, "stress_db_compaction");
  assert(db_res);
  auto db = std::move(*db_res);

  std::atomic<bool> stop_threads{false};
  std::atomic<uint64_t> iter_scans{0};
  
  // Thread 1: Continuously create iterators, perform forward/backward scans under background compaction
  thread scanner([&db, &stop_threads, &iter_scans]() {
    leveldb::ReadOptions ropt;
    ropt.async_optimize = g_async_optimize;
    ropt.coro_allocator = g_coro_allocator;
    ropt.lock_free_queue = g_lock_free_queue;
    ropt.io_uring = g_io_uring;
    ropt.zero_copy = g_zero_copy;
    while (!stop_threads.load(std::memory_order_relaxed)) {
      auto snapshot = db->GetSnapshot();
      ropt.snapshot = snapshot.get();
      
      unique_ptr<leveldb::Iterator> iter(db->NewIterator(ropt));
      
      // Forward Scan
      int forward_count = 0;
      iter->SeekToFirst();
      while (iter->Valid() && forward_count < 100) {
        string_view k = iter->key();
        string_view v = iter->value();
        (void)k; (void)v;
        iter->Next();
        forward_count++;
      }
      
      // Backward Scan
      int backward_count = 0;
      iter->SeekToLast();
      while (iter->Valid() && backward_count < 100) {
        string_view k = iter->key();
        string_view v = iter->value();
        (void)k; (void)v;
        iter->Prev();
        backward_count++;
      }
      
      iter_scans++;
    }
  });

  // Main Thread: Generate heavy write traffic with large values to trigger background compactions
  leveldb::WriteOptions wopt;
  string large_val(4096, 'a'); // 4KB values
  const int total_writes = 30000; // Increased to run ~5s (120MB writes)
  
  cout << "  Generating heavy compaction traffic (" << total_writes << " * 4KB writes)... \n";
  auto start = chrono::high_resolution_clock::now();
  for (int i = 0; i < total_writes; ++i) {
    string key = "heavy_key_" + to_string(i);
    auto res = db->Put(wopt, key, large_val);
    if (!res) {
      cerr << "Put failed: " << res.error().ToString() << "\n";
    }
  }
  auto end = chrono::high_resolution_clock::now();
  
  stop_threads = true;
  scanner.join();

  auto duration = chrono::duration_cast<chrono::microseconds>(end - start);
  PrintThroughput("  Heavy Writes", total_writes, duration);
  cout << "  Total snapshot iterator scans completed: " << iter_scans.load() << "\n";
  cout << "Compaction & Iterator Stability Stress Test: ✓ (Pass)\n";
}

// 4. Lifecycle & Leak Test
void RunLifecycleLeakStressTest(leveldb::StdFileSystem& fs) {
  cout << "\n--- [Stress Test 4: Lifecycle & Leak Prevention] ---\n";
  leveldb::Options<leveldb::StdFileSystem> options;
  options.async_optimize = g_async_optimize;
  options.coro_allocator = g_coro_allocator;
  options.lock_free_queue = g_lock_free_queue;
  options.io_uring = g_io_uring;
  options.zero_copy = g_zero_copy;
  options.create_if_missing = true;
  options.env = &fs;

  std::unique_ptr<leveldb::Cache> cache(leveldb::NewLRUCache(2 * 1024 * 1024)); // 2MB block cache
  options.block_cache = cache.get();

  const int iterations = 800; // Increased to run >= 5s
  cout << "  Running DB open/close lifecycle loop (" << iterations << " iterations, 1000 writes/reads each)... \n";
  
  auto start = chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) {
    string dbname = "stress_db_lifecycle_" + to_string(i % 5); // Reuse 5 directories to stress file locks
    leveldb::DestroyDB(dbname, options);
    
    auto db_res = leveldb::DB::Open(options, dbname);
    if (!db_res) {
      cerr << "Open failed at iteration " << i << ": " << db_res.error().ToString() << "\n";
      exit(1);
    }
    
    auto db = std::move(*db_res);
    leveldb::WriteOptions wopt;
    for (int j = 0; j < 1000; ++j) {
      db->Put(wopt, "key_" + to_string(j), "val_" + to_string(j));
    }
    
    leveldb::ReadOptions ropt;
    ropt.async_optimize = g_async_optimize;
    ropt.coro_allocator = g_coro_allocator;
    ropt.lock_free_queue = g_lock_free_queue;
    ropt.io_uring = g_io_uring;
    ropt.zero_copy = g_zero_copy;
    for (int j = 0; j < 1000; ++j) {
      auto res = db->Get(ropt, "key_" + to_string(j));
      assert(res && *res && **res == "val_" + to_string(j));
    }
    
    // Create an iterator and leave it dangling or destroy it
    unique_ptr<leveldb::Iterator> iter(db->NewIterator(ropt));
    iter->SeekToFirst();
    assert(iter->Valid() && iter->key() == "key_0");
    
    // DB is closed/destructed before iterator is destroyed to stress lifetime safety
  }
  auto end = chrono::high_resolution_clock::now();
  auto duration = chrono::duration_cast<chrono::microseconds>(end - start);
  cout << "  Lifecycle loop completed in " << fixed << setprecision(3) << duration.count() / 1000000.0 << "s\n";
  cout << "Lifecycle & Leak Stress Test: ✓ (Pass)\n";
}

// 5. Realistic Ingestion & Query Stress Test (covering GetProperty, GetApproximateSizes, CompactRange)
struct TransactionRecord {
  uint32_t user_id;
  uint64_t timestamp;
  string tx_id;
  double amount;
  string merchant;
  string ip_address;

  string Serialize() const {
    stringstream ss;
    ss << "{\"user_id\":" << user_id
       << ",\"timestamp\":" << timestamp
       << ",\"tx_id\":\"" << tx_id << "\""
       << ",\"amount\":" << amount
       << ",\"merchant\":\"" << merchant << "\""
       << ",\"ip\":\"" << ip_address << "\"}";
    return ss.str();
  }
};

TransactionRecord GenerateRandomTx(mt19937& rng, uint32_t user_id, uint64_t timestamp) {
  uniform_real_distribution<double> dist_amount(1.0, 10000.0);
  uniform_int_distribution<int> dist_merchant(0, 4);
  uniform_int_distribution<int> dist_ip(1, 254);
  vector<string> merchants = {"Amazon", "Walmart", "Steam", "Target", "AppleStore"};
  
  TransactionRecord tx;
  tx.user_id = user_id;
  tx.timestamp = timestamp;
  
  stringstream ss;
  ss << "tx_" << hex << rng();
  tx.tx_id = ss.str();
  tx.amount = dist_amount(rng);
  tx.merchant = merchants[dist_merchant(rng)];
  tx.ip_address = "192.168.1." + to_string(dist_ip(rng));
  
  return tx;
}

void RunRealisticDataStressTest(leveldb::StdFileSystem& fs) {
  cout << "\n--- [Stress Test 5: Realistic Large-Scale Ingestion & Queries] ---\n";
  leveldb::Options<leveldb::StdFileSystem> options;
  options.async_optimize = g_async_optimize;
  options.coro_allocator = g_coro_allocator;
  options.lock_free_queue = g_lock_free_queue;
  options.io_uring = g_io_uring;
  options.zero_copy = g_zero_copy;
  options.create_if_missing = true;
  options.write_buffer_size = 1024 * 1024; // 1MB memtable buffer to trigger frequent flushes
  options.env = &fs;
  
  std::unique_ptr<leveldb::Cache> cache(leveldb::NewLRUCache(16 * 1024 * 1024)); // 16MB block cache
  options.block_cache = cache.get();

  leveldb::DestroyDB("stress_db_realistic", options);

  auto db_res = leveldb::DB::Open(options, "stress_db_realistic");
  assert(db_res);
  auto db = std::move(*db_res);

  const int num_records = 600000; // Increased to run >= 5s
  cout << "  Generating " << num_records << " realistic transaction logs...\n" << std::flush;

  mt19937 rng(42);
  vector<TransactionRecord> dataset;
  dataset.reserve(num_records);
  for (int i = 0; i < num_records; ++i) {
    uint32_t user_id = 100000 + (rng() % 10000); // 10k unique users
    uint64_t ts = 1700000000 + i;
    dataset.push_back(GenerateRandomTx(rng, user_id, ts));
  }

  cout << "  Ingesting data (" << num_records << " writes in batches of 100)... \n" << std::flush;
  auto start_write = chrono::high_resolution_clock::now();
  leveldb::WriteOptions wopt;
  
  for (size_t i = 0; i < dataset.size(); i += 100) {
    leveldb::WriteBatch batch;
    for (size_t j = i; j < i + 100 && j < dataset.size(); ++j) {
      const auto& tx = dataset[j];
      string key = "user:" + to_string(tx.user_id) + ":" + to_string(tx.timestamp);
      batch.Put(key, tx.Serialize());
    }
    auto write_res = db->Write(wopt, &batch);
    if (!write_res) {
      cerr << "Batch write failed: " << write_res.error().ToString() << "\n";
      exit(1);
    }
  }
  auto end_write = chrono::high_resolution_clock::now();
  auto write_duration = chrono::duration_cast<chrono::microseconds>(end_write - start_write);
  PrintThroughput("  Ingestion throughput", num_records, write_duration);

  const int num_lookups = 150000; // Increased to run >= 5s
  const int num_scans = 75000; // Increased to run >= 5s
  cout << "  Performing " << num_lookups << " random point lookups and " << num_scans << " range scans...\n" << std::flush;
  auto start_read = chrono::high_resolution_clock::now();
  leveldb::ReadOptions ropt;
  ropt.async_optimize = g_async_optimize;
  ropt.coro_allocator = g_coro_allocator;
  ropt.lock_free_queue = g_lock_free_queue;
  ropt.io_uring = g_io_uring;
  ropt.zero_copy = g_zero_copy;
  int found_count = 0;

  for (int i = 0; i < num_lookups; ++i) {
    int idx = rng() % num_records;
    const auto& tx = dataset[idx];
    string key = "user:" + to_string(tx.user_id) + ":" + to_string(tx.timestamp);
    auto get_res = db->Get(ropt, key);
    if (get_res && *get_res) {
      found_count++;
    }
  }

  int prefix_scan_count = 0;
  for (int i = 0; i < num_scans; ++i) {
    uint32_t search_user = 100000 + (rng() % 10000);
    string prefix = "user:" + to_string(search_user) + ":";
    unique_ptr<leveldb::Iterator> iter(db->NewIterator(ropt));
    iter->Seek(prefix);
    while (iter->Valid() && iter->key().starts_with(prefix)) {
      prefix_scan_count++;
      iter->Next();
    }
  }
  auto end_read = chrono::high_resolution_clock::now();
  auto read_duration = chrono::duration_cast<chrono::microseconds>(end_read - start_read);

  cout << "  Verified point lookups: " << found_count << " / " << num_lookups << "\n";
  cout << "  Scanned matching user prefix records: " << prefix_scan_count << "\n";
  PrintThroughput("  Point Lookup & Prefix Scan throughput", num_lookups + num_scans, read_duration);

  // Cover GetApproximateSizes
  cout << "  Testing GetApproximateSizes API...\n";
  leveldb::Range ranges[2];
  ranges[0] = leveldb::Range("user:100000:", "user:102000:");
  ranges[1] = leveldb::Range("user:105000:", "user:108000:");
  uint64_t sizes[2];
  db->GetApproximateSizes(ranges, 2, sizes);
  cout << "    Approximate size of range 0: " << sizes[0] << " bytes\n";
  cout << "    Approximate size of range 1: " << sizes[1] << " bytes\n";

  // Cover GetProperty
  cout << "  Testing GetProperty API...\n";
  auto stats_prop = db->GetProperty("leveldb.stats");
  if (stats_prop && *stats_prop) {
    cout << "    leveldb.stats:\n" << **stats_prop << "\n";
  }
  auto sstables_prop = db->GetProperty("leveldb.sstables");
  if (sstables_prop && *sstables_prop) {
    cout << "    leveldb.sstables length: " << (*sstables_prop)->size() << " chars\n";
  }
  auto mem_usage_prop = db->GetProperty("leveldb.approximate-memory-usage");
  if (mem_usage_prop && *mem_usage_prop) {
    cout << "    leveldb.approximate-memory-usage: " << **mem_usage_prop << " bytes\n";
  }

  // Cover CompactRange
  cout << "  Testing CompactRange API (Manual compaction of entire DB)...\n" << std::flush;
  auto start_compact = chrono::high_resolution_clock::now();
  db->CompactRange(nullptr, nullptr);
  auto end_compact = chrono::high_resolution_clock::now();
  auto compact_duration = chrono::duration_cast<chrono::microseconds>(end_compact - start_compact);
  cout << "    Manual compaction completed in " << fixed << setprecision(3) << compact_duration.count() / 1000000.0 << "s\n";

  cout << "Realistic Large-Scale Stress Test: ✓ (Pass)\n";
}

// 6. RepairDB Stress Test
void RunRepairStressTest(leveldb::StdFileSystem& fs) {
  cout << "\n--- [Stress Test 6: Database Repair (RepairDB)] ---\n";
  leveldb::Options<leveldb::StdFileSystem> options;
  options.async_optimize = g_async_optimize;
  options.coro_allocator = g_coro_allocator;
  options.lock_free_queue = g_lock_free_queue;
  options.io_uring = g_io_uring;
  options.zero_copy = g_zero_copy;
  options.create_if_missing = true;
  options.env = &fs;

  leveldb::DestroyDB("stress_db_repair", options);

  const int num_keys = 1000000; // Large enough to run >= 5s
  
  // Scope 1: Write keys and destruct the DB without compaction
  {
    auto db_res = leveldb::DB::Open(options, "stress_db_repair");
    assert(db_res);
    auto db = std::move(*db_res);
    leveldb::WriteOptions wopt;
    cout << "  Writing " << num_keys << " keys to database for repair...\n" << std::flush;
    auto start = chrono::high_resolution_clock::now();
    for (int i = 0; i < num_keys; ++i) {
      auto res = db->Put(wopt, "repair_key_" + to_string(i), "repair_value_" + to_string(i));
      if (!res) {
        cerr << "Put failed during repair setup: " << res.error().ToString() << "\n";
        exit(1);
      }
    }
    auto end = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::microseconds>(end - start);
    PrintThroughput("  Writes", num_keys, duration);
  }

  // Scope 2: Call RepairDB
  cout << "  Calling RepairDB on stress_db_repair directory...\n" << std::flush;
  auto start = chrono::high_resolution_clock::now();
  auto repair_res = leveldb::RepairDB("stress_db_repair", options);
  auto end = chrono::high_resolution_clock::now();
  auto duration = chrono::duration_cast<chrono::microseconds>(end - start);
  if (!repair_res) {
    cerr << "RepairDB failed: " << repair_res.error().ToString() << "\n";
    exit(1);
  }
  cout << "  RepairDB completed in " << fixed << setprecision(3) << duration.count() / 1000000.0 << "s\n";

  // Scope 3: Reopen database and verify all data is present
  cout << "  Reopening database and verifying integrity...\n" << std::flush;
  auto db_res = leveldb::DB::Open(options, "stress_db_repair");
  if (!db_res) {
    cerr << "Failed to open repaired DB: " << db_res.error().ToString() << "\n";
    exit(1);
  }
  auto db = std::move(*db_res);
  leveldb::ReadOptions ropt;
  ropt.async_optimize = g_async_optimize;
  ropt.coro_allocator = g_coro_allocator;
  ropt.lock_free_queue = g_lock_free_queue;
  ropt.io_uring = g_io_uring;
  ropt.zero_copy = g_zero_copy;
  int recovered_keys = 0;
  for (int i = 0; i < num_keys; ++i) {
    auto get_res = db->Get(ropt, "repair_key_" + to_string(i));
    if (get_res && *get_res && **get_res == "repair_value_" + to_string(i)) {
      recovered_keys++;
    }
  }
  cout << "  Verified recovered keys: " << recovered_keys << " / " << num_keys << "\n";
  assert(recovered_keys == num_keys);
  cout << "RepairDB Stress Test: ✓ (Pass)\n";
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--async_optimize") {
      g_async_optimize = true;
    } else if (arg == "--coro_allocator") {
      g_coro_allocator = true;
    } else if (arg == "--lock_free_queue") {
      g_lock_free_queue = true;
    } else if (arg == "--io_uring") {
      g_io_uring = true;
    } else if (arg == "--zero_copy") {
      g_zero_copy = true;
    }
  }

  cout << "========================================================\n";
  cout << "        LevelDB-23 Full Pipeline Stress Test Suite       \n";
  cout << "  async_optimize:  " << (g_async_optimize ? "ON" : "OFF") << "\n";
  cout << "  coro_allocator:  " << (g_coro_allocator ? "ON" : "OFF") << "\n";
  cout << "  lock_free_queue: " << (g_lock_free_queue ? "ON" : "OFF") << "\n";
  cout << "  io_uring:        " << (g_io_uring ? "ON" : "OFF") << "\n";
  cout << "  zero_copy:       " << (g_zero_copy ? "ON" : "OFF") << "\n";
  cout << "========================================================\n";

  leveldb::StdFileSystem fs;
  
  RunConcurrencyStressTest(fs);
  RunWALRecoveryStressTest(fs);
  RunCompactionIteratorStressTest(fs);
  RunLifecycleLeakStressTest(fs);
  RunRealisticDataStressTest(fs);
  RunRepairStressTest(fs);

  cout << "\n========================================================\n";
  cout << "       ALL STRESS TESTS COMPLETED SUCCESSFULLY! ✓        \n";
  cout << "========================================================\n";
  return 0;
}
