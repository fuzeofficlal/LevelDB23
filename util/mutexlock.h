//
// Modernized for C++23.
//
// Original: custom MutexLock RAII class wrapping port::Mutex.
// New: type alias for std::scoped_lock<std::mutex>.
//
// NOTE: call-site syntax changes from MutexLock l(&mu_) to MutexLock l(mu_)
// because std::scoped_lock takes references, not pointers.

#pragma once

#include <mutex>

namespace leveldb {

// MutexLock is now a direct alias for std::scoped_lock<std::mutex>.
// This eliminates the port::Mutex dependency while preserving the type name
// used throughout the codebase.
using MutexLock = std::scoped_lock<std::mutex>;

}  // namespace leveldb
