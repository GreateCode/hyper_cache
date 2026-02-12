#pragma once

#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hyper_cache::lru {

struct CacheSharedOptions {
    size_t capacity   = 0;  // total capacity of the cache
    int64_t entry_num = 0;  // number of entries in the cache
    size_t value_size = 0;  // only set one of entry_num , value_size.
};

using ObjectPtr = std::shared_ptr<int64_t>;
using DeleterFn = void (*)(ObjectPtr obj);

struct Handle {
    ObjectPtr value     = nullptr;
    DeleterFn del_cb    = nullptr;
    size_t total_charge = 0;

    size_t GetTotalCharge() const { return total_charge; }

    void FreeData() {
        if (del_cb && value) {
            del_cb(value);
            value        = nullptr;
            total_charge = 0;
        }
    }
};

template <typename Key>
class LRUCacheShard {
public:
    explicit LRUCacheShard(const CacheSharedOptions& options) : capacity_(options.capacity) {
        if (options.entry_num > 0) {
            max_entry_num_ = options.entry_num;
        } else if (options.value_size > 0) {
            max_entry_num_ = capacity_ / options.value_size;
        }
    }
    ~LRUCacheShard() {
        for (auto& kv : entries_) {
            kv.second.FreeData();
        }
    }
    size_t GetUsage() const { return usage_; }
    size_t GetCapacity() const { return capacity_; }
    int64_t GetMaxEntryNum() const { return max_entry_num_; }
    size_t GetEntryNum() const { return entries_.size(); }

    bool Get(const Key& key, Handle* handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }

        entries_.splice(entries_.begin(), entries_, it->second);
        *handle = it->second->second;
        return true;
    }

    void Insert(const Key& key, const Handle& handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            Handle& h                = it->second->second;
            const size_t need_charge = handle.GetTotalCharge() - h.GetTotalCharge();
            if (need_charge > 0) {
                EvictForCharge(need_charge);
            }
            h.FreeData();

            h = handle;
            entries_.splice(entries_.begin(), entries_, it->second);
            return;
        }

        EvictForCharge(handle.GetTotalCharge());
        EvictForOccupancy();

        entries_.emplace_front(key, handle);
        map_[key] = entries_.begin();

        usage_ += handle.GetTotalCharge();
    }

    void Print() const {
        for (auto& kv : entries_) {
            std::cout << kv.first << ":" << kv.second.value << " ";
        }
        std::cout << std::endl;
    }

private:
    void EvictForCharge(size_t need_charge) {
        if (capacity_ == 0) {
            return;
        }
        while (usage_ + need_charge > capacity_) {
            auto& last = entries_.back();
            map_.erase(last.first);
            entries_.pop_back();
            last.second.FreeData();
            usage_ -= last.second.GetTotalCharge();
        }
    }
    void EvictForOccupancy() {
        if (max_entry_num_ == 0) {
            return;
        }
        while (entries_.size() >= max_entry_num_) {
            auto& last = entries_.back();
            map_.erase(last.first);
            entries_.pop_back();
            last.second.FreeData();
            usage_ -= last.second.GetTotalCharge();
        }
    }

private:
    const size_t capacity_ = 0;
    int64_t max_entry_num_ = 0;
    size_t usage_          = 0;
    std::list<std::pair<Key, Handle>> entries_;
    std::unordered_map<Key, typename std::list<std::pair<Key, Handle>>::iterator> map_;
    std::mutex mutex_;
};

}  // namespace hyper_cache::lru
