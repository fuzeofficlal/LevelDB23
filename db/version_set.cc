//
// Modernized for C++23.

#include "db/version_set.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "db/log_reader.h"
#include "table/two_level_iterator.h"
#include "table/merger.h"
#include "util/coding.h"
#include <algorithm>

namespace leveldb {

int FindFile(const InternalKeyComparator& icmp,
             const std::vector<std::shared_ptr<FileMetaData>>& files,
             std::string_view key) {
  uint32_t left = 0;
  uint32_t right = files.size();
  while (left < right) {
    uint32_t mid = (left + right) / 2;
    const auto& f = files[mid];
    if (icmp.Compare(f->largest.Encode(), key) < 0) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  return right;
}

bool SomeFileOverlapsRange(const InternalKeyComparator& icmp,
                           bool disjoint_sorted_files,
                           const std::vector<std::shared_ptr<FileMetaData>>& files,
                           const std::string_view* smallest_user_key,
                           const std::string_view* largest_user_key) {
  const Comparator* ucmp = icmp.user_comparator();
  if (!disjoint_sorted_files) {
    for (size_t i = 0; i < files.size(); i++) {
      const auto& f = files[i];
      if (largest_user_key != nullptr &&
          ucmp->Compare(f->smallest.user_key(), *largest_user_key) > 0) {
        continue;
      }
      if (smallest_user_key != nullptr &&
          ucmp->Compare(f->largest.user_key(), *smallest_user_key) < 0) {
        continue;
      }
      return true;
    }
    return false;
  }

  uint32_t index = 0;
  if (smallest_user_key != nullptr) {
    InternalKey small_key(*smallest_user_key, kMaxSequenceNumber,
                          kValueTypeForSeek);
    index = FindFile(icmp, files, small_key.Encode());
  }

  if (index >= files.size()) {
    return false;
  }

  if (largest_user_key != nullptr &&
      ucmp->Compare(files[index]->smallest.user_key(), *largest_user_key) > 0) {
    return false;
  }
  return true;
}

Version::Version(VersionSet* vset)
    : vset_(vset), next_(this), prev_(this) {}

Version::~Version() {
  if (prev_ != nullptr) prev_->next_ = next_;
  if (next_ != nullptr) next_->prev_ = prev_;
}

// Stubs for remaining complex methods
class Version::LevelFileNumIterator : public Iterator {
 public:
  LevelFileNumIterator(const InternalKeyComparator& icmp,
                       const std::vector<std::shared_ptr<FileMetaData>>* flist)
      : icmp_(icmp), flist_(flist), index_(flist->size()) {}

  bool Valid() const noexcept override { return index_ < flist_->size(); }
  void Seek(std::string_view target) override {
    index_ = FindFile(icmp_, *flist_, target);
  }
  void SeekToFirst() override { index_ = 0; }
  void SeekToLast() override {
    index_ = flist_->empty() ? 0 : flist_->size() - 1;
  }
  void Next() override {
    assert(Valid());
    index_++;
  }
  void Prev() override {
    assert(Valid());
    if (index_ == 0) {
      index_ = flist_->size();
    } else {
      index_--;
    }
  }
  std::string_view key() const override {
    assert(Valid());
    return (*flist_)[index_]->largest.Encode();
  }
  std::string_view value() const override {
    assert(Valid());
    auto& file = (*flist_)[index_];
    value_buf_.clear();
    PutFixed64(value_buf_, file->number);
    PutFixed64(value_buf_, file->file_size);
    return value_buf_;
  }
  Result<void> status() const override { return {}; }

 private:
  const InternalKeyComparator icmp_;
  const std::vector<std::shared_ptr<FileMetaData>>* const flist_;
  uint32_t index_;
  mutable std::string value_buf_;
};

void Version::AddIterators(const ReadOptions& options, std::vector<std::unique_ptr<Iterator>>& iters) {
  for (size_t i = 0; i < files_[0].size(); i++) {
    iters.push_back(
        vset_->table_cache_->NewIterator(options, files_[0][i]->number, files_[0][i]->file_size));
  }

  for (int level = 1; level < config::kNumLevels; level++) {
    if (!files_[level].empty()) {
      iters.push_back(NewConcatenatingIterator(options, level));
    }
  }
}

static bool NewestFirst(const std::shared_ptr<FileMetaData>& a, const std::shared_ptr<FileMetaData>& b) {
  return a->number > b->number;
}

template <typename Func>
void Version::ForEachOverlapping(std::string_view user_key, std::string_view internal_key, Func func) {
  const Comparator* ucmp = vset_->icmp_.user_comparator();

  std::vector<std::shared_ptr<FileMetaData>> tmp;
  tmp.reserve(files_[0].size());
  for (uint32_t i = 0; i < files_[0].size(); i++) {
    auto f = files_[0][i];
    if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
        ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
      tmp.push_back(f);
    }
  }
  if (!tmp.empty()) {
    std::sort(tmp.begin(), tmp.end(), NewestFirst);
    for (uint32_t i = 0; i < tmp.size(); i++) {
      if (!func(0, tmp[i])) return;
    }
  }

  for (int level = 1; level < config::kNumLevels; level++) {
    size_t num_files = files_[level].size();
    if (num_files == 0) continue;

    uint32_t index = FindFile(vset_->icmp_, files_[level], internal_key);
    if (index < num_files) {
      auto f = files_[level][index];
      if (ucmp->Compare(user_key, f->smallest.user_key()) < 0) {
        // All of "f" is past any data for user_key
      } else {
        if (!func(level, f)) return;
      }
    }
  }
}

Result<std::optional<std::string>> Version::Get(const ReadOptions& options,
                                                const LookupKey& k, GetStats* stats) {
  stats->seek_file = nullptr;
  stats->seek_file_level = -1;

  std::optional<std::string> result;
  Status s;
  bool found = false;

  std::shared_ptr<FileMetaData> last_file_read;
  int last_file_read_level = -1;

  auto match_func = [&](int level, std::shared_ptr<FileMetaData> f) -> bool {
    if (stats->seek_file == nullptr && last_file_read != nullptr) {
      stats->seek_file = last_file_read;
      stats->seek_file_level = last_file_read_level;
    }

    last_file_read = f;
    last_file_read_level = level;

    bool file_found = false;
    auto get_res = vset_->table_cache_->Get(
        options, f->number, f->file_size, k.internal_key(),
        [&](std::string_view ikey, std::string_view v) {
          auto opt_key = ParseInternalKey(ikey);
          if (!opt_key) {
            s = Status::Corruption("corrupted key for " + std::string(k.user_key()));
            found = true;
            file_found = true;
          } else {
            ParsedInternalKey parsed_key = *opt_key;
            if (vset_->icmp_.user_comparator()->Compare(parsed_key.user_key, k.user_key()) == 0) {
              if (parsed_key.type == kTypeValue) {
                result = std::string(v);
              } else {
                result = std::nullopt; // Deleted
              }
              found = true;
              file_found = true;
            }
          }
        });

    if (!get_res) {
      s = get_res.error();
      found = true;
      return false; // Stop iteration
    }

    return !found; // Continue if not found
  };

  ForEachOverlapping(k.user_key(), k.internal_key(), match_func);

  if (!s.ok()) {
    return std::unexpected(s);
  }
  
  if (found) {
    if (result) {
      return *result;
    } else {
      return std::unexpected(Status::NotFoundErr("deleted"));
    }
  }
  
  return std::unexpected(Status::NotFoundErr("not found"));
}

bool Version::UpdateStats(const GetStats& stats) {
  if (stats.seek_file != nullptr) {
    stats.seek_file->allowed_seeks--;
    if (stats.seek_file->allowed_seeks <= 0 && file_to_compact_ == nullptr) {
      file_to_compact_ = stats.seek_file;
      file_to_compact_level_ = stats.seek_file_level;
      return true;
    }
  }
  return false;
}

bool Version::RecordReadSample(std::string_view internal_key) {
  auto opt_key = ParseInternalKey(internal_key);
  if (!opt_key) {
    return false;
  }
  ParsedInternalKey ikey = *opt_key;

  GetStats stats;
  int matches = 0;

  auto match_func = [&](int level, std::shared_ptr<FileMetaData> f) -> bool {
    matches++;
    if (matches == 1) {
      stats.seek_file = f;
      stats.seek_file_level = level;
    }
    return matches < 2;
  };

  ForEachOverlapping(ikey.user_key, internal_key, match_func);

  if (matches >= 2) {
    return UpdateStats(stats);
  }
  return false;
}

void Version::GetOverlappingInputs(int level, const InternalKey* begin,
                                   const InternalKey* end,
                                   std::vector<std::shared_ptr<FileMetaData>>& inputs) {
  inputs.clear();
  std::string_view user_begin, user_end;
  if (begin != nullptr) user_begin = begin->user_key();
  if (end != nullptr) user_end = end->user_key();
  const Comparator* user_cmp = vset_->icmp_.user_comparator();

  for (size_t i = 0; i < files_[level].size();) {
    auto f = files_[level][i++];
    const std::string_view file_start = f->smallest.user_key();
    const std::string_view file_limit = f->largest.user_key();
    if (begin != nullptr && user_cmp->Compare(file_limit, user_begin) < 0) {
      // "f" is completely before specified range; skip it
    } else if (end != nullptr && user_cmp->Compare(file_start, user_end) > 0) {
      // "f" is completely after specified range; skip it
    } else {
      inputs.push_back(f);
      if (level == 0) {
        if (begin != nullptr && user_cmp->Compare(file_start, user_begin) < 0) {
          user_begin = file_start;
          inputs.clear();
          i = 0;
        } else if (end != nullptr && user_cmp->Compare(file_limit, user_end) > 0) {
          user_end = file_limit;
          inputs.clear();
          i = 0;
        }
      }
    }
  }
}

bool Version::OverlapInLevel(int level, const std::string_view* smallest_user_key,
                             const std::string_view* largest_user_key) {
  return SomeFileOverlapsRange(vset_->icmp_, (level > 0), files_[level],
                               smallest_user_key, largest_user_key);
}

static double MaxBytesForLevel(const Options<StdFileSystem>* options, int level) {
  double result = 10. * 1048576.0;
  while (level > 1) {
    result *= 10;
    level--;
  }
  return result;
}

static size_t TargetFileSize(const Options<StdFileSystem>* options) {
  return options->max_file_size;
}

static int64_t MaxGrandParentOverlapBytes(const Options<StdFileSystem>* options) {
  return 10 * TargetFileSize(options);
}

static uint64_t MaxFileSizeForLevel(const Options<StdFileSystem>* options, int level) {
  return TargetFileSize(options);
}

static int64_t ExpandedCompactionByteSizeLimit(const Options<StdFileSystem>* options) {
  return 25 * TargetFileSize(options);
}

static int64_t TotalFileSize(const std::vector<std::shared_ptr<FileMetaData>>& files) {
  int64_t sum = 0;
  for (size_t i = 0; i < files.size(); i++) {
    sum += files[i]->file_size;
  }
  return sum;
}

int Version::PickLevelForMemTableOutput(std::string_view smallest_user_key,
                                        std::string_view largest_user_key) {
  int level = 0;
  if (!OverlapInLevel(0, &smallest_user_key, &largest_user_key)) {
    InternalKey start(smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey limit(largest_user_key, 0, static_cast<ValueType>(0));
    std::vector<std::shared_ptr<FileMetaData>> overlaps;
    while (level < config::kMaxMemCompactLevel) {
      if (OverlapInLevel(level + 1, &smallest_user_key, &largest_user_key)) {
        break;
      }
      if (level + 2 < config::kNumLevels) {
        GetOverlappingInputs(level + 2, &start, &limit, overlaps);
        const int64_t sum = TotalFileSize(overlaps);
        if (sum > MaxGrandParentOverlapBytes(vset_->options_)) {
          break;
        }
      }
      level++;
    }
  }
  return level;
}
std::string Version::DebugString() const {
  std::string r;
  for (int level = 0; level < config::kNumLevels; level++) {
    r.append("--- level ");
    r.append(std::to_string(level));
    r.append(" ---\n");
    const auto& files = files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      r.push_back(' ');
      r.append(std::to_string(files[i]->number));
      r.push_back(':');
      r.append(std::to_string(files[i]->file_size));
      r.append("[");
      r.append(files[i]->smallest.DebugString());
      r.append(" .. ");
      r.append(files[i]->largest.DebugString());
      r.append("]\n");
    }
  }
  return r;
}
std::unique_ptr<Iterator> Version::NewConcatenatingIterator(const ReadOptions& options, int level) const {
  auto index_iter = std::make_unique<LevelFileNumIterator>(vset_->icmp_, &files_[level]);
  return NewTwoLevelIterator(
      std::move(index_iter),
      [vset = vset_](const ReadOptions& options, std::string_view index_value) -> std::unique_ptr<Iterator> {
        if (index_value.size() != 16) {
          return std::unique_ptr<Iterator>(NewErrorIterator(std::unexpected(Status::Corruption("FileReader invoked with unexpected value"))));
        }
        uint64_t file_number = DecodeFixed64(index_value.data());
        uint64_t file_size = DecodeFixed64(index_value.data() + 8);
        return vset->table_cache_->NewIterator(options, file_number, file_size);
      },
      options);
}

VersionSet::VersionSet(std::string dbname, const Options<StdFileSystem>* options,
                       TableCache* table_cache, const InternalKeyComparator* cmp)
    : env_(options->env),
      dbname_(std::move(dbname)),
      options_(options),
      table_cache_(table_cache),
      icmp_(*cmp),
      dummy_versions_(std::make_shared<Version>(this)),
      current_(dummy_versions_) {
  AppendVersion(std::make_shared<Version>(this));
}

VersionSet::~VersionSet() {}


int VersionSet::NumLevelFiles(int level) const { return current_->NumFiles(level); }
int64_t VersionSet::NumLevelBytes(int level) const {
  assert(level >= 0);
  assert(level < config::kNumLevels);
  return TotalFileSize(current_->files_[level]);
}

void VersionSet::MarkFileNumberUsed(uint64_t number) {
  if (next_file_number_ <= number) {
    next_file_number_ = number + 1;
  }
}

std::unique_ptr<Compaction> VersionSet::PickCompaction() {
  std::unique_ptr<Compaction> c;
  int level;

  const bool size_compaction = (current_->compaction_score_ >= 1);
  const bool seek_compaction = (current_->file_to_compact_ != nullptr);
  if (size_compaction) {
    level = current_->compaction_level_;
    c = std::unique_ptr<Compaction>(new Compaction(options_, level));

    for (size_t i = 0; i < current_->files_[level].size(); i++) {
      auto f = current_->files_[level][i];
      if (compact_pointer_[level].empty() ||
          icmp_.Compare(f->largest.Encode(), compact_pointer_[level]) > 0) {
        c->inputs_[0].push_back(f);
        break;
      }
    }
    if (c->inputs_[0].empty()) {
      c->inputs_[0].push_back(current_->files_[level][0]);
    }
  } else if (seek_compaction) {
    level = current_->file_to_compact_level_;
    c = std::unique_ptr<Compaction>(new Compaction(options_, level));
    c->inputs_[0].push_back(current_->file_to_compact_);
  } else {
    return nullptr;
  }

  c->input_version_ = current_;

  if (level == 0) {
    InternalKey smallest, largest;
    GetRange(c->inputs_[0], &smallest, &largest);
    current_->GetOverlappingInputs(0, &smallest, &largest, c->inputs_[0]);
  }

  SetupOtherInputs(c.get());

  return c;
}

std::unique_ptr<Compaction> VersionSet::CompactRange(int level, const InternalKey* begin, const InternalKey* end) {
  std::vector<std::shared_ptr<FileMetaData>> inputs;
  current_->GetOverlappingInputs(level, begin, end, inputs);
  if (inputs.empty()) {
    return nullptr;
  }

  if (level > 0) {
    const uint64_t limit = MaxFileSizeForLevel(options_, level);
    uint64_t total = 0;
    for (size_t i = 0; i < inputs.size(); i++) {
      uint64_t s = inputs[i]->file_size;
      total += s;
      if (total >= limit) {
        inputs.resize(i + 1);
        break;
      }
    }
  }

  auto c = std::unique_ptr<Compaction>(new Compaction(options_, level));
  c->input_version_ = current_;
  c->inputs_[0] = inputs;
  SetupOtherInputs(c.get());
  return c;
}

int64_t VersionSet::MaxNextLevelOverlappingBytes() {
  int64_t result = 0;
  std::vector<std::shared_ptr<FileMetaData>> overlaps;
  for (int level = 1; level < config::kNumLevels - 1; level++) {
    for (size_t i = 0; i < current_->files_[level].size(); i++) {
      auto f = current_->files_[level][i];
      current_->GetOverlappingInputs(level + 1, &f->smallest, &f->largest, overlaps);
      const int64_t sum = TotalFileSize(overlaps);
      if (sum > result) {
        result = sum;
      }
    }
  }
  return result;
}

std::unique_ptr<Iterator> VersionSet::MakeInputIterator(Compaction* c) {
  ReadOptions options;
  options.verify_checksums = options_->paranoid_checks;
  options.fill_cache = false;

  std::vector<std::unique_ptr<Iterator>> list;
  for (int which = 0; which < 2; which++) {
    if (!c->inputs_[which].empty()) {
      if (c->level() + which == 0) {
        const auto& files = c->inputs_[which];
        for (size_t i = 0; i < files.size(); i++) {
          list.push_back(table_cache_->NewIterator(options, files[i]->number, files[i]->file_size));
        }
      } else {
        auto index_iter = std::make_unique<Version::LevelFileNumIterator>(icmp_, &c->inputs_[which]);
        list.push_back(NewTwoLevelIterator(
            std::move(index_iter),
            [this](const ReadOptions& options, std::string_view index_value) -> std::unique_ptr<Iterator> {
              if (index_value.size() != 16) {
                return std::unique_ptr<Iterator>(NewErrorIterator(std::unexpected(Status::Corruption("FileReader invoked with unexpected value"))));
              }
              uint64_t file_number = DecodeFixed64(index_value.data());
              uint64_t file_size = DecodeFixed64(index_value.data() + 8);
              return table_cache_->NewIterator(options, file_number, file_size);
            },
            options));
      }
    }
  }
  return NewMergingIterator(&icmp_, std::move(list));
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
  for (auto v = dummy_versions_->next_; v != dummy_versions_.get(); v = v->next_) {
    for (int level = 0; level < config::kNumLevels; level++) {
      const auto& files = v->files_[level];
      for (size_t i = 0; i < files.size(); i++) {
        live->insert(files[i]->number);
      }
    }
  }
}

uint64_t VersionSet::ApproximateOffsetOf(std::shared_ptr<Version> v, const InternalKey& ikey) {
  uint64_t result = 0;
  for (int level = 0; level < config::kNumLevels; level++) {
    const auto& files = v->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      if (icmp_.Compare(files[i]->largest, ikey) <= 0) {
        result += files[i]->file_size;
      } else if (icmp_.Compare(files[i]->smallest, ikey) > 0) {
        if (level > 0) {
          break;
        }
      } else {
        result += table_cache_->ApproximateOffsetOf(files[i]->number, files[i]->file_size, ikey.Encode());
      }
    }
  }
  return result;
}

bool VersionSet::ReuseManifest(std::string_view dscname, std::string_view dscbase) {
  return false; // Skip reuse manifest for now
}

class VersionSet::Builder {
 private:
  struct BySmallestKey {
    const InternalKeyComparator* internal_comparator;
    bool operator()(const std::shared_ptr<FileMetaData>& f1,
                    const std::shared_ptr<FileMetaData>& f2) const {
      auto r = internal_comparator->Compare(f1->smallest, f2->smallest);
      if (r != 0) return (r < 0);
      return (f1->number < f2->number);
    }
  };

  typedef std::set<std::shared_ptr<FileMetaData>, BySmallestKey> FileSet;
  struct LevelState {
    std::set<uint64_t> deleted_files;
    FileSet added_files;
  };

  VersionSet* vset_;
  std::shared_ptr<Version> base_;
  LevelState levels_[config::kNumLevels];

 public:
  Builder(VersionSet* vset, std::shared_ptr<Version> base)
      : vset_(vset), base_(base) {
    BySmallestKey cmp;
    cmp.internal_comparator = &vset_->icmp_;
    for (int level = 0; level < config::kNumLevels; level++) {
      levels_[level].added_files = FileSet(cmp);
    }
  }

  void Apply(VersionEdit* edit) {
    for (size_t i = 0; i < edit->compact_pointers_.size(); i++) {
      const int level = edit->compact_pointers_[i].first;
      vset_->compact_pointer_[level] =
          std::string(edit->compact_pointers_[i].second.Encode());
    }

    for (const auto& deleted : edit->deleted_files_) {
      levels_[deleted.first].deleted_files.insert(deleted.second);
    }

    for (size_t i = 0; i < edit->new_files_.size(); i++) {
      const int level = edit->new_files_[i].first;
      auto f = edit->new_files_[i].second;
      f->allowed_seeks = static_cast<int>((f->file_size / 16384U));
      if (f->allowed_seeks < 100) f->allowed_seeks = 100;

      levels_[level].deleted_files.erase(f->number);
      levels_[level].added_files.insert(f);
    }
  }

  void SaveTo(std::shared_ptr<Version> v) {
    BySmallestKey cmp;
    cmp.internal_comparator = &vset_->icmp_;
    for (int level = 0; level < config::kNumLevels; level++) {
      const auto& base_files = base_->files_[level];
      auto base_iter = base_files.begin();
      auto base_end = base_files.end();
      const auto& added_files = levels_[level].added_files;
      v->files_[level].reserve(base_files.size() + added_files.size());

      for (const auto& added_file : added_files) {
        for (auto bpos = std::upper_bound(base_iter, base_end, added_file, cmp);
             base_iter != bpos; ++base_iter) {
          MaybeAddFile(v.get(), level, *base_iter);
        }
        MaybeAddFile(v.get(), level, added_file);
      }

      for (; base_iter != base_end; ++base_iter) {
        MaybeAddFile(v.get(), level, *base_iter);
      }
    }
  }

  void MaybeAddFile(Version* v, int level, std::shared_ptr<FileMetaData> f) {
    if (levels_[level].deleted_files.count(f->number) > 0) {
      // File is deleted: do nothing
    } else {
      std::vector<std::shared_ptr<FileMetaData>>* files = &v->files_[level];
      if (level > 0 && !files->empty()) {
        assert(vset_->icmp_.Compare((*files)[files->size() - 1]->largest,
                                    f->smallest) < 0);
      }
      files->push_back(f);
    }
  }
};

void VersionSet::Finalize(std::shared_ptr<Version> v) {
  int best_level = -1;
  double best_score = -1;

  for (int level = 0; level < config::kNumLevels - 1; level++) {
    double score;
    if (level == 0) {
      score = v->files_[level].size() / static_cast<double>(config::kL0_CompactionTrigger);
    } else {
      const uint64_t level_bytes = TotalFileSize(v->files_[level]);
      score = static_cast<double>(level_bytes) / MaxBytesForLevel(options_, level);
    }

    if (score > best_score) {
      best_score = score;
      best_level = level;
    }
  }

  v->compaction_level_ = best_level;
  v->compaction_score_ = best_score;
}

Result<void> VersionSet::LogAndApply(VersionEdit* edit) {
  if (edit->log_number_.has_value()) {
    assert(*edit->log_number_ >= log_number_);
    assert(*edit->log_number_ < next_file_number_);
  } else {
    edit->SetLogNumber(log_number_);
  }

  if (!edit->prev_log_number_.has_value()) {
    edit->SetPrevLogNumber(prev_log_number_);
  }

  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(last_sequence_);

  auto v = std::make_shared<Version>(this);
  {
    Builder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);
  }
  Finalize(v);

  std::string new_manifest_file;
  Status s;
  if (descriptor_log_ == nullptr) {
    new_manifest_file = DescriptorFileName(dbname_, manifest_file_number_);
    auto file_res = env_->NewWritableFile(new_manifest_file);
    if (file_res) {
      descriptor_file_ = std::make_unique<StdFileSystem::WritableFile>(std::move(*file_res));
      descriptor_log_ = std::make_unique<log::Writer<StdFileSystem::WritableFile>>(descriptor_file_.get());
      auto write_res = WriteSnapshot(descriptor_log_.get());
      if (!write_res) s = write_res.error();
    } else {
      s = file_res.error();
    }
  }

  if (s.ok()) {
    std::string record;
    edit->EncodeTo(record);
    auto write_res = descriptor_log_->AddRecord(record);
    if (write_res) {
      auto sync_res = descriptor_file_->Sync();
      if (!sync_res) s = sync_res.error();
    } else {
      s = write_res.error();
    }
  }

  if (s.ok() && !new_manifest_file.empty()) {
    auto set_res = SetCurrentFile(env_, dbname_, manifest_file_number_);
    if (!set_res) s = set_res.error();
  }

  if (s.ok()) {
    AppendVersion(v);
    log_number_ = *edit->log_number_;
    prev_log_number_ = *edit->prev_log_number_;
    return {};
  } else {
    if (!new_manifest_file.empty()) {
      descriptor_log_.reset();
      descriptor_file_.reset();
      env_->RemoveFile(new_manifest_file);
    }
    return std::unexpected(s);
  }
}

Result<bool> VersionSet::Recover() {
  std::string current_file = CurrentFileName(dbname_);
  auto current_res = env_->NewSequentialFile(current_file);
  if (!current_res) {
    return std::unexpected(current_res.error());
  }

  std::string current_content(1024, '\0');
  auto read_res = (*current_res).Read(1024, current_content.data());
  if (!read_res) return std::unexpected(read_res.error());
  current_content.assign(read_res->data(), read_res->size());

  if (current_content.empty() || current_content.back() != '\n') {
    return std::unexpected(Status::Corruption("CURRENT file does not end with newline"));
  }
  current_content.pop_back();

  std::string manifest_file = dbname_ + "/" + current_content;
  auto file_res = env_->NewSequentialFile(manifest_file);
  if (!file_res) return std::unexpected(file_res.error());

  log::Reader<StdFileSystem::SequentialFile> reader(&(*file_res), nullptr, true, 0);
  std::string scratch;

  Builder builder(this, current_);
  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  uint64_t log_number = 0;
  uint64_t prev_log_number = 0;
  bool have_log_number = false;
  bool have_prev_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;

  while (auto record_opt = reader.ReadRecord(scratch)) {
    VersionEdit edit;
    auto s = edit.DecodeFrom(*record_opt);
    if (!s) {
      return std::unexpected(s.error());
    }

    if (edit.comparator_.has_value() && *edit.comparator_ != icmp_.user_comparator()->Name()) {
      return std::unexpected(Status::InvalidArgument(
          *edit.comparator_ + " does not match existing comparator ",
          icmp_.user_comparator()->Name()));
    }

    builder.Apply(&edit);

    if (edit.log_number_.has_value()) {
      log_number = *edit.log_number_;
      have_log_number = true;
    }

    if (edit.prev_log_number_.has_value()) {
      prev_log_number = *edit.prev_log_number_;
      have_prev_log_number = true;
    }

    if (edit.next_file_number_.has_value()) {
      next_file = *edit.next_file_number_;
      have_next_file = true;
    }

    if (edit.last_sequence_.has_value()) {
      last_sequence = *edit.last_sequence_;
      have_last_sequence = true;
    }
  }

  if (!have_next_file) {
    return std::unexpected(Status::Corruption("no meta-nextfile entry in descriptor"));
  }
  if (!have_log_number) {
    return std::unexpected(Status::Corruption("no meta-lognumber entry in descriptor"));
  }
  if (!have_last_sequence) {
    return std::unexpected(Status::Corruption("no last-sequence-number entry in descriptor"));
  }

  if (!have_prev_log_number) {
    prev_log_number = 0;
  }

  MarkFileNumberUsed(prev_log_number);
  MarkFileNumberUsed(log_number);

  auto v = std::make_shared<Version>(this);
  builder.SaveTo(v);
  Finalize(v);
  AppendVersion(v);

  manifest_file_number_ = next_file;
  next_file_number_ = next_file + 1;
  last_sequence_ = last_sequence;
  log_number_ = log_number;
  prev_log_number_ = prev_log_number;

  // Try to reuse the manifest file.
  if (ReuseManifest(manifest_file, current_content)) {
    // OK
  } else {
    // We will create a new one when LogAndApply is called.
  }

  return true;
}

Result<void> VersionSet::WriteSnapshot(log::Writer<StdFileSystem::WritableFile>* log) {
  VersionEdit edit;
  edit.SetComparatorName(icmp_.user_comparator()->Name());

  for (int level = 0; level < config::kNumLevels; level++) {
    if (!compact_pointer_[level].empty()) {
      InternalKey key;
      key.DecodeFrom(compact_pointer_[level]);
      edit.SetCompactPointer(level, key);
    }
  }

  for (int level = 0; level < config::kNumLevels; level++) {
    const auto& files = current_->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      const auto& f = files[i];
      edit.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
    }
  }

  std::string record;
  edit.EncodeTo(record);
  return log->AddRecord(record);
}

void VersionSet::GetRange(const std::vector<std::shared_ptr<FileMetaData>>& inputs, InternalKey* smallest, InternalKey* largest) {
  assert(!inputs.empty());
  smallest->Clear();
  largest->Clear();
  for (size_t i = 0; i < inputs.size(); i++) {
    auto f = inputs[i];
    if (i == 0) {
      *smallest = f->smallest;
      *largest = f->largest;
    } else {
      if (icmp_.Compare(f->smallest, *smallest) < 0) {
        *smallest = f->smallest;
      }
      if (icmp_.Compare(f->largest, *largest) > 0) {
        *largest = f->largest;
      }
    }
  }
}

void VersionSet::GetRange2(const std::vector<std::shared_ptr<FileMetaData>>& inputs1, const std::vector<std::shared_ptr<FileMetaData>>& inputs2, InternalKey* smallest, InternalKey* largest) {
  std::vector<std::shared_ptr<FileMetaData>> all = inputs1;
  all.insert(all.end(), inputs2.begin(), inputs2.end());
  GetRange(all, smallest, largest);
}

bool FindLargestKey(const InternalKeyComparator& icmp,
                    const std::vector<std::shared_ptr<FileMetaData>>& files,
                    InternalKey* largest_key) {
  if (files.empty()) {
    return false;
  }
  *largest_key = files[0]->largest;
  for (size_t i = 1; i < files.size(); ++i) {
    auto f = files[i];
    if (icmp.Compare(f->largest, *largest_key) > 0) {
      *largest_key = f->largest;
    }
  }
  return true;
}

std::shared_ptr<FileMetaData> FindSmallestBoundaryFile(
    const InternalKeyComparator& icmp,
    const std::vector<std::shared_ptr<FileMetaData>>& level_files,
    const InternalKey& largest_key) {
  const Comparator* user_cmp = icmp.user_comparator();
  std::shared_ptr<FileMetaData> smallest_boundary_file = nullptr;
  for (size_t i = 0; i < level_files.size(); ++i) {
    auto f = level_files[i];
    if (icmp.Compare(f->smallest, largest_key) > 0 &&
        user_cmp->Compare(f->smallest.user_key(), largest_key.user_key()) == 0) {
      if (smallest_boundary_file == nullptr ||
          icmp.Compare(f->smallest, smallest_boundary_file->smallest) < 0) {
        smallest_boundary_file = f;
      }
    }
  }
  return smallest_boundary_file;
}

void AddBoundaryInputs(const InternalKeyComparator& icmp,
                       const std::vector<std::shared_ptr<FileMetaData>>& level_files,
                       std::vector<std::shared_ptr<FileMetaData>>* compaction_files) {
  InternalKey largest_key;
  if (!FindLargestKey(icmp, *compaction_files, &largest_key)) {
    return;
  }

  bool continue_searching = true;
  while (continue_searching) {
    auto smallest_boundary_file = FindSmallestBoundaryFile(icmp, level_files, largest_key);
    if (smallest_boundary_file != nullptr) {
      compaction_files->push_back(smallest_boundary_file);
      largest_key = smallest_boundary_file->largest;
    } else {
      continue_searching = false;
    }
  }
}

void VersionSet::SetupOtherInputs(Compaction* c) {
  const int level = c->level();
  InternalKey smallest, largest;

  AddBoundaryInputs(icmp_, current_->files_[level], &c->inputs_[0]);
  GetRange(c->inputs_[0], &smallest, &largest);

  current_->GetOverlappingInputs(level + 1, &smallest, &largest, c->inputs_[1]);
  AddBoundaryInputs(icmp_, current_->files_[level + 1], &c->inputs_[1]);

  InternalKey all_start, all_limit;
  GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

  if (!c->inputs_[1].empty()) {
    std::vector<std::shared_ptr<FileMetaData>> expanded0;
    current_->GetOverlappingInputs(level, &all_start, &all_limit, expanded0);
    AddBoundaryInputs(icmp_, current_->files_[level], &expanded0);
    const int64_t inputs0_size = TotalFileSize(c->inputs_[0]);
    const int64_t inputs1_size = TotalFileSize(c->inputs_[1]);
    const int64_t expanded0_size = TotalFileSize(expanded0);
    if (expanded0.size() > c->inputs_[0].size() &&
        inputs1_size + expanded0_size < ExpandedCompactionByteSizeLimit(options_)) {
      InternalKey new_start, new_limit;
      GetRange(expanded0, &new_start, &new_limit);
      std::vector<std::shared_ptr<FileMetaData>> expanded1;
      current_->GetOverlappingInputs(level + 1, &new_start, &new_limit, expanded1);
      AddBoundaryInputs(icmp_, current_->files_[level + 1], &expanded1);
      if (expanded1.size() == c->inputs_[1].size()) {
        smallest = new_start;
        largest = new_limit;
        c->inputs_[0] = expanded0;
        c->inputs_[1] = expanded1;
        GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
      }
    }
  }

  if (level + 2 < config::kNumLevels) {
    current_->GetOverlappingInputs(level + 2, &all_start, &all_limit, c->grandparents_);
  }

  compact_pointer_[level] = largest.Encode();
  c->edit_.SetCompactPointer(level, largest);
}

void VersionSet::AppendVersion(std::shared_ptr<Version> v) {
  // Make "v" current
  if (current_ != dummy_versions_) {
    // We do not manage refs here manually anymore, current_ is overwritten.
  }
  
  v->prev_ = dummy_versions_->prev_;
  v->next_ = dummy_versions_.get();
  v->prev_->next_ = v.get();
  v->next_->prev_ = v.get();
  current_ = v;
}

Compaction::Compaction(const Options<StdFileSystem>* options, int level)
    : level_(level),
      max_output_file_size_(MaxFileSizeForLevel(options, level)) {
  for (int i = 0; i < config::kNumLevels; i++) {
    level_ptrs_[i] = 0;
  }
}

bool Compaction::IsTrivialMove() const {
  const VersionSet* vset = input_version_->vset_;
  return (num_input_files(0) == 1 && num_input_files(1) == 0 &&
          TotalFileSize(grandparents_) <= MaxGrandParentOverlapBytes(vset->options_));
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
  for (int which = 0; which < 2; which++) {
    for (size_t i = 0; i < inputs_[which].size(); i++) {
      edit->RemoveFile(level_ + which, inputs_[which][i]->number);
    }
  }
}

bool Compaction::IsBaseLevelForKey(std::string_view user_key) {
  const Comparator* user_cmp = input_version_->vset_->icmp_.user_comparator();
  for (int lvl = level_ + 2; lvl < config::kNumLevels; lvl++) {
    const auto& files = input_version_->files_[lvl];
    while (level_ptrs_[lvl] < files.size()) {
      auto f = files[level_ptrs_[lvl]];
      if (user_cmp->Compare(user_key, f->largest.user_key()) <= 0) {
        if (user_cmp->Compare(user_key, f->smallest.user_key()) >= 0) {
          return false;
        }
        break;
      }
      level_ptrs_[lvl]++;
    }
  }
  return true;
}

bool Compaction::ShouldStopBefore(std::string_view internal_key) {
  const VersionSet* vset = input_version_->vset_;
  const InternalKeyComparator* icmp = &vset->icmp_;
  while (grandparent_index_ < grandparents_.size() &&
         icmp->Compare(internal_key, grandparents_[grandparent_index_]->largest.Encode()) > 0) {
    if (seen_key_) {
      overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
    }
    grandparent_index_++;
  }
  seen_key_ = true;

  if (overlapped_bytes_ > MaxGrandParentOverlapBytes(vset->options_)) {
    overlapped_bytes_ = 0;
    return true;
  } else {
    return false;
  }
}

void Compaction::ReleaseInputs() {
  if (input_version_ != nullptr) {
    input_version_ = nullptr;
  }
}

}  // namespace leveldb
