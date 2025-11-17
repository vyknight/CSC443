//
// Created by Zekun Liu on 2025-09-23.
// Adapted from geeksforgeeks.org
//

#include "avl_tree.h"
#include <algorithm>

// ----------------- Helper functions -----------------

int AVLTree::height(const AVLNode* node) {
    if (node == nullptr) return 0;
    return node->height;
}

int AVLTree::balanceFactor(const AVLNode* node) {
    if (node == nullptr) return 0;
    return height(node->left) - height(node->right);
}

AVLNode* AVLTree::rightRotate(AVLNode* y) {
    AVLNode* x  = y->left;
    AVLNode* t2 = x->right;

    x->right = y;
    y->left  = t2;

    y->height = std::max(height(y->left), height(y->right)) + 1;
    x->height = std::max(height(x->left), height(x->right)) + 1;

    return x;
}

AVLNode* AVLTree::leftRotate(AVLNode* x) {
    AVLNode* y  = x->right;
    AVLNode* t2 = y->left;

    y->left = x;
    x->right = t2;

    x->height = std::max(height(x->left), height(x->right)) + 1;
    y->height = std::max(height(y->left), height(y->right)) + 1;

    return y;
}

std::pair<AVLNode*, bool> AVLTree::insert(AVLNode* node,
                                          const std::string& key,
                                          const std::string& value) {
    if (node == nullptr) {
        auto* new_leaf = new AVLNode(key, value);
        return {new_leaf, true};
    }

    bool inserted;
    if (key < node->key) {
        auto res = insert(node->left, key, value);
        node->left = res.first;
        inserted   = res.second;
    } else if (key > node->key) {
        auto res = insert(node->right, key, value);
        node->right = res.first;
        inserted    = res.second;
    } else {
        // Overwrite existing value
        node->value = value;
        return {node, false};
    }

    // Update height and rotate when needed
    node->height = 1 + std::max(height(node->left), height(node->right));
    int balance  = balanceFactor(node);

    // LL
    if (balance > 1 && key < node->left->key)
        return {rightRotate(node), inserted};

    // RR
    if (balance < -1 && key > node->right->key)
        return {leftRotate(node), inserted};

    // LR
    if (balance > 1 && key > node->left->key) {
        node->left = leftRotate(node->left);
        return {rightRotate(node), inserted};
    }

    // RL
    if (balance < -1 && key < node->right->key) {
        node->right = rightRotate(node->right);
        return {leftRotate(node), inserted};
    }

    return {node, inserted};
}

AVLNode* AVLTree::minValueNode(AVLNode* node) {
    AVLNode* current = node;
    while (current && current->left != nullptr)
        current = current->left;
    return current;
}

std::pair<AVLNode*, bool> AVLTree::deleteNode(AVLNode* root,
                                              const std::string& key) {
    if (root == nullptr) return {nullptr, false};

    bool deleted = false;

    if (key < root->key) {
        auto res = deleteNode(root->left, key);
        root->left = res.first;
        deleted    = res.second;
    } else if (key > root->key) {
        auto res = deleteNode(root->right, key);
        root->right = res.first;
        deleted     = res.second;
    } else {
        // Target node found
        deleted = true;

        // At most one child
        if (root->left == nullptr || root->right == nullptr) {
            AVLNode* child = root->left ? root->left : root->right;
            delete root;
            root = child;
        } else {
            // Two children: replace with inorder successor
            AVLNode* succ = minValueNode(root->right);
            root->key   = succ->key;
            root->value = succ->value;
            auto res = deleteNode(root->right, succ->key);
            root->right = res.first;
        }
    }

    if (root == nullptr) return {root, deleted};

    // Update height and rotate during unwinding
    root->height = 1 + std::max(height(root->left), height(root->right));
    int balance  = balanceFactor(root);

    // LL
    if (balance > 1 && balanceFactor(root->left) >= 0)
        return {rightRotate(root), deleted};

    // LR
    if (balance > 1 && balanceFactor(root->left) < 0) {
        root->left = leftRotate(root->left);
        return {rightRotate(root), deleted};
    }

    // RR
    if (balance < -1 && balanceFactor(root->right) <= 0)
        return {leftRotate(root), deleted};

    // RL
    if (balance < -1 && balanceFactor(root->right) > 0) {
        root->right = rightRotate(root->right);
        return {leftRotate(root), deleted};
    }

    return {root, deleted};
}

std::string AVLTree::inorder(AVLNode* root) {
    if (root != nullptr) {
        const std::string left  = inorder(root->left);
        const std::string right = inorder(root->right);
        return left + " (" + root->key + ", " + root->value + ") " + right;
    }
    return "";
}

bool AVLTree::search(const AVLNode* root, const std::string& key) {
    if (root == nullptr) return false;
    if (root->key == key) return true;
    if (key < root->key)   return search(root->left, key);
    return search(root->right, key);
}

const AVLNode* AVLTree::search_node(const AVLNode* root,
                                    const std::string& key) {
    if (root == nullptr) return nullptr;
    if (root->key == key) return root;
    if (key < root->key)  return search_node(root->left, key);
    return search_node(root->right, key);
}

void AVLTree::inorder_scan(AVLNode* node,
                           const std::function<void(const std::string&,
                                                    const std::string&)>& cb) {
    if (!node) return;
    inorder_scan(node->left, cb);
    cb(node->key, node->value);
    inorder_scan(node->right, cb);
}

// ----------------- Public Interface -----------------

bool AVLTree::insert(const std::string& key, const std::string& value) {
    auto res = insert(root, key, value);
    root     = res.first;
    if (res.second) {
        ++size;
    }
    return res.second;  // true = new key inserted, false = existing key overwritten
}

void AVLTree::remove(const std::string& key) {
    auto res = deleteNode(root, key);
    root     = res.first;
    if (size > 0 && res.second) {
        --size;
    }
}

bool AVLTree::search(const std::string& key) const {
    return search(root, key);
}

bool AVLTree::get(const std::string& key, std::string& value_out) const {
    const AVLNode* p = search_node(root, key);
    if (!p) return false;
    value_out = p->value;
    return true;
}

std::string AVLTree::inorder() const {
    return inorder(root);
}

int AVLTree::get_size() const {
    return size;
}

void AVLTree::destroy(AVLNode* node) {
    if (!node) return;
    destroy(node->left);
    destroy(node->right);
    delete node;
}

void AVLTree::clear() {
    destroy(root);
    root = nullptr;
    size = 0;
}

void AVLTree::inorder_scan(
    const std::function<void(const std::string&, const std::string&)>& cb
) const {
    inorder_scan(root, cb);
}

AVLTree::~AVLTree() {
    destroy(root);
}
