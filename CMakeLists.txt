cmake_minimum_required(VERSION 3.20)
project(LevelDB_23 C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(leveldb
        util/coding.cc
        util/hash.cc
        util/crc32c.cc
        util/arena.cc
        util/logging.cc
        util/comparator.cc
        util/bloom.cc
        util/cache.cc
        db/dbformat.cc
        db/memtable.cc
        db/version_edit.cc
        db/version_set.cc
        db/builder.cc
        db/filename.cc
        db/table_cache.cc
        db/write_batch.cc
        db/db_impl.cc
        db/repair.cc
        db/c.cc
        db/dumpfile.cc
        include/leveldb/status.h
        include/leveldb/export.h
        util/coding.h
        util/hash.h
        util/crc32c.h
        util/arena.h
        util/random.h
        util/mutexlock.h
        util/no_destructor.h
        util/logging.h
        util/compression.h
        include/leveldb/env.h
        include/leveldb/options.h
        include/leveldb/std_file_system.h
        include/leveldb/comparator.h
        include/leveldb/filter_policy.h
        include/leveldb/iterator.h
        include/leveldb/cache.h
        include/leveldb/write_batch.h
        db/dbformat.h
        db/skiplist.h
        db/memtable.h
        db/log_format.h
        db/log_writer.h
        db/log_reader.h
        db/co_task.h
        db/async_executor.h
        table/format.cc
        table/block.cc
        table/block_builder.cc
        table/filter_block.cc
        table/iterator.cc
        table/two_level_iterator.cc
        table/merger.cc
        db/db_iter.cc
        util/std_file_system.cc)

target_include_directories(leveldb PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR})

if (WIN32)
    # Windows doesn't need -pthread flag usually, but MSVC might need different flags if not using clang.
    # GCC/MinGW needs it.
    target_link_libraries(leveldb pthread)
endif()

add_executable(LevelDB_23 main.cpp)
target_link_libraries(LevelDB_23 leveldb)

add_executable(leveldbutil db/leveldbutil.cc)
target_link_libraries(leveldbutil leveldb)

add_executable(c_test db/c_test.c)
target_link_libraries(c_test leveldb)

add_executable(db_stress_test db/db_stress_test.cpp)
target_link_libraries(db_stress_test leveldb)
