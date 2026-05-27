#ifndef SYS_PROG_B_TREE_H
#define SYS_PROG_B_TREE_H

#include <initializer_list>
#include <iterator>
#include <stack>
#include <stdexcept>
#include <utility>
#include <vector>

#include <associative_container.h>
#include <pp_allocator.h>

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class B_tree final : private compare
{
    static_assert(t >= 2, "invalid t for b-tree");

public:
    using tree_data_type = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<const tkey, tvalue>;
    using value_type = tree_data_type_const;

private:
    static constexpr size_t minimum_keys_in_node = t - 1;
    static constexpr size_t maximum_keys_in_node = 2 * t - 1;

    struct btree_node
    {
        std::vector<tree_data_type> _keys;
        std::vector<btree_node *> _pointers;

        btree_node()
        {
            _keys.reserve(maximum_keys_in_node + 1);
            _pointers.reserve(maximum_keys_in_node + 2);
        }

        bool is_leaf() const noexcept
        {
            return _pointers.empty();
        }
    };

    using iterator_path = std::stack<std::pair<btree_node **, size_t>>;
    using const_iterator_path = std::stack<std::pair<btree_node *const *, size_t>>;

    struct insert_result
    {
        bool overflow_split;
        tree_data_type promoted_key;
        btree_node *right_node;
    };

    pp_allocator<value_type> _allocator;
    btree_node *_root;
    size_t _size;

    bool key_less(const tkey &lhs, const tkey &rhs) const
    {
        return compare::operator()(lhs, rhs);
    }

    bool key_equal(const tkey &lhs, const tkey &rhs) const
    {
        return !key_less(lhs, rhs) && !key_less(rhs, lhs);
    }

    size_t find_key_index(const btree_node *node, const tkey &key) const
    {
        size_t index = 0;
        while (index < node->_keys.size() && key_less(node->_keys[index].first, key))
        {
            ++index;
        }

        return index;
    }

    size_t find_upper_index(const btree_node *node, const tkey &key) const
    {
        size_t index = 0;
        while (index < node->_keys.size() && !key_less(key, node->_keys[index].first))
        {
            ++index;
        }

        return index;
    }

    static tree_data_type_const &pair_as_value(tree_data_type &value) noexcept
    {
        return reinterpret_cast<tree_data_type_const &>(value);
    }

    static const tree_data_type_const &pair_as_value(const tree_data_type &value) noexcept
    {
        return reinterpret_cast<const tree_data_type_const &>(value);
    }

    btree_node *allocate_node()
    {
        return pp_allocator<btree_node>(_allocator).template new_object<btree_node>();
    }

    void free_node(btree_node *node) noexcept
    {
        if (node == nullptr)
        {
            return;
        }

        pp_allocator<btree_node>(_allocator).delete_object(node);
    }

    void free_subtree(btree_node *node) noexcept
    {
        if (node == nullptr)
        {
            return;
        }

        for (btree_node *child : node->_pointers)
        {
            free_subtree(child);
        }

        free_node(node);
    }

    btree_node *clone_subtree(const btree_node *node)
    {
        if (node == nullptr)
        {
            return nullptr;
        }

        btree_node *clone = allocate_node();
        try
        {
            clone->_keys = node->_keys;
            for (btree_node *child : node->_pointers)
            {
                clone->_pointers.push_back(clone_subtree(child));
            }
        }
        catch (...)
        {
            free_subtree(clone);
            throw;
        }

        return clone;
    }

    template <typename path_type>
    static size_t iterator_depth(const path_type &path) noexcept
    {
        return path.empty() ? 0 : path.size() - 1;
    }

    template <typename path_type>
    static size_t current_node_key_count(const path_type &path) noexcept
    {
        return path.empty() || *path.top().first == nullptr ? 0 : (*path.top().first)->_keys.size();
    }

    template <typename path_type>
    static bool path_points_to_nothing(const path_type &path) noexcept
    {
        return path.empty() || *path.top().first == nullptr;
    }

    template <typename path_type>
    static void descend_to_leftmost_leaf(path_type &path, typename path_type::value_type::first_type slot)
    {
        btree_node *node = *slot;
        while (node != nullptr && !node->is_leaf())
        {
            auto child_slot = &node->_pointers[0];
            path.push({child_slot, 0});
            node = *child_slot;
        }
    }

    template <typename path_type>
    static void descend_to_rightmost_leaf(path_type &path, typename path_type::value_type::first_type slot)
    {
        btree_node *node = *slot;
        while (node != nullptr && !node->is_leaf())
        {
            size_t child_index = node->_pointers.size() - 1;
            auto child_slot = &node->_pointers[child_index];
            path.push({child_slot, child_index});
            node = *child_slot;
        }
    }

    template <typename path_type>
    static void move_iterator_forward(path_type &path, size_t &index)
    {
        if (path.empty())
        {
            return;
        }

        btree_node *node = *path.top().first;
        if (node == nullptr)
        {
            return;
        }

        if (!node->is_leaf())
        {
            auto child_slot = &node->_pointers[index + 1];
            path.push({child_slot, index + 1});
            descend_to_leftmost_leaf(path, child_slot);
            index = 0;
            return;
        }

        if (index + 1 < node->_keys.size())
        {
            ++index;
            return;
        }

        while (!path.empty())
        {
            auto current = path.top();
            path.pop();

            if (path.empty())
            {
                path.push(current);
                index = (*current.first)->_keys.size();
                return;
            }

            btree_node *parent = *path.top().first;
            if (current.second < parent->_keys.size())
            {
                index = current.second;
                return;
            }
        }
    }

    template <typename path_type>
    static void move_iterator_backward(path_type &path, size_t &index)
    {
        if (path.empty())
        {
            return;
        }

        btree_node *node = *path.top().first;
        if (node == nullptr)
        {
            return;
        }

        if (index == node->_keys.size())
        {
            if (!node->is_leaf())
            {
                auto child_slot = &node->_pointers.back();
                path.push({child_slot, node->_pointers.size() - 1});
                descend_to_rightmost_leaf(path, child_slot);
                index = (*path.top().first)->_keys.size() - 1;
            }
            else if (!node->_keys.empty())
            {
                index = node->_keys.size() - 1;
            }
            return;
        }

        if (!node->is_leaf())
        {
            auto child_slot = &node->_pointers[index];
            path.push({child_slot, index});
            descend_to_rightmost_leaf(path, child_slot);
            index = (*path.top().first)->_keys.size() - 1;
            return;
        }

        if (index > 0)
        {
            --index;
            return;
        }

        while (!path.empty())
        {
            auto current = path.top();
            path.pop();

            if (path.empty())
            {
                path.push(current);
                return;
            }

            if (current.second > 0)
            {
                index = current.second - 1;
                return;
            }
        }
    }

    iterator_path build_begin_path() noexcept
    {
        iterator_path path;
        if (_root == nullptr)
        {
            return path;
        }

        path.push({&_root, 0});
        descend_to_leftmost_leaf(path, &_root);
        return path;
    }

    const_iterator_path build_begin_path() const noexcept
    {
        const_iterator_path path;
        if (_root == nullptr)
        {
            return path;
        }

        auto root_slot = reinterpret_cast<btree_node *const *>(&_root);
        path.push({root_slot, 0});
        descend_to_leftmost_leaf(path, root_slot);
        return path;
    }

    iterator_path build_end_path() noexcept
    {
        iterator_path path;
        if (_root == nullptr)
        {
            return path;
        }

        path.push({&_root, 0});
        return path;
    }

    const_iterator_path build_end_path() const noexcept
    {
        const_iterator_path path;
        if (_root == nullptr)
        {
            return path;
        }

        auto root_slot = reinterpret_cast<btree_node *const *>(&_root);
        path.push({root_slot, 0});
        return path;
    }

    insert_result split_overflowed_node(btree_node *node)
    {
        btree_node *right = allocate_node();
        size_t middle_index = t;
        tree_data_type promoted = std::move(node->_keys[middle_index]);

        for (size_t i = middle_index + 1; i < node->_keys.size(); ++i)
        {
            right->_keys.push_back(std::move(node->_keys[i]));
        }

        if (!node->is_leaf())
        {
            for (size_t i = middle_index + 1; i < node->_pointers.size(); ++i)
            {
                right->_pointers.push_back(node->_pointers[i]);
            }
            node->_pointers.resize(middle_index + 1);
        }

        node->_keys.resize(middle_index);
        return {true, std::move(promoted), right};
    }

    insert_result insert_and_split_overflowed_node(btree_node *node, tree_data_type &&data)
    {
        size_t index = find_key_index(node, data.first);

        if (node->is_leaf())
        {
            node->_keys.insert(node->_keys.begin() + static_cast<ptrdiff_t>(index), std::move(data));
        }
        else
        {
            insert_result child_result = insert_and_split_overflowed_node(node->_pointers[index], std::move(data));
            if (child_result.overflow_split)
            {
                node->_keys.insert(node->_keys.begin() + static_cast<ptrdiff_t>(index), std::move(child_result.promoted_key));
                node->_pointers.insert(node->_pointers.begin() + static_cast<ptrdiff_t>(index + 1), child_result.right_node);
            }
        }

        if (node->_keys.size() <= maximum_keys_in_node)
        {
            return {false, tree_data_type(), nullptr};
        }

        return split_overflowed_node(node);
    }

    tree_data_type largest_pair_in_subtree(const btree_node *node) const
    {
        const btree_node *current = node;
        while (!current->is_leaf())
        {
            current = current->_pointers.back();
        }

        return current->_keys.back();
    }

    tree_data_type smallest_pair_in_subtree(const btree_node *node) const
    {
        const btree_node *current = node;
        while (!current->is_leaf())
        {
            current = current->_pointers.front();
        }

        return current->_keys.front();
    }

    void merge_child_nodes(btree_node *parent, size_t left_child_index)
    {
        btree_node *left = parent->_pointers[left_child_index];
        btree_node *right = parent->_pointers[left_child_index + 1];

        left->_keys.push_back(std::move(parent->_keys[left_child_index]));
        for (tree_data_type &item : right->_keys)
        {
            left->_keys.push_back(std::move(item));
        }

        for (btree_node *child : right->_pointers)
        {
            left->_pointers.push_back(child);
        }

        parent->_keys.erase(parent->_keys.begin() + static_cast<ptrdiff_t>(left_child_index));
        parent->_pointers.erase(parent->_pointers.begin() + static_cast<ptrdiff_t>(left_child_index + 1));
        free_node(right);
    }

    void move_key_from_left_sibling(btree_node *parent, size_t child_index)
    {
        btree_node *child = parent->_pointers[child_index];
        btree_node *left = parent->_pointers[child_index - 1];

        child->_keys.insert(child->_keys.begin(), std::move(parent->_keys[child_index - 1]));
        parent->_keys[child_index - 1] = std::move(left->_keys.back());
        left->_keys.pop_back();

        if (!left->is_leaf())
        {
            child->_pointers.insert(child->_pointers.begin(), left->_pointers.back());
            left->_pointers.pop_back();
        }
    }

    void move_key_from_right_sibling(btree_node *parent, size_t child_index)
    {
        btree_node *child = parent->_pointers[child_index];
        btree_node *right = parent->_pointers[child_index + 1];

        child->_keys.push_back(std::move(parent->_keys[child_index]));
        parent->_keys[child_index] = std::move(right->_keys.front());
        right->_keys.erase(right->_keys.begin());

        if (!right->is_leaf())
        {
            child->_pointers.push_back(right->_pointers.front());
            right->_pointers.erase(right->_pointers.begin());
        }
    }

    void prepare_child_for_erase(btree_node *parent, size_t child_index)
    {
        if (child_index > 0 && parent->_pointers[child_index - 1]->_keys.size() > minimum_keys_in_node)
        {
            move_key_from_left_sibling(parent, child_index);
            return;
        }

        if (child_index + 1 < parent->_pointers.size() && parent->_pointers[child_index + 1]->_keys.size() > minimum_keys_in_node)
        {
            move_key_from_right_sibling(parent, child_index);
            return;
        }

        if (child_index + 1 < parent->_pointers.size())
        {
            merge_child_nodes(parent, child_index);
        }
        else
        {
            merge_child_nodes(parent, child_index - 1);
        }
    }

    bool erase_key_from_node(btree_node *node, const tkey &key)
    {
        size_t index = find_key_index(node, key);

        if (index < node->_keys.size() && key_equal(node->_keys[index].first, key))
        {
            if (node->is_leaf())
            {
                node->_keys.erase(node->_keys.begin() + static_cast<ptrdiff_t>(index));
                return true;
            }

            if (node->_pointers[index]->_keys.size() > minimum_keys_in_node)
            {
                node->_keys[index] = largest_pair_in_subtree(node->_pointers[index]);
                return erase_key_from_node(node->_pointers[index], node->_keys[index].first);
            }

            if (node->_pointers[index + 1]->_keys.size() > minimum_keys_in_node)
            {
                node->_keys[index] = smallest_pair_in_subtree(node->_pointers[index + 1]);
                return erase_key_from_node(node->_pointers[index + 1], node->_keys[index].first);
            }

            merge_child_nodes(node, index);
            return erase_key_from_node(node->_pointers[index], key);
        }

        if (node->is_leaf())
        {
            return false;
        }

        if (node->_pointers[index]->_keys.size() == minimum_keys_in_node)
        {
            prepare_child_for_erase(node, index);
            if (index > node->_keys.size())
            {
                --index;
            }
        }

        return erase_key_from_node(node->_pointers[index], key);
    }

public:
    class btree_iterator;
    class btree_reverse_iterator;
    class btree_const_iterator;
    class btree_const_reverse_iterator;

    class btree_iterator final
    {
        iterator_path _path;
        size_t _index;

    public:
        using value_type = tree_data_type_const;
        using reference = value_type &;
        using pointer = value_type *;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_iterator;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_const_iterator;
        friend class btree_const_reverse_iterator;

        explicit btree_iterator(const iterator_path &path = iterator_path(), size_t index = 0)
            : _path(path), _index(index)
        {
        }

        reference operator*() const noexcept
        {
            return pair_as_value((*_path.top().first)->_keys[_index]);
        }

        pointer operator->() const noexcept
        {
            return &pair_as_value((*_path.top().first)->_keys[_index]);
        }

        self &operator++()
        {
            move_iterator_forward(_path, _index);
            return *this;
        }

        self operator++(int)
        {
            self copy(*this);
            ++(*this);
            return copy;
        }

        self &operator--()
        {
            move_iterator_backward(_path, _index);
            return *this;
        }

        self operator--(int)
        {
            self copy(*this);
            --(*this);
            return copy;
        }

        bool operator==(const self &other) const noexcept
        {
            return _index == other._index && _path == other._path;
        }

        bool operator!=(const self &other) const noexcept
        {
            return !(*this == other);
        }

        size_t depth() const noexcept
        {
            return iterator_depth(_path);
        }

        size_t current_node_keys_count() const noexcept
        {
            return current_node_key_count(_path);
        }

        bool is_terminate_node() const noexcept
        {
            return path_points_to_nothing(_path);
        }

        size_t index() const noexcept
        {
            return _index;
        }
    };

    class btree_const_iterator final
    {
        const_iterator_path _path;
        size_t _index;

    public:
        using value_type = tree_data_type_const;
        using reference = const value_type &;
        using pointer = const value_type *;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_const_iterator;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_iterator;
        friend class btree_const_reverse_iterator;

        explicit btree_const_iterator(const const_iterator_path &path = const_iterator_path(), size_t index = 0)
            : _path(path), _index(index)
        {
        }

        btree_const_iterator(const btree_iterator &other) noexcept
            : _index(other._index)
        {
            std::vector<std::pair<btree_node **, size_t>> items;
            iterator_path path = other._path;
            while (!path.empty())
            {
                items.push_back(path.top());
                path.pop();
            }

            for (auto it = items.rbegin(); it != items.rend(); ++it)
            {
                _path.push({reinterpret_cast<btree_node *const *>(it->first), it->second});
            }
        }

        reference operator*() const noexcept
        {
            return pair_as_value((*_path.top().first)->_keys[_index]);
        }

        pointer operator->() const noexcept
        {
            return &pair_as_value((*_path.top().first)->_keys[_index]);
        }

        self &operator++()
        {
            move_iterator_forward(_path, _index);
            return *this;
        }

        self operator++(int)
        {
            self copy(*this);
            ++(*this);
            return copy;
        }

        self &operator--()
        {
            move_iterator_backward(_path, _index);
            return *this;
        }

        self operator--(int)
        {
            self copy(*this);
            --(*this);
            return copy;
        }

        bool operator==(const self &other) const noexcept
        {
            return _index == other._index && _path == other._path;
        }

        bool operator!=(const self &other) const noexcept
        {
            return !(*this == other);
        }

        size_t depth() const noexcept
        {
            return iterator_depth(_path);
        }

        size_t current_node_keys_count() const noexcept
        {
            return current_node_key_count(_path);
        }

        bool is_terminate_node() const noexcept
        {
            return path_points_to_nothing(_path);
        }

        size_t index() const noexcept
        {
            return _index;
        }
    };

    class btree_reverse_iterator final
    {
        btree_iterator _base;

    public:
        using value_type = tree_data_type_const;
        using reference = value_type &;
        using pointer = value_type *;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_reverse_iterator;

        friend class B_tree;
        friend class btree_iterator;
        friend class btree_const_iterator;
        friend class btree_const_reverse_iterator;

        explicit btree_reverse_iterator(const iterator_path &path = iterator_path(), size_t index = 0)
            : _base(path, index)
        {
        }

        btree_reverse_iterator(const btree_iterator &base) noexcept
            : _base(base)
        {
        }

        operator btree_iterator() const noexcept
        {
            return _base;
        }

        reference operator*() const noexcept
        {
            auto it = _base;
            --it;
            return *it;
        }

        pointer operator->() const noexcept
        {
            auto it = _base;
            --it;
            return it.operator->();
        }

        self &operator++()
        {
            --_base;
            return *this;
        }

        self operator++(int)
        {
            self copy(*this);
            ++(*this);
            return copy;
        }

        self &operator--()
        {
            ++_base;
            return *this;
        }

        self operator--(int)
        {
            self copy(*this);
            --(*this);
            return copy;
        }

        bool operator==(const self &other) const noexcept
        {
            return _base == other._base;
        }

        bool operator!=(const self &other) const noexcept
        {
            return !(*this == other);
        }

        size_t depth() const noexcept
        {
            auto it = _base;
            --it;
            return it.depth();
        }

        size_t current_node_keys_count() const noexcept
        {
            auto it = _base;
            --it;
            return it.current_node_keys_count();
        }

        bool is_terminate_node() const noexcept
        {
            return _base.is_terminate_node();
        }

        size_t index() const noexcept
        {
            auto it = _base;
            --it;
            return it.index();
        }
    };

    class btree_const_reverse_iterator final
    {
        btree_const_iterator _base;

    public:
        using value_type = tree_data_type_const;
        using reference = const value_type &;
        using pointer = const value_type *;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_const_reverse_iterator;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_const_iterator;
        friend class btree_iterator;

        explicit btree_const_reverse_iterator(const const_iterator_path &path = const_iterator_path(), size_t index = 0)
            : _base(path, index)
        {
        }

        btree_const_reverse_iterator(const btree_reverse_iterator &base) noexcept
            : _base(static_cast<btree_iterator>(base))
        {
        }

        operator btree_const_iterator() const noexcept
        {
            return _base;
        }

        reference operator*() const noexcept
        {
            auto it = _base;
            --it;
            return *it;
        }

        pointer operator->() const noexcept
        {
            auto it = _base;
            --it;
            return it.operator->();
        }

        self &operator++()
        {
            --_base;
            return *this;
        }

        self operator++(int)
        {
            self copy(*this);
            ++(*this);
            return copy;
        }

        self &operator--()
        {
            ++_base;
            return *this;
        }

        self operator--(int)
        {
            self copy(*this);
            --(*this);
            return copy;
        }

        bool operator==(const self &other) const noexcept
        {
            return _base == other._base;
        }

        bool operator!=(const self &other) const noexcept
        {
            return !(*this == other);
        }

        size_t depth() const noexcept
        {
            auto it = _base;
            --it;
            return it.depth();
        }

        size_t current_node_keys_count() const noexcept
        {
            auto it = _base;
            --it;
            return it.current_node_keys_count();
        }

        bool is_terminate_node() const noexcept
        {
            return _base.is_terminate_node();
        }

        size_t index() const noexcept
        {
            auto it = _base;
            --it;
            return it.index();
        }
    };

    explicit B_tree(const compare &cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>())
        : compare(cmp), _allocator(alloc), _root(nullptr), _size(0)
    {
    }

    explicit B_tree(pp_allocator<value_type> alloc, const compare &comp = compare())
        : B_tree(comp, alloc)
    {
    }

    template <input_iterator_for_pair<tkey, tvalue> iterator>
    explicit B_tree(iterator begin, iterator end, const compare &cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>())
        : B_tree(cmp, alloc)
    {
        for (; begin != end; ++begin)
        {
            emplace(begin->first, begin->second);
        }
    }

    B_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare &cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>())
        : B_tree(cmp, alloc)
    {
        for (const auto &item : data)
        {
            emplace(item.first, item.second);
        }
    }

    B_tree(const B_tree &other)
        : compare(static_cast<const compare &>(other)),
          _allocator(other._allocator.select_on_container_copy_construction()),
          _root(nullptr),
          _size(other._size)
    {
        _root = clone_subtree(other._root);
    }

    B_tree(B_tree &&other) noexcept
        : compare(std::move(static_cast<compare &>(other))),
          _allocator(other._allocator),
          _root(other._root),
          _size(other._size)
    {
        other._root = nullptr;
        other._size = 0;
    }

    B_tree &operator=(const B_tree &other)
    {
        if (this == &other)
        {
            return *this;
        }

        B_tree copy(other);
        *this = std::move(copy);
        return *this;
    }

    B_tree &operator=(B_tree &&other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        clear();
        static_cast<compare &>(*this) = std::move(static_cast<compare &>(other));
        _allocator = other._allocator;
        _root = other._root;
        _size = other._size;
        other._root = nullptr;
        other._size = 0;
        return *this;
    }

    ~B_tree() noexcept
    {
        clear();
    }

    tvalue &at(const tkey &key)
    {
        auto it = find(key);
        if (it == end())
        {
            throw std::out_of_range("key not found");
        }
        return it->second;
    }

    const tvalue &at(const tkey &key) const
    {
        auto it = find(key);
        if (it == end())
        {
            throw std::out_of_range("key not found");
        }
        return it->second;
    }

    tvalue &operator[](const tkey &key)
    {
        return emplace(key, tvalue()).first->second;
    }

    tvalue &operator[](tkey &&key)
    {
        return emplace(std::move(key), tvalue()).first->second;
    }

    btree_iterator begin()
    {
        if (_root == nullptr)
        {
            return end();
        }
        return btree_iterator(build_begin_path(), 0);
    }

    btree_iterator end()
    {
        if (_root == nullptr)
        {
            return btree_iterator();
        }

        auto path = build_end_path();
        return btree_iterator(path, (*path.top().first)->_keys.size());
    }

    btree_const_iterator begin() const
    {
        return cbegin();
    }

    btree_const_iterator end() const
    {
        return cend();
    }

    btree_const_iterator cbegin() const
    {
        if (_root == nullptr)
        {
            return cend();
        }
        return btree_const_iterator(build_begin_path(), 0);
    }

    btree_const_iterator cend() const
    {
        if (_root == nullptr)
        {
            return btree_const_iterator();
        }

        auto path = build_end_path();
        return btree_const_iterator(path, (*path.top().first)->_keys.size());
    }

    btree_reverse_iterator rbegin()
    {
        return btree_reverse_iterator(end());
    }

    btree_reverse_iterator rend()
    {
        return btree_reverse_iterator(begin());
    }

    btree_const_reverse_iterator rbegin() const
    {
        return btree_const_reverse_iterator(end());
    }

    btree_const_reverse_iterator rend() const
    {
        return btree_const_reverse_iterator(begin());
    }

    btree_const_reverse_iterator crbegin() const
    {
        return rbegin();
    }

    btree_const_reverse_iterator crend() const
    {
        return rend();
    }

    size_t size() const noexcept
    {
        return _size;
    }

    bool empty() const noexcept
    {
        return _size == 0;
    }

    btree_iterator find(const tkey &key)
    {
        if (_root == nullptr)
        {
            return end();
        }

        iterator_path path;
        btree_node **slot = &_root;
        path.push({slot, 0});

        while (*slot != nullptr)
        {
            btree_node *node = *slot;
            size_t index = find_key_index(node, key);
            if (index < node->_keys.size() && key_equal(node->_keys[index].first, key))
            {
                return btree_iterator(path, index);
            }

            if (node->is_leaf())
            {
                return end();
            }

            slot = &node->_pointers[index];
            path.push({slot, index});
        }

        return end();
    }

    btree_const_iterator find(const tkey &key) const
    {
        if (_root == nullptr)
        {
            return end();
        }

        const_iterator_path path;
        auto slot = reinterpret_cast<btree_node *const *>(&_root);
        path.push({slot, 0});

        while (*slot != nullptr)
        {
            btree_node *node = *slot;
            size_t index = find_key_index(node, key);
            if (index < node->_keys.size() && key_equal(node->_keys[index].first, key))
            {
                return btree_const_iterator(path, index);
            }

            if (node->is_leaf())
            {
                return end();
            }

            slot = &node->_pointers[index];
            path.push({slot, index});
        }

        return end();
    }

    btree_iterator lower_bound(const tkey &key)
    {
        for (auto it = begin(), last = end(); it != last; ++it)
        {
            if (!key_less(it->first, key))
            {
                return it;
            }
        }
        return end();
    }

    btree_const_iterator lower_bound(const tkey &key) const
    {
        for (auto it = cbegin(), last = cend(); it != last; ++it)
        {
            if (!key_less(it->first, key))
            {
                return it;
            }
        }
        return cend();
    }

    btree_iterator upper_bound(const tkey &key)
    {
        for (auto it = begin(), last = end(); it != last; ++it)
        {
            if (key_less(key, it->first))
            {
                return it;
            }
        }
        return end();
    }

    btree_const_iterator upper_bound(const tkey &key) const
    {
        for (auto it = cbegin(), last = cend(); it != last; ++it)
        {
            if (key_less(key, it->first))
            {
                return it;
            }
        }
        return cend();
    }

    bool contains(const tkey &key) const
    {
        return find(key) != end();
    }

    void clear() noexcept
    {
        free_subtree(_root);
        _root = nullptr;
        _size = 0;
    }

    std::pair<btree_iterator, bool> insert(const tree_data_type &data)
    {
        return emplace(data.first, data.second);
    }

    std::pair<btree_iterator, bool> insert(tree_data_type &&data)
    {
        return emplace(std::move(data.first), std::move(data.second));
    }

    template <typename... Args>
    std::pair<btree_iterator, bool> emplace(Args &&...args)
    {
        tree_data_type data(std::forward<Args>(args)...);
        tkey inserted_key = data.first;

        if (auto it = find(inserted_key); it != end())
        {
            return {it, false};
        }

        if (_root == nullptr)
        {
            _root = allocate_node();
            _root->_keys.push_back(std::move(data));
            ++_size;
            return {begin(), true};
        }

        insert_result result = insert_and_split_overflowed_node(_root, std::move(data));
        if (result.overflow_split)
        {
            btree_node *new_root = allocate_node();
            new_root->_keys.push_back(std::move(result.promoted_key));
            new_root->_pointers.push_back(_root);
            new_root->_pointers.push_back(result.right_node);
            _root = new_root;
        }

        ++_size;
        return {find(inserted_key), true};
    }

    btree_iterator insert_or_assign(const tree_data_type &data)
    {
        return emplace_or_assign(data.first, data.second);
    }

    btree_iterator insert_or_assign(tree_data_type &&data)
    {
        return emplace_or_assign(std::move(data.first), std::move(data.second));
    }

    template <typename... Args>
    btree_iterator emplace_or_assign(Args &&...args)
    {
        tree_data_type data(std::forward<Args>(args)...);
        if (auto it = find(data.first); it != end())
        {
            it->second = std::move(data.second);
            return it;
        }
        return emplace(std::move(data)).first;
    }

    btree_iterator erase(btree_iterator pos)
    {
        if (pos == end())
        {
            return end();
        }

        tkey key = pos->first;
        ++pos;
        erase(key);
        if (pos == end())
        {
            return end();
        }
        return find(pos->first);
    }

    btree_iterator erase(btree_const_iterator pos)
    {
        if (pos == cend())
        {
            return end();
        }

        tkey key = pos->first;
        auto next = pos;
        ++next;
        erase(key);
        if (next == cend())
        {
            return end();
        }
        return find(next->first);
    }

    btree_iterator erase(btree_iterator beg, btree_iterator en)
    {
        while (beg != en)
        {
            beg = erase(beg);
        }
        return en;
    }

    btree_iterator erase(btree_const_iterator beg, btree_const_iterator en)
    {
        while (beg != en)
        {
            beg = btree_const_iterator(erase(beg));
        }

        if (en == cend())
        {
            return end();
        }
        return find(en->first);
    }

    btree_iterator erase(const tkey &key)
    {
        auto current = find(key);
        if (_root == nullptr || current == end())
        {
            return end();
        }

        auto next = current;
        ++next;

        if (!erase_key_from_node(_root, key))
        {
            return end();
        }

        --_size;
        if (_root->_keys.empty())
        {
            btree_node *old_root = _root;
            if (_root->is_leaf())
            {
                _root = nullptr;
            }
            else
            {
                _root = _root->_pointers.front();
                old_root->_pointers.clear();
            }
            free_node(old_root);
        }

        if (next == end())
        {
            return end();
        }
        return find(next->first);
    }
};

template <std::input_iterator iterator, comparator<typename std::iterator_traits<iterator>::value_type::first_type> compare = std::less<typename std::iterator_traits<iterator>::value_type::first_type>,
          std::size_t t = 5, typename U>
B_tree(iterator begin, iterator end, const compare &cmp = compare(), pp_allocator<U> = pp_allocator<U>())
    -> B_tree<typename std::iterator_traits<iterator>::value_type::first_type, typename std::iterator_traits<iterator>::value_type::second_type, compare, t>;

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5, typename U>
B_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare &cmp = compare(), pp_allocator<U> = pp_allocator<U>())
    -> B_tree<tkey, tvalue, compare, t>;

#endif