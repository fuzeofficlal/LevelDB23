#pragma once

#include <cstdint>

#include "db/dbformat.h"
#include "leveldb/db.h"

#include <memory>

namespace leveldb {

class DBImpl;

// Return a new iterator that converts internal keys (yielded by
// "*internal_iter") that were live at the specified "sequence" number
// into appropriate user keys.
std::unique_ptr<Iterator> NewDBIterator(DBImpl* db, const Comparator* user_key_comparator,
                        std::unique_ptr<Iterator> internal_iter, SequenceNumber sequence,
                        uint32_t seed);

}  // namespace leveldb
