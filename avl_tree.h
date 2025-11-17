//
// Created by Zekun Liu on 2025-09-23.
// Adapted from geeksforgeeks.org
//

#ifndef KVDATABASE_AVL_TREE_H
#define KVDATABASE_AVL_TREE_H

#include <string>
#include <utility>
#include <functional>
#include <limits>

class AVLNode {
public:
    std::string key;
    std::string value;
    AVLNode* left;
    AVLNode* right;
    int height;

    AVLNode(std::string k, std::string v)
        : key(std::move(k))
        , value(std::move(v))
        , left(nullptr)
        , right(nullptr)
        , height(1) {}
};

class AVLTree {
    AVLNode* root;
    int size;
    int max_size; // kept for compatibility with the original constructor, but no longer used for capacity control (capacity is handled by KVDatabase)

    static int  height(const AVLNode* node);
    static int  balanceFactor(const AVLNode* node);
    static AVLNode* rightRotate(AVLNode* y);
    static AVLNode* leftRotate(AVLNode* x);

    static std::pair<AVLNode*, bool> insert(AVLNode* node,
                                            const std::string& key,
                                            const std::string& value);
    static AVLNode* minValueNode(AVLNode* node);
    static std::pair<AVLNode*, bool> deleteNode(AVLNode* node,
                                                const std::string& key);

    static std::string inorder(AVLNode* root);
    static bool search(const AVLNode* root, const std::string& key);

    static const AVLNode* search_node(const AVLNode* root,
                                      const std::string& key);

    static void inorder_scan(AVLNode* node,
                             const std::function<void(const std::string&,
                                                      const std::string&)>& cb);

    static void destroy(AVLNode* node);

public:
    AVLTree()
        : root(nullptr)
        , size(0)
        , max_size(std::numeric_limits<int>::max()) {}

    explicit AVLTree(int max)
        : root(nullptr)
        , size(0)
        , max_size(max) {}

    ~AVLTree();

    // Copying is not allowed, to avoid shallow-copying raw pointers
    AVLTree(const AVLTree&)            = delete;
    AVLTree& operator=(const AVLTree&) = delete;

    // Public API: actual MemTable functionalities
    bool insert(const std::string& key, const std::string& value); // true = new key
    void remove(const std::string& key);

    [[nodiscard]] bool search(const std::string& key) const;
    [[nodiscard]] bool get(const std::string& key, std::string& value_out) const;

    [[nodiscard]] std::string inorder() const;
    [[nodiscard]] int get_size() const;

    // Clear the entire tree (used when memtable is reset after flush)
    void clear();

    // Traverse all (key, value) pairs in ascending key order
    void inorder_scan(const std::function<void(const std::string&,
                                               const std::string&)>& cb) const;
};

#endif //KVDATABASE_AVL_TREE_H
