#pragma once

#include <vector>
#include <memory>

namespace leveldb {

class Comparator;
class Iterator;

std::unique_ptr<Iterator> NewMergingIterator(const Comparator* comparator,
                                             std::vector<std::unique_ptr<Iterator>> children);

}  // namespace leveldb
