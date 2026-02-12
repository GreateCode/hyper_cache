#pragma once

#include <cstdint>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

namespace hyper_cache {

struct Node {
    std::string key;
    std::string value;

    std::vector<Node*> next;

    Node(const std::string& key, const std::string& value, uint8_t level) : key(key), value(value), next(level) {}
};

class SkipList {
public:
    SkipList() {
        head_          = new Node("", "", max_level_);
        cur_max_level_ = 1;
        std::srand(std::time(nullptr));
    }

    ~SkipList() {
        Node* node = head_;
        while (node) {
            Node* next = node->next[0];
            delete node;
            node = next;
        }
        head_ = nullptr;
    }

    bool Get(const std::string& key, std::string* value) {
        Node* node = head_;
        for (int8_t level = cur_max_level_ - 1; level >= 0; --level) {
            while (node->next[level] && node->next[level]->key < key) {
                node = node->next[level];
            }
        }
        node = node->next[0];
        if (node && node->key == key) {
            *value = node->value;
            return true;
        }

        return false;
    }

    void Insert(const std::string& key, const std::string& value) {
        std::vector<Node*> update(max_level_, nullptr);  // prev or equal nodes

        Node* node = head_;
        for (int8_t level = cur_max_level_ - 1; level >= 0; --level) {
            while (node->next[level] && node->next[level]->key < key) {
                node = node->next[level];
            }
            update[level] = node;
        }
        node = node->next[0];
        if (node && node->key == key) {
            node->value = value;
            return;
        }

        const int8_t new_level = RandomLevel();
        if (new_level > cur_max_level_) {
            for (int8_t level = cur_max_level_; level < new_level; ++level) {
                update[level] = head_;
            }
            cur_max_level_ = new_level;
        }

        Node* new_node = new Node(key, value, new_level);
        for (int8_t level = 0; level < new_level; ++level) {
            new_node->next[level]      = update[level]->next[level];
            update[level]->next[level] = new_node;
        }
    }

    bool Erase(const std::string& key) {
        std::vector<Node*> update(max_level_, nullptr);  // prev or equal nodes

        Node* node = head_;
        for (int8_t level = cur_max_level_ - 1; level >= 0; --level) {
            while (node->next[level] && node->next[level]->key < key) {
                node = node->next[level];
            }
            update[level] = node;
        }
        node = node->next[0];
        if (!node || node->key != key) {
            return false;
        }

        for (int8_t level = 0; level < cur_max_level_; ++level) {
            if (update[level]->next[level] != node) {
                break;
            }
            update[level]->next[level] = node->next[level];
        }
        delete node;

        while (cur_max_level_ > 1 && head_->next[cur_max_level_ - 1] == nullptr) {
            cur_max_level_--;
        }
        return true;
    }

    void Print() {
        std::cout << "SkipList:\n";
        for (int8_t level = cur_max_level_ - 1; level >= 0; --level) {
            Node* node = head_->next[level];
            std::cout << "Level " << (int32_t)level << ": ";
            while (node) {
                std::cout << "(" << node->key << ":" << node->value << ") ";
                node = node->next[level];
            }
            std::cout << std::endl;
        }
    }

private:
    // 随机生成层数，越高的层数越少见
    int8_t RandomLevel() {
        int8_t level = 1;
        while (((float)rand() / (float)RAND_MAX) < p_ && level < max_level_) {
            ++level;
        }
        return level;
    }

private:
    Node* head_;
    int8_t cur_max_level_;
    const int8_t max_level_ = 16;

    const float p_ = 0.5;  // 生成新层的概率（通常是 0.5）
};

}  // namespace hyper_cache
