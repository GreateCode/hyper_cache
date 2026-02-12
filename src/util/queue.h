#pragma once

#include <cstdint>
#include <iostream>
#include <vector>

#include "util/atomic.h"
#include "util/util.h"

namespace hyper_cache {

template <typename T>
class Queue {
public:
    explicit Queue(int64_t capicity) : capicity_(capicity), buffer_(capicity) {
        // initialize the seq value of each slot to the slot index
        // available for the producer
        for (size_t i = 0; i < capicity_; i++) {
            buffer_[i].seq.StoreRelaxed(i);
        }
        head_.StoreRelaxed(0);
        tail_.StoreRelaxed(0);
    }

    // push: return true if successful, false if the queue is full
    bool Push(const T& value) {
        Node* node;
        int64_t pos = head_.LoadAcquire();

        while (true) {
            node              = &buffer_[pos % capicity_];
            const int64_t seq = node->seq.LoadAcquire();

            // diff == 0 means the slot is just available for the current producer
            const int64_t diff = seq - pos;

            if (diff == 0) {
                // try to push the head from pos to pos+1
                if (head_.CasWeakAcqRel(pos, pos + 1L)) {
                    // succeed to occupy the slot, exit the loop
                    break;
                }
                // if CAS fails, pos will be updated to the latest value, continue to
                // retry
            } else if (diff < 0) {
                // diff < 0 means seq < pos, the queue is full
                return false;
            } else {
                // diff > 0 means other producers have occupied this slot
                // update pos, continue to try
                pos = head_.LoadAcquire();
            }
        }

        // write data to the occupied slot
        node->data = value;
        // update seq, mark the slot as filled, can be consumed by the consumer
        node->seq.StoreRelease(pos + 1);
        return true;
    }

    // pop: return true if successful, false if the queue is empty
    bool Pop(T* value) {
        Node* node;
        int64_t pos = tail_.LoadAcquire();

        while (true) {
            node              = &buffer_[pos % capicity_];
            const int64_t seq = node->seq.LoadAcquire();

            // for the consumer, the seq of the slot should be equal to pos+1
            const int64_t diff = seq - (pos + 1L);

            if (diff == 0) {
                // try to push the tail from pos to pos+1
                if (tail_.CasWeakAcqRel(pos, pos + 1L)) {
                    // succeed to occupy the slot, exit the loop
                    break;
                }
                // if CAS fails, pos will be updated to the latest value, continue to
                // retry
            } else if (diff < 0) {
                // diff < 0 means seq < pos+1, the slot is not filled by the producer
                // the queue is empty
                return false;
            } else {
                // diff > 0 means other consumers have occupied this slot
                // update pos, continue to try
                pos = tail_.LoadAcquire();
            }
        }

        // get the data
        *value = node->data;
        // reset the seq to pos+capicity, mark the slot as empty
        node->seq.StoreRelease(pos + capicity_);
        return true;
    }

    int64_t Size() const {
        int64_t size = head_.LoadRelaxed() - tail_.LoadRelaxed();
        if (likely(size >= 0)) {
            return size;
        }
        return 0;
    }
    int64_t Capicity() const { return capicity_; }

private:
    struct Node {
        AcqRelAtomic<int64_t> seq;  // for marking the slot status
        T data;                     // for storing the data
    };

    int64_t capicity_;
    std::vector<Node> buffer_;
    AcqRelAtomic<int64_t> head_;
    AcqRelAtomic<int64_t> tail_;
};

}  // namespace hyper_cache
