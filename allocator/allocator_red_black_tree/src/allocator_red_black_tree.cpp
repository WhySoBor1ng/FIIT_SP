#include "../include/allocator_red_black_tree.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace
{
    using byte = unsigned char;

    struct extent final
    {
        size_t offset;
        size_t size;
        bool occupied;
        extent* prev;
        extent* next;
    };

    struct rb_allocator_meta final
    {
        std::pmr::memory_resource* parent_allocator;
        allocator_with_fit_mode::fit_mode fit_mode;
        size_t managed_size;
        std::mutex mutex;
        byte* region;
        extent* head;
    };

    rb_allocator_meta* get_meta(void* trusted) noexcept
    {
        return reinterpret_cast<rb_allocator_meta*>(trusted);
    }

    const rb_allocator_meta* get_meta(const void* trusted) noexcept
    {
        return reinterpret_cast<const rb_allocator_meta*>(trusted);
    }

    extent* make_extent(
        size_t offset,
        size_t size,
        bool occupied,
        extent* prev = nullptr,
        extent* next = nullptr)
    {
        return new extent{offset, size, occupied, prev, next};
    }

    void destroy_extents(extent* head) noexcept
    {
        while (head != nullptr)
        {
            auto* next = head->next;
            delete head;
            head = next;
        }
    }

    extent* clone_extents(extent* source_head)
    {
        extent* new_head = nullptr;
        extent* tail = nullptr;

        for (auto* current = source_head; current != nullptr; current = current->next)
        {
            auto* node = make_extent(current->offset, current->size, current->occupied, tail, nullptr);
            if (tail == nullptr)
            {
                new_head = node;
            }
            else
            {
                tail->next = node;
            }
            tail = node;
        }

        return new_head;
    }

    void* clone_state(const void* source_trusted)
    {
        const auto* source_meta = get_meta(source_trusted);
        auto* parent_allocator = source_meta->parent_allocator == nullptr
            ? std::pmr::get_default_resource()
            : source_meta->parent_allocator;

        const auto total_size = sizeof(rb_allocator_meta) + source_meta->managed_size;
        void* new_memory = parent_allocator->allocate(total_size, alignof(std::max_align_t));
        auto* new_meta = new (new_memory) rb_allocator_meta{
            parent_allocator,
            source_meta->fit_mode,
            source_meta->managed_size,
            std::mutex{},
            reinterpret_cast<byte*>(new_memory) + sizeof(rb_allocator_meta),
            nullptr
        };

        std::memcpy(new_meta->region, source_meta->region, source_meta->managed_size);
        new_meta->head = clone_extents(source_meta->head);
        return new_memory;
    }
}

allocator_red_black_tree::~allocator_red_black_tree()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    auto* meta = get_meta(_trusted_memory);
    auto* parent_allocator = meta->parent_allocator == nullptr
        ? std::pmr::get_default_resource()
        : meta->parent_allocator;
    const auto total_size = sizeof(rb_allocator_meta) + meta->managed_size;

    destroy_extents(meta->head);
    meta->mutex.~mutex();
    parent_allocator->deallocate(_trusted_memory, total_size, alignof(std::max_align_t));
    _trusted_memory = nullptr;
}

allocator_red_black_tree::allocator_red_black_tree(
    allocator_red_black_tree&& other) noexcept :
    _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_red_black_tree& allocator_red_black_tree::operator=(
    allocator_red_black_tree&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    this->~allocator_red_black_tree();
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
    return *this;
}

allocator_red_black_tree::allocator_red_black_tree(
    size_t space_size,
    std::pmr::memory_resource* parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode) :
    _trusted_memory(nullptr)
{
    auto* real_parent_allocator = parent_allocator == nullptr
        ? std::pmr::get_default_resource()
        : parent_allocator;

    const auto total_size = sizeof(rb_allocator_meta) + space_size;
    _trusted_memory = real_parent_allocator->allocate(total_size, alignof(std::max_align_t));

    auto* meta = new (_trusted_memory) rb_allocator_meta{
        real_parent_allocator,
        allocate_fit_mode,
        space_size,
        std::mutex{},
        reinterpret_cast<byte*>(_trusted_memory) + sizeof(rb_allocator_meta),
        nullptr
    };

    meta->head = make_extent(0, space_size, false);
}

allocator_red_black_tree::allocator_red_black_tree(const allocator_red_black_tree& other) :
    _trusted_memory(nullptr)
{
    std::lock_guard lock(get_meta(other._trusted_memory)->mutex);
    _trusted_memory = clone_state(other._trusted_memory);
}

allocator_red_black_tree& allocator_red_black_tree::operator=(const allocator_red_black_tree& other)
{
    if (this == &other)
    {
        return *this;
    }

    auto* other_meta = get_meta(other._trusted_memory);
    std::lock_guard lock(other_meta->mutex);
    void* cloned = clone_state(other._trusted_memory);

    this->~allocator_red_black_tree();
    _trusted_memory = cloned;
    return *this;
}

bool allocator_red_black_tree::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    if (this == &other)
    {
        return true;
    }

    auto* other_ptr = dynamic_cast<const allocator_red_black_tree*>(&other);
    return other_ptr != nullptr && other_ptr == this;
}

[[nodiscard]] void* allocator_red_black_tree::do_allocate_sm(
    size_t size)
{
    auto* meta = get_meta(_trusted_memory);
    std::lock_guard lock(meta->mutex);

    if (size == 0)
    {
        size = 1;
    }

    extent* selected = nullptr;
    for (auto* current = meta->head; current != nullptr; current = current->next)
    {
        if (current->occupied || current->size < size)
        {
            continue;
        }

        if (meta->fit_mode == fit_mode::first_fit)
        {
            selected = current;
            break;
        }

        if (selected == nullptr
            || (meta->fit_mode == fit_mode::the_best_fit && current->size < selected->size)
            || (meta->fit_mode == fit_mode::the_worst_fit && current->size > selected->size))
        {
            selected = current;
        }
    }

    if (selected == nullptr)
    {
        throw std::bad_alloc();
    }

    if (selected->size > size)
    {
        auto* tail = make_extent(selected->offset + size, selected->size - size, false, selected, selected->next);
        if (selected->next != nullptr)
        {
            selected->next->prev = tail;
        }
        selected->next = tail;
        selected->size = size;
    }

    selected->occupied = true;
    return meta->region + selected->offset;
}

void allocator_red_black_tree::do_deallocate_sm(
    void* at)
{
    if (at == nullptr)
    {
        return;
    }

    auto* meta = get_meta(_trusted_memory);
    std::lock_guard lock(meta->mutex);

    auto* byte_at = reinterpret_cast<byte*>(at);
    if (byte_at < meta->region || byte_at >= meta->region + meta->managed_size)
    {
        return;
    }

    const auto offset = static_cast<size_t>(byte_at - meta->region);
    extent* block = nullptr;
    for (auto* current = meta->head; current != nullptr; current = current->next)
    {
        if (current->offset == offset)
        {
            block = current;
            break;
        }
    }

    if (block == nullptr || !block->occupied)
    {
        return;
    }

    block->occupied = false;

    if (block->next != nullptr && !block->next->occupied)
    {
        auto* next = block->next;
        block->size += next->size;
        block->next = next->next;
        if (next->next != nullptr)
        {
            next->next->prev = block;
        }
        delete next;
    }

    if (block->prev != nullptr && !block->prev->occupied)
    {
        auto* prev = block->prev;
        prev->size += block->size;
        prev->next = block->next;
        if (block->next != nullptr)
        {
            block->next->prev = prev;
        }
        delete block;
    }
}

void allocator_red_black_tree::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard lock(get_meta(_trusted_memory)->mutex);
    get_meta(_trusted_memory)->fit_mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info() const
{
    std::lock_guard lock(get_meta(_trusted_memory)->mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;
    for (auto it = begin(); it != end(); ++it)
    {
        result.push_back({it.size(), it.occupied()});
    }
    return result;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::begin() const noexcept
{
    return rb_iterator(_trusted_memory);
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::end() const noexcept
{
    return rb_iterator();
}

bool allocator_red_black_tree::rb_iterator::operator==(const allocator_red_black_tree::rb_iterator& other) const noexcept
{
    return _block_ptr == other._block_ptr;
}

bool allocator_red_black_tree::rb_iterator::operator!=(const allocator_red_black_tree::rb_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_red_black_tree::rb_iterator& allocator_red_black_tree::rb_iterator::operator++() & noexcept
{
    if (_block_ptr != nullptr)
    {
        _block_ptr = reinterpret_cast<extent*>(_block_ptr)->next;
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
    return _block_ptr == nullptr ? 0 : reinterpret_cast<extent*>(_block_ptr)->size;
}

void* allocator_red_black_tree::rb_iterator::operator*() const noexcept
{
    if (_block_ptr == nullptr || _trusted == nullptr)
    {
        return nullptr;
    }

    const auto* meta = get_meta(_trusted);
    return meta->region + reinterpret_cast<extent*>(_block_ptr)->offset;
}

allocator_red_black_tree::rb_iterator::rb_iterator() :
    _block_ptr(nullptr),
    _trusted(nullptr)
{
}

allocator_red_black_tree::rb_iterator::rb_iterator(void* trusted) :
    _block_ptr(trusted == nullptr ? nullptr : get_meta(trusted)->head),
    _trusted(trusted)
{
}

bool allocator_red_black_tree::rb_iterator::occupied() const noexcept
{
    return _block_ptr != nullptr && reinterpret_cast<extent*>(_block_ptr)->occupied;
}
