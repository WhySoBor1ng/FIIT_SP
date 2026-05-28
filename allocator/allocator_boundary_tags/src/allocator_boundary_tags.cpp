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

auto allocator_boundary_tags::get_prev_block(void* block)
{
    return reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(size_t));
}

auto allocator_boundary_tags::get_next_block(void* block)
{
    return reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(size_t) + sizeof(void*));
}

auto allocator_boundary_tags::set_prev_block(void* block, void* prev_block)
{
    *get_prev_block(block) = prev_block;
}

auto allocator_boundary_tags::set_next_block(void* block, void* next_block)
{
    *get_next_block(block) = next_block;
}

auto allocator_boundary_tags::get_size_block_ptr(void* block)
{
    return static_cast<size_t*>(block);
}

auto allocator_boundary_tags::get_size_block(void* block)
{
    return *get_size_block_ptr(block) & ~size_t(1);
}

void allocator_boundary_tags::insert_occupied_block(void* block, void* prev, void* next) const
{
    set_prev_block(block, prev);
    set_next_block(block, next);

    if (prev == nullptr)
    {
        *get_head() = block;
    }
    else
    {
        set_next_block(prev, block);
    }

    if (next != nullptr)
    {
        set_prev_block(next, block);
    }
}

void allocator_boundary_tags::remove_occupied_block(void* block) const
{
    void* prev = *get_prev_block(block);
    void* next = *get_next_block(block);

    if (prev == nullptr)
    {
        *get_head() = next;
    }
    else
    {
        set_next_block(prev, next);
    }

    if (next != nullptr)
    {
        set_prev_block(next, prev);
    }
}

allocator_boundary_tags::occupied_block allocator_boundary_tags::find_block(size_t needed) const
{
    occupied_block choice{nullptr, nullptr, nullptr, 0};

    void* previous = nullptr;
    void* current = *get_head();
    char* gap_start = get_memory_start();

    while (true)
    {
        char* gap_end = current == nullptr
            ? get_memory_end()
            : static_cast<char*>(current);
        size_t gap_size = static_cast<size_t>(gap_end - gap_start);

        if (gap_size >= needed)
        {
            if (*get_fit_mode() == fit_mode::first_fit)
            {
                return {gap_start, previous, current, gap_size};
            }

            if (choice.block == nullptr
                || (*get_fit_mode() == fit_mode::the_best_fit && gap_size < choice.size)
                || (*get_fit_mode() == fit_mode::the_worst_fit && gap_size > choice.size))
            {
                choice = {gap_start, previous, current, gap_size};
            }
        }

        if (current == nullptr)
        {
            break;
        }

        previous = current;
        gap_start = static_cast<char*>(current) + get_size_block(current);
        current = *get_next_block(current);
    }

    return choice;
}

void* allocator_boundary_tags::find_next_occupied_block(void* block) const
{
    for (void* current = *get_head(); current != nullptr; current = *get_next_block(current))
    {
        if (current > block)
        {
            return current;
        }
    }

    return nullptr;
}

void* allocator_boundary_tags::find_prev_occupied_block(void* block) const
{
    void* previous = nullptr;

    for (void* current = *get_head(); current != nullptr && current < block; current = *get_next_block(current))
    {
        previous = current;
    }

    return previous;
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
    *get_head() = nullptr;
}

[[nodiscard]] void* allocator_boundary_tags::do_allocate_sm(size_t size)
{
    std::lock_guard lock(*get_mutex());

    size_t needed = size + occupied_block_metadata_size;
    occupied_block choice = find_block(needed);
    if (choice.block == nullptr)
    {
        throw std::bad_alloc();
    }

    size_t occupied_size = needed;
    if (choice.size - needed < occupied_block_metadata_size)
    {
        occupied_size = choice.size;
    }

    *get_size_block_ptr(choice.block) = occupied_size | size_t(1);
    insert_occupied_block(choice.block, choice.prev, choice.next);

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
    void* current = *get_head();

    while (current != nullptr && current != block)
    {
        current = *get_next_block(current);
    }

    if (current == nullptr)
    {
        throw std::invalid_argument("block is free");
    }

    remove_occupied_block(block);
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

    char* current = get_memory_start();
    for (void* occupied = *get_head(); occupied != nullptr; occupied = *get_next_block(occupied))
    {
        if (current < occupied)
        {
            result.push_back({static_cast<size_t>(static_cast<char*>(occupied) - current), false});
        }

        size_t occupied_size = get_size_block(occupied);
        result.push_back({occupied_size, true});
        current = static_cast<char*>(occupied) + occupied_size;
    }

    if (current < get_memory_end())
    {
        result.push_back({static_cast<size_t>(get_memory_end() - current), false});
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
    return _occupied_ptr == other._occupied_ptr && _occupied == other._occupied && _trusted_memory == other._trusted_memory;
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
    if (_occupied)
    {
        void* next_occupied = *allocator_boundary_tags::get_next_block(_occupied_ptr);
        char* next_block = static_cast<char*>(_occupied_ptr) + allocator_boundary_tags::get_size_block(_occupied_ptr);

        if (next_occupied != nullptr)
        {
            if (next_block < next_occupied)
            {
                _occupied_ptr = next_block;
                _occupied = false;
            }
            else
            {
                _occupied_ptr = next_occupied;
                _occupied = true;
            }
            return *this;
        }

        if (next_block < allocator->get_memory_end())
        {
            _occupied_ptr = next_block;
            _occupied = false;
        }
        else
        {
            _occupied_ptr = nullptr;
            _occupied = false;
        }

        return *this;
    }

    void* next_occupied = allocator->find_next_occupied_block(_occupied_ptr);
    _occupied_ptr = next_occupied;
    _occupied = next_occupied != nullptr;
    return *this;
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    if (_trusted_memory == nullptr)
    {
        return *this;
    }

    auto* allocator = static_cast<allocator_boundary_tags*>(_trusted_memory);

    if (_occupied_ptr == nullptr)
    {
        void* last_occupied = allocator->find_prev_occupied_block(allocator->get_memory_end());
        if (last_occupied == nullptr)
        {
            if (allocator->get_memory_start() < allocator->get_memory_end())
            {
                _occupied_ptr = allocator->get_memory_start();
                _occupied = false;
            }
            return *this;
        }

        char* after_last = static_cast<char*>(last_occupied) + allocator_boundary_tags::get_size_block(last_occupied);
        if (after_last < allocator->get_memory_end())
        {
            _occupied_ptr = after_last;
            _occupied = false;
        }
        else
        {
            _occupied_ptr = last_occupied;
            _occupied = true;
        }
        return *this;
    }

    if (_occupied)
    {
        void* prev_occupied = *allocator_boundary_tags::get_prev_block(_occupied_ptr);
        if (prev_occupied == nullptr)
        {
            if (_occupied_ptr > allocator->get_memory_start())
            {
                _occupied_ptr = allocator->get_memory_start();
                _occupied = false;
            }
            return *this;
        }

        char* gap_start = static_cast<char*>(prev_occupied) + allocator_boundary_tags::get_size_block(prev_occupied);
        if (gap_start < _occupied_ptr)
        {
            _occupied_ptr = gap_start;
            _occupied = false;
        }
        else
        {
            _occupied_ptr = prev_occupied;
            _occupied = true;
        }
        return *this;
    }

    void* prev_occupied = allocator->find_prev_occupied_block(_occupied_ptr);
    _occupied_ptr = prev_occupied;
    _occupied = prev_occupied != nullptr;
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
    if (_occupied_ptr == nullptr || _trusted_memory == nullptr)
    {
        return 0;
    }

    auto* allocator = static_cast<allocator_boundary_tags*>(_trusted_memory);
    if (_occupied)
    {
        return allocator_boundary_tags::get_size_block(_occupied_ptr);
    }

    void* next_occupied = allocator->find_next_occupied_block(_occupied_ptr);
    char* gap_end = next_occupied == nullptr
        ? allocator->get_memory_end()
        : static_cast<char*>(next_occupied);
    return static_cast<size_t>(gap_end - static_cast<char*>(_occupied_ptr));
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
    if (allocator->get_memory_start() >= allocator->get_memory_end())
    {
        return;
    }

    void* first_occupied = *allocator->get_head();
    if (first_occupied == nullptr || first_occupied > allocator->get_memory_start())
    {
        _occupied_ptr = allocator->get_memory_start();
        _occupied = false;
    }
    else
    {
        _occupied_ptr = first_occupied;
        _occupied = true;
    }
}

void* allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr;
}
