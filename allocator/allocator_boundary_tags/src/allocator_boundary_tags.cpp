#include "../include/allocator_boundary_tags.h"

#include "allocator_with_fit_mode.h"

#include <memory_resource>
#include <new>
#include <stdexcept>

auto allocator_boundary_tags::get_parent_alloc() const
{
    return static_cast<memory_resource**>(_trusted_memory);
}

auto allocator_boundary_tags::get_fit_mode() const
{
    return reinterpret_cast<fit_mode*>(static_cast<char*>(_trusted_memory) + sizeof(memory_resource*));
}

auto allocator_boundary_tags::get_full_size() const
{
    return reinterpret_cast<size_t*>(static_cast<char*>(_trusted_memory) + sizeof(memory_resource*) + sizeof(fit_mode));
}

auto allocator_boundary_tags::get_mutex() const
{
    return reinterpret_cast<std::mutex*>(static_cast<char*>(_trusted_memory) + sizeof(memory_resource*) + sizeof(fit_mode) + sizeof(size_t));
}

auto allocator_boundary_tags::get_head() const
{
    return reinterpret_cast<void**>(static_cast<char*>(_trusted_memory) + sizeof(memory_resource*) + sizeof(fit_mode) + sizeof(size_t) + sizeof(std::mutex));
}

auto allocator_boundary_tags::get_memory_start() const
{
    return static_cast<char*>(_trusted_memory) + allocator_metadata_size;
}

auto allocator_boundary_tags::get_memory_end() const
{
    return static_cast<char*>(_trusted_memory) + *get_full_size();
}

auto allocator_boundary_tags::get_free_prev_block(void* block)
{
    return reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(size_t));
}

auto allocator_boundary_tags::get_free_next_block(void* block)
{
    return reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(size_t) + sizeof(void*));
}

auto allocator_boundary_tags::get_prev_block(void* block)
{
    return reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(size_t) + 2 * sizeof(void*));
}

auto allocator_boundary_tags::set_prev_block(void* block, void* prev_block)
{
    *get_prev_block(block) = prev_block;
}

auto allocator_boundary_tags::get_size_block_ptr(void* block)
{
    return static_cast<size_t*>(block);
}

auto allocator_boundary_tags::get_size_block(void* block)
{
    return *get_size_block_ptr(block) & ~size_t(1);
}

void allocator_boundary_tags::insert_free_block(void* block) const
{
    void* prev = nullptr;
    void* current = *get_head();

    while (current != nullptr && current < block)
    {
        prev = current;
        current = *get_free_next_block(current);
    }

    *get_free_prev_block(block) = prev;
    *get_free_next_block(block) = current;

    if (prev == nullptr)
    {
        *get_head() = block;
    }
    else
    {
        *get_free_next_block(prev) = block;
    }

    if (current != nullptr)
    {
        *get_free_prev_block(current) = block;
    }
}

void allocator_boundary_tags::remove_free_block(void* block) const
{
    void* prev = *get_free_prev_block(block);
    void* next = *get_free_next_block(block);

    if (prev == nullptr)
    {
        *get_head() = next;
    }
    else
    {
        *get_free_next_block(prev) = next;
    }

    if (next != nullptr)
    {
        *get_free_prev_block(next) = prev;
    }
}

allocator_boundary_tags::block_choice allocator_boundary_tags::find_block(size_t needed) const
{
    block_choice choice{nullptr, 0};

    for (void* block = *get_head(); block != nullptr; block = *get_free_next_block(block))
    {
        size_t block_size = get_size_block(block);
        if (block_size < needed)
        {
            continue;
        }

        if (*get_fit_mode() == fit_mode::first_fit)
        {
            return {block, block_size};
        }

        if (choice.block == nullptr)
        {
            choice = {block, block_size};
            continue;
        }

        if (*get_fit_mode() == fit_mode::the_best_fit && block_size < choice.size)
        {
            choice = {block, block_size};
        }

        if (*get_fit_mode() == fit_mode::the_worst_fit && block_size > choice.size)
        {
            choice = {block, block_size};
        }
    }

    return choice;
}

bool allocator_boundary_tags::is_block_free(void* block)
{
    return (*get_size_block_ptr(block) & size_t(1)) == 0;
}

allocator_boundary_tags::~allocator_boundary_tags()
{
    get_mutex()->~mutex();
    (*get_parent_alloc())->deallocate(_trusted_memory, *get_full_size());
}

allocator_boundary_tags::allocator_boundary_tags(
    size_t space_size,
    std::pmr::memory_resource* parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size < occupied_block_metadata_size)
    {
        throw std::bad_alloc();
    }

    parent_allocator = parent_allocator != nullptr
        ? parent_allocator
        : std::pmr::get_default_resource();

    size_t total_size = allocator_metadata_size + space_size;
    _trusted_memory = parent_allocator->allocate(total_size);

    *get_parent_alloc() = parent_allocator;
    *get_fit_mode() = allocate_fit_mode;
    *get_full_size() = total_size;
    new (get_mutex()) std::mutex();

    void* first_block = get_memory_start();
    *get_head() = first_block;
    *get_free_prev_block(first_block) = nullptr;
    *get_free_next_block(first_block) = nullptr;
    *get_prev_block(first_block) = nullptr;
    *get_size_block_ptr(first_block) = space_size;
}

[[nodiscard]] void* allocator_boundary_tags::do_allocate_sm(size_t size)
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
    if (remainder >= occupied_block_metadata_size)
    {
        void* free_block = static_cast<char*>(choice.block) + needed;
        *get_size_block_ptr(choice.block) = needed | size_t(1);
        set_prev_block(choice.block, prev_block);

        *get_size_block_ptr(free_block) = remainder;
        *get_free_prev_block(free_block) = nullptr;
        *get_free_next_block(free_block) = nullptr;
        set_prev_block(free_block, choice.block);

        void* next_block = static_cast<char*>(free_block) + remainder;
        if (next_block < get_memory_end())
        {
            set_prev_block(next_block, free_block);
        }

        insert_free_block(free_block);
    }
    else
    {
        *get_size_block_ptr(choice.block) = choice.size | size_t(1);
        set_prev_block(choice.block, prev_block);
    }

    return static_cast<char*>(choice.block) + occupied_block_metadata_size;
}

void allocator_boundary_tags::do_deallocate_sm(void* at)
{
    std::lock_guard lock(*get_mutex());

    if (at == nullptr)
    {
        return;
    }

    if (at < get_memory_start() || at >= get_memory_end())
    {
        throw std::invalid_argument("block bounds are invalid");
    }

    void* block = static_cast<char*>(at) - occupied_block_metadata_size;
    *get_size_block_ptr(block) = get_size_block(block);

    if (void* prev = *get_prev_block(block); prev != nullptr && is_block_free(prev))
    {
        remove_free_block(prev);
        *get_size_block_ptr(prev) = get_size_block(prev) + get_size_block(block);
        block = prev;
    }

    void* next = static_cast<char*>(block) + get_size_block(block);
    if (next < get_memory_end() && is_block_free(next))
    {
        remove_free_block(next);
        *get_size_block_ptr(block) = get_size_block(block) + get_size_block(next);
    }

    next = static_cast<char*>(block) + get_size_block(block);
    if (next < get_memory_end())
    {
        set_prev_block(next, block);
    }

    insert_free_block(block);
}

inline void allocator_boundary_tags::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard lock(*get_mutex());
    *get_fit_mode() = mode;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    std::lock_guard lock(*get_mutex());
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
{
    std::vector<block_info> result;

    for (char* block = get_memory_start(); block < get_memory_end(); block += get_size_block(block))
    {
        result.push_back({get_size_block(block), !is_block_free(block)});
    }

    return result;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    return {const_cast<allocator_boundary_tags*>(this)};
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() noexcept
{
    return {};
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    if (this == &other)
    {
        return true;
    }

    auto* rhs = dynamic_cast<const allocator_boundary_tags*>(&other);
    return rhs != nullptr && _trusted_memory == rhs->_trusted_memory;
}

bool allocator_boundary_tags::boundary_iterator::operator==(const allocator_boundary_tags::boundary_iterator& other) const noexcept
{
    return _occupied_ptr == other._occupied_ptr && _trusted_memory == other._trusted_memory;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(const allocator_boundary_tags::boundary_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator++() & noexcept
{
    if (_occupied_ptr == nullptr || _trusted_memory == nullptr)
    {
        return *this;
    }

    auto* allocator = static_cast<allocator_boundary_tags*>(_trusted_memory);
    _occupied_ptr = static_cast<char*>(_occupied_ptr) + allocator_boundary_tags::get_size_block(_occupied_ptr);
    if (_occupied_ptr >= allocator->get_memory_end())
    {
        _occupied_ptr = nullptr;
        _occupied = false;
    }
    else
    {
        _occupied = !allocator_boundary_tags::is_block_free(_occupied_ptr);
    }

    return *this;
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    if (_occupied_ptr == nullptr || _trusted_memory == nullptr)
    {
        return *this;
    }

    auto* allocator = static_cast<allocator_boundary_tags*>(_trusted_memory);
    void* prev = *allocator_boundary_tags::get_prev_block(_occupied_ptr);
    if (prev != nullptr && prev >= allocator->get_memory_start())
    {
        _occupied_ptr = prev;
        _occupied = !allocator_boundary_tags::is_block_free(_occupied_ptr);
    }

    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator++(int)
{
    auto copy = *this;
    ++(*this);
    return copy;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator--(int)
{
    auto copy = *this;
    --(*this);
    return copy;
}

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept
{
    return _occupied_ptr == nullptr ? 0 : allocator_boundary_tags::get_size_block(_occupied_ptr);
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    return _occupied;
}

void* allocator_boundary_tags::boundary_iterator::operator*() const noexcept
{
    return occupied() ? static_cast<char*>(_occupied_ptr) + occupied_block_metadata_size : nullptr;
}

allocator_boundary_tags::boundary_iterator::boundary_iterator()
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(nullptr)
{
}

allocator_boundary_tags::boundary_iterator::boundary_iterator(void* trusted)
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(trusted)
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    auto* allocator = static_cast<allocator_boundary_tags*>(_trusted_memory);
    if (allocator->get_memory_start() < allocator->get_memory_end())
    {
        _occupied_ptr = allocator->get_memory_start();
        _occupied = !allocator_boundary_tags::is_block_free(_occupied_ptr);
    }
}

void* allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr;
}
