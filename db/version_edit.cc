//
// Modernized for C++23.

#include "db/version_edit.h"
#include "util/coding.h"

namespace leveldb {

enum Tag : uint32_t {
  kComparator = 1,
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kCompactPointer = 5,
  kDeletedFile = 6,
  kNewFile = 7,
  // 8 was used for large value refs
  kPrevLogNumber = 9
};

void VersionEdit::Clear() {
  comparator_.reset();
  log_number_.reset();
  prev_log_number_.reset();
  last_sequence_.reset();
  next_file_number_.reset();
  compact_pointers_.clear();
  deleted_files_.clear();
  new_files_.clear();
}

void VersionEdit::EncodeTo(std::string& dst) const {
  if (comparator_) {
    PutVarint32(dst, kComparator);
    PutLengthPrefixedSlice(dst, *comparator_);
  }
  if (log_number_) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, *log_number_);
  }
  if (prev_log_number_) {
    PutVarint32(dst, kPrevLogNumber);
    PutVarint64(dst, *prev_log_number_);
  }
  if (next_file_number_) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, *next_file_number_);
  }
  if (last_sequence_) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, *last_sequence_);
  }

  for (const auto& [level, key] : compact_pointers_) {
    PutVarint32(dst, kCompactPointer);
    PutVarint32(dst, level);
    PutLengthPrefixedSlice(dst, key.Encode());
  }

  for (const auto& [level, file] : deleted_files_) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, level);
    PutVarint64(dst, file);
  }

  for (const auto& [level, f] : new_files_) {
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, level);
    PutVarint64(dst, f->number);
    PutVarint64(dst, f->file_size);
    PutLengthPrefixedSlice(dst, f->smallest.Encode());
    PutLengthPrefixedSlice(dst, f->largest.Encode());
  }
}

static Result<InternalKey> GetInternalKey(std::string_view& input) {
  auto str = GetLengthPrefixedSlice(input);
  if (str) {
    InternalKey key;
    if (key.DecodeFrom(*str)) {
      return key;
    }
  }
  return std::unexpected(Status::CorruptionErr("VersionEdit: bad InternalKey"));
}

static Result<uint32_t> GetLevel(std::string_view& input) {
  auto v = GetVarint32(input);
  if (v && *v < config::kNumLevels) {
    return *v;
  }
  return std::unexpected(Status::CorruptionErr("VersionEdit: bad level"));
}

Result<void> VersionEdit::DecodeFrom(std::string_view src) {
  Clear();
  std::string_view input = src;

  int level;
  FileMetaData f;

  while (auto tag_opt = GetVarint32(input)) {
    uint32_t tag = *tag_opt;
    switch (tag) {
      case kComparator: {
        auto str = GetLengthPrefixedSlice(input);
        if (str) {
          comparator_ = std::string(*str);
        } else {
          return std::unexpected(Status::CorruptionErr("VersionEdit: comparator name"));
        }
        break;
      }

      case kLogNumber: {
        auto num = GetVarint64(input);
        if (num) {
          log_number_ = *num;
        } else {
          return std::unexpected(Status::CorruptionErr("VersionEdit: log number"));
        }
        break;
      }

      case kPrevLogNumber: {
        auto num = GetVarint64(input);
        if (num) {
          prev_log_number_ = *num;
        } else {
          return std::unexpected(Status::CorruptionErr("VersionEdit: previous log number"));
        }
        break;
      }

      case kNextFileNumber: {
        auto num = GetVarint64(input);
        if (num) {
          next_file_number_ = *num;
        } else {
          return std::unexpected(Status::CorruptionErr("VersionEdit: next file number"));
        }
        break;
      }

      case kLastSequence: {
        auto num = GetVarint64(input);
        if (num) {
          last_sequence_ = *num;
        } else {
          return std::unexpected(Status::CorruptionErr("VersionEdit: last sequence number"));
        }
        break;
      }

      case kCompactPointer: {
        auto lvl_res = GetLevel(input);
        if (!lvl_res) return std::unexpected(lvl_res.error());
        level = *lvl_res;

        auto key_res = GetInternalKey(input);
        if (!key_res) return std::unexpected(key_res.error());
        compact_pointers_.emplace_back(level, *key_res);
        break;
      }

      case kDeletedFile: {
        auto lvl_res = GetLevel(input);
        if (!lvl_res) return std::unexpected(lvl_res.error());
        level = *lvl_res;

        auto num = GetVarint64(input);
        if (num) {
          deleted_files_.emplace(level, *num);
        } else {
          return std::unexpected(Status::CorruptionErr("VersionEdit: deleted file"));
        }
        break;
      }

      case kNewFile: {
        auto lvl_res = GetLevel(input);
        if (!lvl_res) return std::unexpected(lvl_res.error());
        level = *lvl_res;

        auto num = GetVarint64(input);
        auto size = GetVarint64(input);
        if (num && size) {
          f.number = *num;
          f.file_size = *size;
          
          auto smallest_res = GetInternalKey(input);
          if (!smallest_res) return std::unexpected(smallest_res.error());
          f.smallest = *smallest_res;

          auto largest_res = GetInternalKey(input);
          if (!largest_res) return std::unexpected(largest_res.error());
          f.largest = *largest_res;

          new_files_.emplace_back(level, std::make_shared<FileMetaData>(f));
        } else {
          return std::unexpected(Status::CorruptionErr("VersionEdit: new-file entry"));
        }
        break;
      }

      default:
        // Ignore unknown tags for forward compatibility
        if (tag == 8) { // Legacy large value refs
            return std::unexpected(Status::CorruptionErr("VersionEdit: unknown tag 8"));
        }
        return std::unexpected(Status::CorruptionErr("VersionEdit: unknown tag"));
    }
  }

  if (!input.empty()) {
    return std::unexpected(Status::CorruptionErr("VersionEdit: trailing garbage"));
  }

  return {};
}

// Minimal implementation of AppendNumberTo
static void AppendNumberTo(std::string* str, uint64_t num) {
  str->append(std::to_string(num));
}

std::string VersionEdit::DebugString() const {
  std::string r;
  r.append("VersionEdit {");
  if (comparator_) {
    r.append("\n  Comparator: ");
    r.append(*comparator_);
  }
  if (log_number_) {
    r.append("\n  LogNumber: ");
    AppendNumberTo(&r, *log_number_);
  }
  if (prev_log_number_) {
    r.append("\n  PrevLogNumber: ");
    AppendNumberTo(&r, *prev_log_number_);
  }
  if (next_file_number_) {
    r.append("\n  NextFile: ");
    AppendNumberTo(&r, *next_file_number_);
  }
  if (last_sequence_) {
    r.append("\n  LastSeq: ");
    AppendNumberTo(&r, *last_sequence_);
  }
  for (const auto& [level, key] : compact_pointers_) {
    r.append("\n  CompactPointer: ");
    AppendNumberTo(&r, level);
    r.append(" ");
    r.append(key.DebugString());
  }
  for (const auto& [level, file] : deleted_files_) {
    r.append("\n  DeletedFile: ");
    AppendNumberTo(&r, level);
    r.append(" ");
    AppendNumberTo(&r, file);
  }
  for (const auto& [level, f] : new_files_) {
    r.append("\n  AddedFile: ");
    AppendNumberTo(&r, level);
    r.append(" ");
    AppendNumberTo(&r, f->number);
    r.append(" ");
    AppendNumberTo(&r, f->file_size);
    r.append(" ");
    r.append(f->smallest.DebugString());
    r.append(" .. ");
    r.append(f->largest.DebugString());
  }
  r.append("\n}\n");
  return r;
}

}  // namespace leveldb
