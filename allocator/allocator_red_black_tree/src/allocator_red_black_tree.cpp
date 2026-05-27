#include "../include/allocator_red_black_tree.h"

#include <memory_resource>
#include <new>
#include <stdexcept>

auto allocator_red_black_tree::get_parent_alloc() const
{
    return static_cast<memory_resource**>(_trusted_memory);
}

auto allocator_red_black_tree::get_fit_mode() const
{
    return reinterpret_cast<fit_mode*>(static_cast<char*>(_trusted_memory) + sizeof(memory_resource*));
}

auto allocator_red_black_tree::get_full_size() const
{
    return reinterpret_cast<size_t*>(static_cast<char*>(_trusted_memory) + sizeof(memory_resource*) + sizeof(fit_mode));
}

auto allocator_red_black_tree::get_mutex() const
{
    return reinterpret_cast<std::mutex*>(static_cast<char*>(_trusted_memory) + sizeof(memory_resource*) + sizeof(fit_mode) + sizeof(size_t));
}

auto allocator_red_black_tree::get_root() const
{
    return reinterpret_cast<void**>(static_cast<char*>(_trusted_memory) + sizeof(memory_resource*) + sizeof(fit_mode) + sizeof(size_t) + sizeof(std::mutex));
}

auto allocator_red_black_tree::get_memory_start() const
{
    return static_cast<char*>(_trusted_memory) + allocator_metadata_size;
}

auto allocator_red_black_tree::get_memory_end() const
{
    return static_cast<char*>(_trusted_memory) + *get_full_size();
}

auto allocator_red_black_tree::get_metadata(void* block)
{
    return static_cast<block_data*>(block);
}

auto allocator_red_black_tree::get_size_ptr(void* block)
{
    return reinterpret_cast<size_t*>(static_cast<char*>(block) + sizeof(block_data));
}

auto allocator_red_black_tree::get_prev_block(void* block)
{
    return reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(block_data) + sizeof(size_t));
}

auto allocator_red_black_tree::get_parent_node(void* block)
{
    return reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(block_data) + sizeof(size_t) + sizeof(void*));
}

auto allocator_red_black_tree::get_left_node(void* block)
{
    return reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(block_data) + sizeof(size_t) + 2 * sizeof(void*));
}

auto allocator_red_black_tree::get_right_node(void* block)
{
    return reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(block_data) + sizeof(size_t) + 3 * sizeof(void*));
}

auto allocator_red_black_tree::get_block_size(void* block)
{
    return *get_size_ptr(block);
}

auto allocator_red_black_tree::is_block_free(void* block)
{
    return !get_metadata(block)->occupied;
}

void allocator_red_black_tree::set_prev_block(void* block, void* prev)
{
    *get_prev_block(block) = prev;
}

void allocator_red_black_tree::set_color(void* block, block_color color)
{
    get_metadata(block)->color = color;
}

auto allocator_red_black_tree::get_color(void* block)
{
    return get_metadata(block)->color;
}

void allocator_red_black_tree::insert_free_block(void* block) const
{
    get_metadata(block)->occupied = false;
    set_color(block, block_color::BLACK);
    *get_parent_node(block) = nullptr;
    *get_left_node(block) = nullptr;
    *get_right_node(block) = nullptr;

    void** link = get_root();
    void* parent = nullptr;
    while (*link != nullptr)
    {
        parent = *link;
        bool goes_left = get_block_size(block) < get_block_size(parent)
            || (get_block_size(block) == get_block_size(parent) && block < parent);
        link = goes_left ? get_left_node(parent) : get_right_node(parent);
    }

    *link = block;
    *get_parent_node(block) = parent;
}

auto allocator_red_black_tree::minimum(void* block)
{
    while (block != nullptr && *get_left_node(block) != nullptr)
    {
        block = *get_left_node(block);
    }
    return block;
}

void allocator_red_black_tree::remove_free_block(void* block) const
{
    auto replace_link = [&](void* node) -> void** {
        void* parent = *get_parent_node(node);
        if (parent == nullptr)
        {
            return get_root();
        }
        return *get_left_node(parent) == node ? get_left_node(parent) : get_right_node(parent);
    };

    auto transplant = [&](void* target, void* replacement) {
        *replace_link(target) = replacement;
        if (replacement != nullptr)
        {
            *get_parent_node(replacement) = *get_parent_node(target);
        }
    };

    if (*get_left_node(block) == nullptr)
    {
        transplant(block, *get_right_node(block));
    }
    else if (*get_right_node(block) == nullptr)
    {
        transplant(block, *get_left_node(block));
    }
    else
    {
        void* successor = minimum(*get_right_node(block));
        if (*get_parent_node(successor) != block)
        {
            transplant(successor, *get_right_node(successor));
            *get_right_node(successor) = *get_right_node(block);
            *get_parent_node(*get_right_node(successor)) = successor;
        }

        transplant(block, successor);
        *get_left_node(successor) = *get_left_node(block);
        *get_parent_node(*get_left_node(successor)) = successor;
    }

    *get_parent_node(block) = nullptr;
    *get_left_node(block) = nullptr;
    *get_right_node(block) = nullptr;
}

void allocator_red_black_tree::collect_suitable_block(void* block, size_t needed, block_choice& choice) const
{
    if (block == nullptr)
    {
        return;
    }

    collect_suitable_block(*get_left_node(block), needed, choice);

    size_t block_size = get_block_size(block);
    if (block_size >= needed)
    {
        if (choice.block == nullptr
            || (*get_fit_mode() == fit_mode::first_fit && block_size < choice.size)
            || (*get_fit_mode() == fit_mode::the_best_fit && block_size < choice.size)
            || (*get_fit_mode() == fit_mode::the_worst_fit && block_size > choice.size))
        {
            choice = {block, block_size};
        }
    }

    collect_suitable_block(*get_right_node(block), needed, choice);
}

allocator_red_black_tree::block_choice allocator_red_black_tree::find_block(size_t needed) const
{
    block_choice choice{nullptr, 0};
    collect_suitable_block(*get_root(), needed, choice);
    return choice;
}

allocator_red_black_tree::~allocator_red_black_tree()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    get_mutex()->~mutex();
    if (auto* parent = *get_parent_alloc(); parent != nullptr)
    {
        parent->deallocate(_trusted_memory, *get_full_size());
    }
    else
    {
        ::operator delete(_trusted_memory);
    }
}

allocator_red_black_tree::allocator_red_black_tree(
    size_t space_size,
    std::pmr::memory_resource* parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size < free_block_metadata_size)
    {
        throw std::logic_error("space size is too small");
    }

    size_t total_size = allocator_metadata_size + space_size;
    _trusted_memory = parent_allocator != nullptr
        ? parent_allocator->allocate(total_size)
        : ::operator new(total_size);

    *get_parent_alloc() = parent_allocator;
    *get_fit_mode() = allocate_fit_mode;
    *get_full_size() = total_size;
    new (get_mutex()) std::mutex();
    *get_root() = nullptr;

    void* first_block = get_memory_start();
    get_metadata(first_block)->occupied = false;
    set_color(first_block, block_color::BLACK);
    *get_size_ptr(first_block) = space_size;
    *get_prev_block(first_block) = nullptr;
    insert_free_block(first_block);
}

[[nodiscard]] void* allocator_red_black_tree::do_allocate_sm(size_t size)
{
    std::lock_guard lock(*get_mutex());

    size_t needed = size + occupied_block_metadata_size;
    block_choice choice = find_block(needed);
    if (choice.block == nullptr)
    {
        throw std::bad_alloc();
    }

    void* prev_block = *get_prev_block(choice.block);
    remove_free_block(choice.block);

    size_t remainder = choice.size - needed;
    if (remainder >= free_block_metadata_size)
    {
        void* free_block = static_cast<char*>(choice.block) + needed;
        get_metadata(choice.block)->occupied = true;
        *get_size_ptr(choice.block) = needed;
        set_prev_block(choice.block, prev_block);

        get_metadata(free_block)->occupied = false;
        set_color(free_block, block_color::BLACK);
        *get_size_ptr(free_block) = remainder;
        set_prev_block(free_block, choice.block);
        *get_parent_node(free_block) = nullptr;
        *get_left_node(free_block) = nullptr;
        *get_right_node(free_block) = nullptr;

        char* next = static_cast<char*>(free_block) + remainder;
        if (next < get_memory_end())
        {
            set_prev_block(next, free_block);
        }

        insert_free_block(free_block);
    }
    else
    {
        get_metadata(choice.block)->occupied = true;
        *get_size_ptr(choice.block) = choice.size;
        set_prev_block(choice.block, prev_block);
    }

    return static_cast<char*>(choice.block) + occupied_block_metadata_size;
}

void allocator_red_black_tree::do_deallocate_sm(void* at)
{
    std::lock_guard lock(*get_mutex());

    if (at == nullptr)
    {
        return;
    }

    char* data = static_cast<char*>(at);
    if (data < get_memory_start() || data >= get_memory_end())
    {
        throw std::invalid_argument("block bounds are invalid");
    }

    void* block = data - occupied_block_metadata_size;
    if (is_block_free(block))
    {
        throw std::invalid_argument("block is free");
    }

    get_metadata(block)->occupied = false;
    if (void* prev = *get_prev_block(block); prev != nullptr && is_block_free(prev))
    {
        remove_free_block(prev);
        *get_size_ptr(prev) = get_block_size(prev) + get_block_size(block);
        block = prev;
    }

    char* next = static_cast<char*>(block) + get_block_size(block);
    if (next < get_memory_end() && is_block_free(next))
    {
        remove_free_block(next);
        *get_size_ptr(block) = get_block_size(block) + get_block_size(next);
    }

    next = static_cast<char*>(block) + get_block_size(block);
    if (next < get_memory_end())
    {
        set_prev_block(next, block);
    }

    insert_free_block(block);
}

bool allocator_red_black_tree::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    if (this == &other)
    {
        return true;
    }

    auto* rhs = dynamic_cast<const allocator_red_black_tree*>(&other);
    return rhs != nullptr && rhs->_trusted_memory == _trusted_memory;
}

void allocator_red_black_tree::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard lock(*get_mutex());
    *get_fit_mode() = mode;
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info() const
{
    std::lock_guard lock(*get_mutex());
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info_inner() const
{
    std::vector<block_info> result;

    for (char* block = get_memory_start(); block < get_memory_end(); block += get_block_size(block))
    {
        result.push_back({get_block_size(block), !is_block_free(block)});
    }

    return result;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::begin() const noexcept
{
    return {const_cast<allocator_red_black_tree*>(this)};
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::end() const noexcept
{
    return {};
}

bool allocator_red_black_tree::rb_iterator::operator==(const allocator_red_black_tree::rb_iterator& other) const noexcept
{
    return _block_ptr == other._block_ptr && _trusted == other._trusted;
}

bool allocator_red_black_tree::rb_iterator::operator!=(const allocator_red_black_tree::rb_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_red_black_tree::rb_iterator& allocator_red_black_tree::rb_iterator::operator++() & noexcept
{
    if (_block_ptr == nullptr || _trusted == nullptr)
    {
        return *this;
    }

    auto* allocator = static_cast<allocator_red_black_tree*>(_trusted);
    _block_ptr = static_cast<char*>(_block_ptr) + allocator_red_black_tree::get_block_size(_block_ptr);
    if (_block_ptr >= allocator->get_memory_end())
    {
        _block_ptr = nullptr;
    }

    return *this;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::rb_iterator::operator++(int)
{
    auto copy = *this;
    ++(*this);
    return copy;
}

size_t allocator_red_black_tree::rb_iterator::size() const noexcept
{
    return _block_ptr == nullptr ? 0 : allocator_red_black_tree::get_block_size(_block_ptr);
}

void* allocator_red_black_tree::rb_iterator::operator*() const noexcept
{
    return occupied() ? static_cast<char*>(_block_ptr) + occupied_block_metadata_size : nullptr;
}

allocator_red_black_tree::rb_iterator::rb_iterator()
    : _block_ptr(nullptr), _trusted(nullptr)
{
}

allocator_red_black_tree::rb_iterator::rb_iterator(void* trusted)
    : _block_ptr(nullptr), _trusted(trusted)
{
    if (_trusted == nullptr)
    {
        return;
    }

    auto* allocator = static_cast<allocator_red_black_tree*>(_trusted);
    if (allocator->get_memory_start() < allocator->get_memory_end())
    {
        _block_ptr = allocator->get_memory_start();
    }
}

bool allocator_red_black_tree::rb_iterator::occupied() const noexcept
{
    return _block_ptr != nullptr && !allocator_red_black_tree::is_block_free(_block_ptr);
}
