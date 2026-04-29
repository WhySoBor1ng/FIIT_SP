#include "../include/allocator_sorted_list.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace
{
    struct allocator_sorted_list_meta final
    {
        std::pmr::memory_resource* parent_allocator;
        allocator_with_fit_mode::fit_mode current_fit_mode;
        size_t managed_space_size;
        std::mutex mutex;
        void* free_head;
    };

    struct allocator_sorted_list_block final
    {
        allocator_sorted_list_block* next_free;
        size_t block_size;
    };

    allocator_sorted_list_meta* get_meta(void* trusted) noexcept
    {
        return reinterpret_cast<allocator_sorted_list_meta*>(trusted);
    }

    const allocator_sorted_list_meta* get_meta(const void* trusted) noexcept
    {
        return reinterpret_cast<const allocator_sorted_list_meta*>(trusted);
    }

    allocator_sorted_list_block* get_first_block(void* trusted) noexcept
    {
        return reinterpret_cast<allocator_sorted_list_block*>(
            reinterpret_cast<unsigned char*>(trusted) + sizeof(allocator_sorted_list_meta));
    }

    const allocator_sorted_list_block* get_first_block(const void* trusted) noexcept
    {
        return reinterpret_cast<const allocator_sorted_list_block*>(
            reinterpret_cast<const unsigned char*>(trusted) + sizeof(allocator_sorted_list_meta));
    }

    unsigned char* get_region_end(void* trusted) noexcept
    {
        auto* meta = get_meta(trusted);
        return reinterpret_cast<unsigned char*>(get_first_block(trusted))
               + sizeof(allocator_sorted_list_block)
               + meta->managed_space_size;
    }

    const unsigned char* get_region_end(const void* trusted) noexcept
    {
        auto* meta = get_meta(trusted);
        return reinterpret_cast<const unsigned char*>(get_first_block(trusted))
               + sizeof(allocator_sorted_list_block)
               + meta->managed_space_size;
    }

    allocator_sorted_list_block* get_next_physical_block(
        allocator_sorted_list_block* block,
        void* trusted) noexcept
    {
        auto* next = reinterpret_cast<allocator_sorted_list_block*>(
            reinterpret_cast<unsigned char*>(block)
            + sizeof(allocator_sorted_list_block)
            + block->block_size);

        return reinterpret_cast<unsigned char*>(next) < get_region_end(trusted) ? next : nullptr;
    }

    const allocator_sorted_list_block* get_next_physical_block(
        const allocator_sorted_list_block* block,
        const void* trusted) noexcept
    {
        auto* next = reinterpret_cast<const allocator_sorted_list_block*>(
            reinterpret_cast<const unsigned char*>(block)
            + sizeof(allocator_sorted_list_block)
            + block->block_size);

        return reinterpret_cast<const unsigned char*>(next) < get_region_end(trusted) ? next : nullptr;
    }

    bool is_free_block(
        const void* trusted,
        const allocator_sorted_list_block* block) noexcept
    {
        auto* current = reinterpret_cast<const allocator_sorted_list_block*>(get_meta(trusted)->free_head);
        while (current != nullptr)
        {
            if (current == block)
            {
                return true;
            }
            current = current->next_free;
        }
        return false;
    }

    allocator_sorted_list_block* clone_state(const void* source_trusted)
    {
        const auto* source_meta = get_meta(source_trusted);
        auto* parent_allocator = source_meta->parent_allocator == nullptr
            ? std::pmr::get_default_resource()
            : source_meta->parent_allocator;

        const auto total_size = sizeof(allocator_sorted_list_meta)
                                + sizeof(allocator_sorted_list_block)
                                + source_meta->managed_space_size;

        void* new_memory = parent_allocator->allocate(total_size, alignof(std::max_align_t));
        std::memcpy(new_memory, source_trusted, total_size);

        auto* new_meta = get_meta(new_memory);
        new (&new_meta->mutex) std::mutex();
        new_meta->parent_allocator = parent_allocator;

        const auto offset = reinterpret_cast<unsigned char*>(new_memory)
                            - reinterpret_cast<const unsigned char*>(source_trusted);

        if (new_meta->free_head != nullptr)
        {
            new_meta->free_head = reinterpret_cast<unsigned char*>(new_meta->free_head) + offset;

            auto* current = reinterpret_cast<allocator_sorted_list_block*>(new_meta->free_head);
            while (current != nullptr)
            {
                if (current->next_free != nullptr)
                {
                    current->next_free = reinterpret_cast<allocator_sorted_list_block*>(
                        reinterpret_cast<unsigned char*>(current->next_free) + offset);
                }
                current = current->next_free;
            }
        }

        return reinterpret_cast<allocator_sorted_list_block*>(new_memory);
    }
}

allocator_sorted_list::~allocator_sorted_list()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    auto* meta = get_meta(_trusted_memory);
    auto* parent_allocator = meta->parent_allocator == nullptr
        ? std::pmr::get_default_resource()
        : meta->parent_allocator;
    const auto total_size = sizeof(allocator_sorted_list_meta)
                            + sizeof(allocator_sorted_list_block)
                            + meta->managed_space_size;

    meta->mutex.~mutex();
    parent_allocator->deallocate(_trusted_memory, total_size, alignof(std::max_align_t));
    _trusted_memory = nullptr;
}

allocator_sorted_list::allocator_sorted_list(
    allocator_sorted_list&& other) noexcept :
    _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_sorted_list& allocator_sorted_list::operator=(
    allocator_sorted_list&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    this->~allocator_sorted_list();
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
    return *this;
}

allocator_sorted_list::allocator_sorted_list(
    size_t space_size,
    std::pmr::memory_resource* parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode) :
    _trusted_memory(nullptr)
{
    auto* real_parent_allocator = parent_allocator == nullptr
        ? std::pmr::get_default_resource()
        : parent_allocator;

    const auto total_size = sizeof(allocator_sorted_list_meta)
                            + sizeof(allocator_sorted_list_block)
                            + space_size;

    _trusted_memory = real_parent_allocator->allocate(total_size, alignof(std::max_align_t));

    auto* meta = new (_trusted_memory) allocator_sorted_list_meta{
        real_parent_allocator,
        allocate_fit_mode,
        space_size,
        std::mutex{},
        nullptr
    };

    auto* first_block = get_first_block(_trusted_memory);
    first_block->next_free = nullptr;
    first_block->block_size = space_size;
    meta->free_head = first_block;
}

[[nodiscard]] void* allocator_sorted_list::do_allocate_sm(
    size_t size)
{
    std::lock_guard lock(get_meta(_trusted_memory)->mutex);

    if (size == 0)
    {
        size = 1;
    }

    auto* meta = get_meta(_trusted_memory);
    auto* current = reinterpret_cast<allocator_sorted_list_block*>(meta->free_head);
    allocator_sorted_list_block* previous = nullptr;

    allocator_sorted_list_block* selected = nullptr;
    allocator_sorted_list_block* selected_previous = nullptr;

    while (current != nullptr)
    {
        if (current->block_size >= size)
        {
            if (meta->current_fit_mode == fit_mode::first_fit)
            {
                selected = current;
                selected_previous = previous;
                break;
            }

            if (selected == nullptr)
            {
                selected = current;
                selected_previous = previous;
            }
            else if (meta->current_fit_mode == fit_mode::the_best_fit
                     && current->block_size < selected->block_size)
            {
                selected = current;
                selected_previous = previous;
            }
            else if (meta->current_fit_mode == fit_mode::the_worst_fit
                     && current->block_size > selected->block_size)
            {
                selected = current;
                selected_previous = previous;
            }
        }

        previous = current;
        current = current->next_free;
    }

    if (selected == nullptr)
    {
        throw std::bad_alloc();
    }

    const auto remaining_size = selected->block_size - size;
    if (remaining_size > sizeof(allocator_sorted_list_block))
    {
        auto* new_free_block = reinterpret_cast<allocator_sorted_list_block*>(
            reinterpret_cast<unsigned char*>(selected)
            + sizeof(allocator_sorted_list_block)
            + size);

        new_free_block->block_size = remaining_size - sizeof(allocator_sorted_list_block);
        new_free_block->next_free = selected->next_free;

        if (selected_previous == nullptr)
        {
            meta->free_head = new_free_block;
        }
        else
        {
            selected_previous->next_free = new_free_block;
        }

        selected->block_size = size;
    }
    else
    {
        if (selected_previous == nullptr)
        {
            meta->free_head = selected->next_free;
        }
        else
        {
            selected_previous->next_free = selected->next_free;
        }
    }

    selected->next_free = nullptr;
    return reinterpret_cast<unsigned char*>(selected) + sizeof(allocator_sorted_list_block);
}

allocator_sorted_list::allocator_sorted_list(
    const allocator_sorted_list& other) :
    _trusted_memory(nullptr)
{
    std::lock_guard lock(get_meta(other._trusted_memory)->mutex);
    _trusted_memory = clone_state(other._trusted_memory);
}

allocator_sorted_list& allocator_sorted_list::operator=(const allocator_sorted_list& other)
{
    if (this == &other)
    {
        return *this;
    }

    auto* other_meta = get_meta(other._trusted_memory);
    std::lock_guard other_lock(other_meta->mutex);
    void* new_memory = clone_state(other._trusted_memory);

    this->~allocator_sorted_list();
    _trusted_memory = new_memory;
    return *this;
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    if (this == &other)
    {
        return true;
    }

    auto* other_ptr = dynamic_cast<const allocator_sorted_list*>(&other);
    return other_ptr != nullptr && other_ptr == this;
}

void allocator_sorted_list::do_deallocate_sm(
    void* at)
{
    if (at == nullptr)
    {
        return;
    }

    auto* meta = get_meta(_trusted_memory);
    std::lock_guard lock(meta->mutex);

    auto* block = reinterpret_cast<allocator_sorted_list_block*>(
        reinterpret_cast<unsigned char*>(at) - sizeof(allocator_sorted_list_block));

    allocator_sorted_list_block* previous = nullptr;
    auto* current = reinterpret_cast<allocator_sorted_list_block*>(meta->free_head);

    while (current != nullptr && current < block)
    {
        previous = current;
        current = current->next_free;
    }

    block->next_free = current;
    if (previous == nullptr)
    {
        meta->free_head = block;
    }
    else
    {
        previous->next_free = block;
    }

    if (current != nullptr
        && reinterpret_cast<unsigned char*>(block)
           + sizeof(allocator_sorted_list_block)
           + block->block_size
           == reinterpret_cast<unsigned char*>(current))
    {
        block->block_size += sizeof(allocator_sorted_list_block) + current->block_size;
        block->next_free = current->next_free;
    }

    if (previous != nullptr
        && reinterpret_cast<unsigned char*>(previous)
           + sizeof(allocator_sorted_list_block)
           + previous->block_size
           == reinterpret_cast<unsigned char*>(block))
    {
        previous->block_size += sizeof(allocator_sorted_list_block) + block->block_size;
        previous->next_free = block->next_free;
    }
}

inline void allocator_sorted_list::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard lock(get_meta(_trusted_memory)->mutex);
    get_meta(_trusted_memory)->current_fit_mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    std::lock_guard lock(get_meta(_trusted_memory)->mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;

    for (auto it = begin(); it != end(); ++it)
    {
        result.push_back({it.size(), it.occupied()});
    }

    return result;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept
{
    return sorted_free_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept
{
    return sorted_free_iterator();
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept
{
    return sorted_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept
{
    return sorted_iterator();
}

bool allocator_sorted_list::sorted_free_iterator::operator==(
    const allocator_sorted_list::sorted_free_iterator& other) const noexcept
{
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(
    const allocator_sorted_list::sorted_free_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_free_iterator& allocator_sorted_list::sorted_free_iterator::operator++() & noexcept
{
    if (_free_ptr != nullptr)
    {
        _free_ptr = reinterpret_cast<allocator_sorted_list_block*>(_free_ptr)->next_free;
    }
    return *this;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::sorted_free_iterator::operator++(int)
{
    auto copy = *this;
    ++(*this);
    return copy;
}

size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept
{
    return _free_ptr == nullptr
        ? 0
        : reinterpret_cast<allocator_sorted_list_block*>(_free_ptr)->block_size;
}

void* allocator_sorted_list::sorted_free_iterator::operator*() const noexcept
{
    return _free_ptr == nullptr
        ? nullptr
        : reinterpret_cast<unsigned char*>(_free_ptr) + sizeof(allocator_sorted_list_block);
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator() :
    _free_ptr(nullptr)
{
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void* trusted) :
    _free_ptr(trusted == nullptr ? nullptr : get_meta(trusted)->free_head)
{
}

bool allocator_sorted_list::sorted_iterator::operator==(
    const allocator_sorted_list::sorted_iterator& other) const noexcept
{
    return _current_ptr == other._current_ptr && _trusted_memory == other._trusted_memory;
}

bool allocator_sorted_list::sorted_iterator::operator!=(
    const allocator_sorted_list::sorted_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_iterator& allocator_sorted_list::sorted_iterator::operator++() & noexcept
{
    if (_current_ptr != nullptr)
    {
        _current_ptr = get_next_physical_block(
            reinterpret_cast<allocator_sorted_list_block*>(_current_ptr),
            _trusted_memory);
    }
    return *this;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::sorted_iterator::operator++(int)
{
    auto copy = *this;
    ++(*this);
    return copy;
}

size_t allocator_sorted_list::sorted_iterator::size() const noexcept
{
    return _current_ptr == nullptr
        ? 0
        : reinterpret_cast<allocator_sorted_list_block*>(_current_ptr)->block_size;
}

void* allocator_sorted_list::sorted_iterator::operator*() const noexcept
{
    return _current_ptr == nullptr
        ? nullptr
        : reinterpret_cast<unsigned char*>(_current_ptr) + sizeof(allocator_sorted_list_block);
}

allocator_sorted_list::sorted_iterator::sorted_iterator() :
    _free_ptr(nullptr),
    _current_ptr(nullptr),
    _trusted_memory(nullptr)
{
}

allocator_sorted_list::sorted_iterator::sorted_iterator(void* trusted) :
    _free_ptr(trusted == nullptr ? nullptr : get_meta(trusted)->free_head),
    _current_ptr(trusted == nullptr ? nullptr : get_first_block(trusted)),
    _trusted_memory(trusted)
{
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept
{
    return _current_ptr != nullptr
        && !is_free_block(_trusted_memory, reinterpret_cast<allocator_sorted_list_block*>(_current_ptr));
}
