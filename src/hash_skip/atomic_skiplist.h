#pragma once

#include <cassert>
#include <climits>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <random>

#include "hash_skip/node.h"
#include "hash_skip/ptr_util.h"

namespace cache::hash_skip {

class AtomicSkipList {
public:
  AtomicSkipList() {
    head_ = new Node(INT64_MIN, nullptr, max_level_);
    tail_ = new Node(INT64_MAX, nullptr, max_level_);

    for (int8_t i = 0; i <= max_level_; ++i) {
      head_->nexts[i].StoreRelaxed(tail_);
    }
  }

  ~AtomicSkipList() {
    Node *pred = head_;
    Node *curr = nullptr;
    while (true) {
      curr = static_cast<Node *>(pred->nexts[0].LoadAcquire().GetRawPtr());
      delete pred;
      if (curr == tail_) {
        delete curr;
        break;
      }
      pred = curr;
    }
  }

  // int64_t GetMemoryUsage() { return memory_used_.LoadRelaxed(); }
  int64_t GetNodeNum() { return node_num_.LoadRelaxed(); }
  int64_t GetMarkedDeletedNum() { return marked_deleted_num_.LoadRelaxed(); }

  bool Get(int64_t key, std::shared_ptr<Handle> *handle) {
    Node *node = Get(key);
    if (!node) {
      return false;
    }
    *handle = node->GetHandle();
    node->IncrHitCount();
    return *handle != nullptr;
  }

  Node *Get(int64_t key) {
    Node *preds[max_level_ + 1];
    MarkVersionedPtr succs[max_level_ + 1];
    if (!Find(key, preds, succs, true)) {
      return nullptr;
    }
    Node *node = static_cast<Node *>(succs[0].GetRawPtr());
    if (node->IsMarked()) {
      return nullptr;
    }
    return node;
  }

  std::shared_ptr<Handle> Insert(int64_t key, std::shared_ptr<Handle> handle) {
    if (!handle) {
      return nullptr;
    }
    Node *preds[max_level_ + 1];
    MarkVersionedPtr succs[max_level_ + 1];
    int64_t charge = handle ? handle->Charge() : 0;

    while (true) {
      if (Find(key, preds, succs)) {
        Node *node = static_cast<Node *>(succs[0].GetRawPtr());
        std::shared_ptr<Handle> old_handle =
            node->UpdateHandle(std::move(handle));
        // if (old_handle) {
        //     memory_used_.FetchAddRelaxed(charge - old_handle->Charge());
        // }
        return old_handle;
      }

      const int8_t top_level = RandomLevel();
      Node *new_node = new Node(key, std::move(handle), top_level);

      // 新node的next指针指向succs[level]，无并发问题
      for (int8_t level = 0; level <= top_level; ++level) {
        Node *succ_raw = static_cast<Node *>(succs[level].GetRawPtr());
        new_node->nexts[level].StoreRelaxed(
            MarkVersionedPtr::PackPtr(succ_raw, 0, false));
      }

      // 关键：先在 level 0 插入（使用 pred->next[0] 的完整 packed 值的版本+1）
      if (!LinkLevel(preds[0], succs[0], new_node, 0, true)) {
        delete new_node;
        continue;
      }

      for (int8_t level = 1; level <= top_level; ++level) {
        while (!LinkLevel(preds[level], succs[level], new_node, level, false)) {
          Find(key, preds, succs);
        }
      }
      node_num_.FetchAddRelaxed(1);
      // memory_used_.FetchAddRelaxed(charge);
      return nullptr;
    }
  }

  // 遍历level0所有节点
  void IterateLevel(std::function<bool(Node *)> fn, int8_t level = 0) {
    Node *pred = head_;
    Node *curr = nullptr;
    while (true) {
      const MarkVersionedPtr ptr = pred->nexts[level].LoadAcquire();
      curr = static_cast<Node *>(ptr.GetRawPtr());

      if (curr->IsMarked()) { // 被标记，跳过
        pred = curr;
        continue;
      }
      if (curr == tail_) {
        break;
      }
      if (!fn(curr)) {
        break;
      }
      pred = curr;
    }
  }

  // 逻辑删除接口：只在 level 0 上打标记
  int32_t MarkDeleted(Node *node) {
    const int32_t charge = node->MarkDeleted();
    if (charge > 0) {
      // memory_used_.FetchSubRelaxed(charge);
      marked_deleted_num_.FetchAddRelaxed(1);
    }
    return charge;
  }

  void DebugPrint() {
    for (int level = max_level_; level >= 0; --level) {
      const MarkVersionedPtr cur_ptr = head_->nexts[level].LoadAcquire();
      Node *node = static_cast<Node *>(cur_ptr.GetRawPtr());
      bool valid = false;
      if (node != tail_) {
        std::cout << "Level " << level << ": ";
        valid = true;
      }
      while (node != tail_) {
        std::cout << node->key << " ";
        const MarkVersionedPtr curr_ptr = node->nexts[level].LoadAcquire();
        node = static_cast<Node *>(curr_ptr.GetRawPtr());
      }
      if (valid) {
        std::cout << "\n";
      }
    }
    std::cout << std::endl;
  }

private:
  // preds: 目标前节点
  // succs: 目标或目标后节点
  bool Find(int64_t key, Node **preds, MarkVersionedPtr *succs,
            bool skip_marked = false) {
    bool found = false;
    Node *pred = head_;
    for (int8_t level = max_level_; level >= 0; --level) {
      MarkVersionedPtr curr_ptr = pred->nexts[level].LoadAcquire();
      Node *curr = static_cast<Node *>(curr_ptr.GetRawPtr());

      while (curr->key < key) {
        pred = curr;
        // curr_ptr = curr->nexts[level].LoadAcquire();
        curr_ptr = curr->nexts[level].LoadRelaxed();
        curr = static_cast<Node *>(curr_ptr.GetRawPtr());
      }
      preds[level] = pred;
      succs[level] = curr_ptr; // 记录 curr 的 packed pointer
      if (!found && curr->key == key) {
        found = true;
      }
    }
    return found;
  }

  int RandomLevel() {
    static thread_local std::mt19937 gen(std::random_device{}());
    static thread_local std::uniform_real_distribution<float> dist(0.0, 1.0);
    int8_t level = 0;
    while (dist(gen) < probability_ && level < max_level_) {
      ++level;
    }
    return level;
  }

  bool LinkLevel(Node *pred, MarkVersionedPtr expected, Node *new_node,
                 int8_t level, bool strong) {
    // 期望的后继应当是 succs[0] (但 expectedPacked 里可能含版本/标记)
    // 在 desired 中设置 pointer=newNode，version = expected.version +1，mark=0
    const MarkVersionedPtr desired =
        MarkVersionedPtr::PackPtr(new_node, expected.GetVersion() + 1, false);
    if (strong) {
      return pred->nexts[level].CasStrongAcqRel(expected, desired);
    }
    return pred->nexts[level].CasWeakAcqRel(expected, desired);
  }

private:
  static constexpr int8_t max_level_ = 15; // 最高层序号,总层数+1
  const float probability_ = 0.5;
  Node *head_;
  Node *tail_;
  // int64_t capacity_{0};
  // AcqRelAtomic<int64_t> memory_used_{0};

  AcqRelAtomic<int64_t> node_num_{0};
  AcqRelAtomic<int64_t> marked_deleted_num_{0};
};

} // namespace cache::hash_skip
