#pragma once

// Educational skiplist (LevelDB-style). Template keyed by Comparator on Key.
//
// Thread-safety: NOT safe for concurrent writers. Concurrent readers are only
// safe when there are no concurrent writers (externally synchronized use, e.g.
// under a DB mutex for mutations). No internal locks.

#include <cassert>
#include <cstdint>
#include <new>
#include <random>
#include <utility>

namespace tinylsm {

template <typename Key, typename Comparator>
class SkipList {
 public:
  // Max tower height (LevelDB uses 12).
  static constexpr int kMaxHeight = 12;
  // Probability of increasing height is 1/kBranching.
  static constexpr int kBranching = 4;

  // Requires: Comparator is copyable and implements
  //   int Compare(const Key& a, const Key& b) const
  // returning <0 / 0 / >0.
  explicit SkipList(Comparator cmp)
      : compare_(std::move(cmp)),
        head_(new Node(Key{}, kMaxHeight)),
        max_height_(1),
        rng_(0xdeadbeefu) {
    for (int i = 0; i < kMaxHeight; ++i) {
      head_->next[i] = nullptr;
    }
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  ~SkipList() {
    Node* node = head_->next[0];
    while (node != nullptr) {
      Node* next = node->next[0];
      delete node;
      node = next;
    }
    delete head_;
  }

  // Insert key into the list. Requires: !Contains(key) (no duplicate keys).
  void Insert(const Key& key) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);

    // Do not allow duplicate keys.
    assert(x == nullptr || !equal(key, x->key));

    const int height = RandomHeight();
    if (height > max_height_) {
      for (int i = max_height_; i < height; ++i) {
        prev[i] = head_;
      }
      max_height_ = height;
    }

    x = new Node(key, height);
    for (int i = 0; i < height; ++i) {
      x->next[i] = prev[i]->next[i];
      prev[i]->next[i] = x;
    }
    // Higher unused levels remain nullptr from Node construction.
  }

  // Returns true iff an entry that compares equal to key is in the list.
  bool Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    return x != nullptr && equal(key, x->key);
  }

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

    bool Valid() const { return node_ != nullptr; }

    // REQUIRES: Valid()
    const Key& key() const {
      assert(Valid());
      return node_->key;
    }

    // Advances to the next entry. REQUIRES: Valid()
    void Next() {
      assert(Valid());
      node_ = node_->next[0];
    }

    // Position at the first entry with a key >= target.
    // Valid() iff such an entry exists.
    void Seek(const Key& target) {
      node_ = list_->FindGreaterOrEqual(target, nullptr);
    }

    // Position at the first entry. Valid() iff the list is not empty.
    void SeekToFirst() { node_ = list_->head_->next[0]; }

   private:
    const SkipList* list_;
    typename SkipList::Node* node_;
  };

 private:
  struct Node {
    Key key;
    int height;
    // Fixed-size tower; only next[0..height) are used for linking.
    Node* next[kMaxHeight];

    Node(const Key& k, int h) : key(k), height(h) {
      for (int i = 0; i < kMaxHeight; ++i) {
        next[i] = nullptr;
      }
    }
  };

  int RandomHeight() {
    int height = 1;
    while (height < kMaxHeight && (rng_() % kBranching) == 0) {
      ++height;
    }
    return height;
  }

  bool equal(const Key& a, const Key& b) const {
    return compare_.Compare(a, b) == 0;
  }

  // True if (n != nullptr) and n->key < key.
  bool key_is_after_node(const Key& key, Node* n) const {
    return (n != nullptr) && (compare_.Compare(n->key, key) < 0);
  }

  // Return the earliest node that compares >= key.
  // If prev != nullptr, fill prev[level] with the last node < key at each level.
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x = head_;
    int level = max_height_ - 1;
    while (true) {
      Node* next = x->next[level];
      if (key_is_after_node(key, next)) {
        x = next;
      } else {
        if (prev != nullptr) {
          prev[level] = x;
        }
        if (level == 0) {
          return next;
        }
        --level;
      }
    }
  }

  Comparator compare_;
  Node* const head_;
  int max_height_;
  // PRNG for height selection only (not for security).
  std::mt19937 rng_;
};

}  // namespace tinylsm
