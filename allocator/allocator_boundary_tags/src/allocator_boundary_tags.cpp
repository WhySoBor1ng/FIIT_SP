#include "../include/allocator_boundary_tags.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace
{
    struct boundary_allocator_meta final
    {
        std::pmr::memory_resource* parent_allocator;
        allocator_with_fit_mode::fit_mode fit_mode;
        size_t space_size;
        std::mutex mutex;
        void* first_occupied;
        void* last_occupied;
    };

    struct occupied_block final
    {
        size_t payload_capacity;
        occupied_block* prev;
        occupied_block* next;
        void* reserved;
    };

    boundary_allocator_meta* get_meta(void* trusted) noexcept
    {
        return reinterpret_cast<boundary_allocator_meta*>(trusted);
    }

    const boundary_allocator_meta* get_meta(const void* trusted) noexcept
    {
        return reinterpret_cast<const boundary_allocator_meta*>(trusted);
    }

    unsigned char* region_begin(void* trusted) noexcept
    {
        return reinterpret_cast<unsigned char*>(trusted) + sizeof(boundary_allocator_meta);
    }

    const unsigned char* region_begin(const void* trusted) noexcept
    {
        return reinterpret_cast<const unsigned char*>(trusted) + sizeof(boundary_allocator_meta);
    }

    unsigned char* region_end(void* trusted) noexcept
    {
        return region_begin(trusted) + get_meta(trusted)->space_size;
    }

    const unsigned char* region_end(const void* trusted) noexcept
    {
        return region_begin(trusted) + get_meta(trusted)->space_size;
    }

    occupied_block* first_block(void* trusted) noexcept
    {
        return reinterpret_cast<occupied_block*>(get_meta(trusted)->first_occupied);
    }

    const occupied_block* first_block(const void* trusted) noexcept
    {
        return reinterpret_cast<const occupied_block*>(get_meta(trusted)->first_occupied);
    }

    occupied_block* next_physical_block(occupied_block* block) noexcept
    {
        return block == nullptr ? nullptr : block->next;
    }

    const occupied_block* next_physical_block(const occupied_block* block) noexcept
    {
        return block == nullptr ? nullptr : block->next;
    }

    unsigned char* block_end(occupied_block* block) noexcept
    {
        return reinterpret_cast<unsigned char*>(block)
               + sizeof(occupied_block)
               + block->payload_capacity;
    }

    const unsigned char* block_end(const occupied_block* block) noexcept
    {
        return reinterpret_cast<const unsigned char*>(block)
               + sizeof(occupied_block)
               + block->payload_capacity;
    }

    void* clone_state(const void* source_trusted)
    {
        const auto* source_meta = get_meta(source_trusted);
        auto* parent_allocator = source_meta->parent_allocator == nullptr
            ? std::pmr::get_default_resource()
            : source_meta->parent_allocator;

        const auto total_size = sizeof(boundary_allocator_meta) + source_meta->space_size;
        void* new_memory = parent_allocator->allocate(total_size, alignof(std::max_align_t));
        std::memcpy(new_memory, source_trusted, total_size);

        auto* new_meta = get_meta(new_memory);
        new (&new_meta->mutex) std::mutex();
        new_meta->parent_allocator = parent_allocator;

        const auto offset = reinterpret_cast<unsigned char*>(new_memory)
                            - reinterpret_cast<const unsigned char*>(source_trusted);

        if (new_meta->first_occupied != nullptr)
        {
            new_meta->first_occupied = reinterpret_cast<unsigned char*>(new_meta->first_occupied) + offset;
        }
        if (new_meta->last_occupied != nullptr)
        {
            new_meta->last_occupied = reinterpret_cast<unsigned char*>(new_meta->last_occupied) + offset;
        }

        auto* current = reinterpret_cast<occupied_block*>(new_meta->first_occupied);
        while (current != nullptr)
        {
            if (current->prev != nullptr)
            {
                current->prev = reinterpret_cast<occupied_block*>(reinterpret_cast<unsigned char*>(current->prev) + offset);
            }
            if (current->next != nullptr)
            {
                current->next = reinterpret_cast<occupied_block*>(reinterpret_cast<unsigned char*>(current->next) + offset);
            }
            current = current->next;
        }

        return new_memory;
    }
}

allocator_boundary_tags::~allocator_boundary_tags()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    auto* meta = get_meta(_trusted_memory);
    auto* parent_allocator = meta->parent_allocator == nullptr
        ? std::pmr::get_default_resource()
        : meta->parent_allocator;
    const auto total_size = sizeof(boundary_allocator_meta) + meta->space_size;

    meta->mutex.~mutex();
    parent_allocator->deallocate(_trusted_memory, total_size, alignof(std::max_align_t));
    _trusted_memory = nullptr;
}

allocator_boundary_tags::allocator_boundary_tags(
    allocator_boundary_tags&& other) noexcept :
    _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_boundary_tags& allocator_boundary_tags::operator=(
    allocator_boundary_tags&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    this->~allocator_boundary_tags();
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
    return *this;
}

allocator_boundary_tags::allocator_boundary_tags(
    size_t space_size,
    std::pmr::memory_resource* parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode) :
    _trusted_memory(nullptr)
{
    auto* real_parent_allocator = parent_allocator == nullptr
        ? std::pmr::get_default_resource()
        : parent_allocator;

    const auto total_size = sizeof(boundary_allocator_meta) + space_size;
    _trusted_memory = real_parent_allocator->allocate(total_size, alignof(std::max_align_t));

    new (_trusted_memory) boundary_allocator_meta{
        real_parent_allocator,
        allocate_fit_mode,
        space_size,
        std::mutex{},
        nullptr,
        nullptr
    };
}

[[nodiscard]] void* allocator_boundary_tags::do_allocate_sm(
    size_t size)
{
    std::lock_guard lock(get_meta(_trusted_memory)->mutex);

    auto* meta = get_meta(_trusted_memory);
    occupied_block* best_prev = nullptr;
    occupied_block* best_next = nullptr;
    unsigned char* best_start = nullptr;
    size_t best_gap = 0;

    occupied_block* prev = nullptr;
    auto* current = first_block(_trusted_memory);
    auto* gap_start = region_begin(_trusted_memory);

    const auto required = sizeof(occupied_block) + size;

    while (true)
    {
        const auto* gap_end = current == nullptr
            ? region_end(_trusted_memory)
            : reinterpret_cast<unsigned char*>(current);
        const auto gap_size = static_cast<size_t>(gap_end - gap_start);

        if (gap_size >= required)
        {
            if (meta->fit_mode == fit_mode::first_fit)
            {
                best_prev = prev;
                best_next = current;
                best_start = gap_start;
                best_gap = gap_size;
                break;
            }

            if (best_start == nullptr
                || (meta->fit_mode == fit_mode::the_best_fit && gap_size < best_gap)
                || (meta->fit_mode == fit_mode::the_worst_fit && gap_size > best_gap))
            {
                best_prev = prev;
                best_next = current;
                best_start = gap_start;
                best_gap = gap_size;
            }
        }

        if (current == nullptr)
        {
            break;
        }

        gap_start = block_end(current);
        prev = current;
        current = current->next;
    }

    if (best_start == nullptr)
    {
        throw std::bad_alloc();
    }

    size_t payload_capacity = size;
    if (best_gap != 0 && best_gap - required < sizeof(occupied_block))
    {
        payload_capacity = best_gap - sizeof(occupied_block);
    }

    auto* block = new (best_start) occupied_block{payload_capacity, best_prev, best_next, nullptr};

    if (best_prev == nullptr)
    {
        meta->first_occupied = block;
    }
    else
    {
        best_prev->next = block;
    }

    if (best_next == nullptr)
    {
        meta->last_occupied = block;
    }
    else
    {
        best_next->prev = block;
    }

    return reinterpret_cast<unsigned char*>(block) + sizeof(occupied_block);
}

void allocator_boundary_tags::do_deallocate_sm(
    void* at)
{
    if (at == nullptr)
    {
        return;
    }

    auto* meta = get_meta(_trusted_memory);
    std::lock_guard lock(meta->mutex);

    auto* block = reinterpret_cast<occupied_block*>(
        reinterpret_cast<unsigned char*>(at) - sizeof(occupied_block));

    if (reinterpret_cast<unsigned char*>(block) < region_begin(_trusted_memory)
        || reinterpret_cast<unsigned char*>(block) >= region_end(_trusted_memory))
    {
        return;
    }

    auto* current = first_block(_trusted_memory);
    while (current != nullptr && current != block)
    {
        current = current->next;
    }

    if (current == nullptr)
    {
        return;
    }

    if (block->prev == nullptr)
    {
        meta->first_occupied = block->next;
    }
    else
    {
        block->prev->next = block->next;
    }

    if (block->next == nullptr)
    {
        meta->last_occupied = block->prev;
    }
    else
    {
        block->next->prev = block->prev;
    }
}

inline void allocator_boundary_tags::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard lock(get_meta(_trusted_memory)->mutex);
    get_meta(_trusted_memory)->fit_mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    std::lock_guard lock(get_meta(_trusted_memory)->mutex);
    return get_blocks_info_inner();
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    return boundary_iterator(_trusted_memory);
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept
{
    return boundary_iterator();
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;
    for (auto it = begin(); it != end(); ++it)
    {
        result.push_back({it.size(), it.occupied()});
    }
    return result;
}

allocator_boundary_tags::allocator_boundary_tags(const allocator_boundary_tags& other) :
    _trusted_memory(nullptr)
{
    std::lock_guard lock(get_meta(other._trusted_memory)->mutex);
    _trusted_memory = clone_state(other._trusted_memory);
}

allocator_boundary_tags& allocator_boundary_tags::operator=(const allocator_boundary_tags& other)
{
    if (this == &other)
    {
        return *this;
    }

    auto* other_meta = get_meta(other._trusted_memory);
    std::lock_guard lock(other_meta->mutex);
    void* cloned = clone_state(other._trusted_memory);

    this->~allocator_boundary_tags();
    _trusted_memory = cloned;
    return *this;
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    if (this == &other)
    {
        return true;
    }

    auto* other_ptr = dynamic_cast<const allocator_boundary_tags*>(&other);
    return other_ptr != nullptr && other_ptr == this;
}

bool allocator_boundary_tags::boundary_iterator::operator==(
    const allocator_boundary_tags::boundary_iterator& other) const noexcept
{
    if (_occupied_ptr == nullptr && other._occupied_ptr == nullptr)
    {
        return true;
    }

    return _occupied_ptr == other._occupied_ptr
           && _occupied == other._occupied
           && _trusted_memory == other._trusted_memory;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(
    const allocator_boundary_tags::boundary_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator++() & noexcept
{
    if (_trusted_memory == nullptr || _occupied_ptr == nullptr)
    {
        return *this;
    }

    if (_occupied)
    {
        auto* block = reinterpret_cast<occupied_block*>(_occupied_ptr);
        auto* next = next_physical_block(block);
        auto* free_start = block_end(block);
        auto* free_end = next == nullptr
            ? region_end(_trusted_memory)
            : reinterpret_cast<unsigned char*>(next);

        if (free_start < free_end)
        {
            _occupied = false;
            _occupied_ptr = free_start;
        }
        else if (next != nullptr)
        {
            _occupied_ptr = next;
            _occupied = true;
        }
        else
        {
            _occupied_ptr = nullptr;
        }
    }
    else
    {
        auto* current = first_block(_trusted_memory);
        while (current != nullptr && reinterpret_cast<unsigned char*>(current) < _occupied_ptr)
        {
            current = current->next;
        }
        _occupied_ptr = current;
        _occupied = current != nullptr;
    }

    return *this;
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    if (_trusted_memory == nullptr)
    {
        return *this;
    }

    if (_occupied_ptr == nullptr)
    {
        auto* last = reinterpret_cast<occupied_block*>(get_meta(_trusted_memory)->last_occupied);
        if (last == nullptr)
        {
            return *this;
        }

        const auto* end_ptr = region_end(_trusted_memory);
        if (block_end(last) < end_ptr)
        {
            _occupied = false;
            _occupied_ptr = const_cast<unsigned char*>(block_end(last));
        }
        else
        {
            _occupied = true;
            _occupied_ptr = last;
        }
        return *this;
    }

    if (_occupied)
    {
        auto* block = reinterpret_cast<occupied_block*>(_occupied_ptr);
        auto* prev = block->prev;
        auto* prev_end = prev == nullptr
            ? region_begin(_trusted_memory)
            : block_end(prev);

        if (prev_end < reinterpret_cast<unsigned char*>(block))
        {
            _occupied = false;
            _occupied_ptr = prev_end;
        }
        else
        {
            _occupied_ptr = prev;
            _occupied = prev != nullptr;
        }
    }
    else
    {
        auto* current = first_block(_trusted_memory);
        occupied_block* prev = nullptr;
        while (current != nullptr && block_end(current) <= _occupied_ptr)
        {
            prev = current;
            current = current->next;
        }
        _occupied_ptr = prev;
        _occupied = prev != nullptr;
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
    if (_trusted_memory == nullptr || _occupied_ptr == nullptr)
    {
        return 0;
    }

    if (_occupied)
    {
        auto* block = reinterpret_cast<occupied_block*>(_occupied_ptr);
        return sizeof(occupied_block) + block->payload_capacity;
    }

    auto* current = first_block(_trusted_memory);
    while (current != nullptr && reinterpret_cast<unsigned char*>(current) < _occupied_ptr)
    {
        current = current->next;
    }

    auto* end = current == nullptr
        ? region_end(_trusted_memory)
        : reinterpret_cast<unsigned char*>(current);
    return static_cast<size_t>(end - reinterpret_cast<unsigned char*>(_occupied_ptr));
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    return _occupied;
}

void* allocator_boundary_tags::boundary_iterator::operator*() const noexcept
{
    if (_occupied_ptr == nullptr)
    {
        return nullptr;
    }

    return _occupied
        ? reinterpret_cast<unsigned char*>(_occupied_ptr) + sizeof(occupied_block)
        : _occupied_ptr;
}

allocator_boundary_tags::boundary_iterator::boundary_iterator() :
    _occupied_ptr(nullptr),
    _occupied(false),
    _trusted_memory(nullptr)
{
}

allocator_boundary_tags::boundary_iterator::boundary_iterator(void* trusted) :
    _occupied_ptr(nullptr),
    _occupied(false),
    _trusted_memory(trusted)
{
    if (trusted == nullptr)
    {
        return;
    }

    auto* first = first_block(trusted);
    if (first == nullptr)
    {
        _occupied_ptr = region_begin(trusted);
        _occupied = false;
        if (_occupied_ptr == region_end(trusted))
        {
            _occupied_ptr = nullptr;
        }
        return;
    }

    if (region_begin(trusted) < reinterpret_cast<unsigned char*>(first))
    {
        _occupied_ptr = region_begin(trusted);
        _occupied = false;
    }
    else
    {
        _occupied_ptr = first;
        _occupied = true;
    }
}

void* allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr;
}
