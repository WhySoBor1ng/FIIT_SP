#include "../include/allocator_buddies_system.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace
{
    constexpr unsigned char max_supported_order = static_cast<unsigned char>(sizeof(size_t) * 8 - 1);

    struct local_block_metadata final
    {
        bool occupied : 1;
        unsigned char size : 7;
    };

    struct buddies_allocator_meta final
    {
        std::pmr::memory_resource* parent_allocator;
        allocator_with_fit_mode::fit_mode fit_mode;
        unsigned char max_order;
        unsigned char min_order;
        std::mutex mutex;
        void* free_heads[sizeof(size_t) * 8]{};
    };

    local_block_metadata* get_block_metadata(void* block) noexcept
    {
        return reinterpret_cast<local_block_metadata*>(block);
    }

    const local_block_metadata* get_block_metadata(const void* block) noexcept
    {
        return reinterpret_cast<const local_block_metadata*>(block);
    }

    buddies_allocator_meta* get_meta(void* trusted) noexcept
    {
        return reinterpret_cast<buddies_allocator_meta*>(trusted);
    }

    const buddies_allocator_meta* get_meta(const void* trusted) noexcept
    {
        return reinterpret_cast<const buddies_allocator_meta*>(trusted);
    }

    unsigned char* region_begin(void* trusted) noexcept
    {
        return reinterpret_cast<unsigned char*>(trusted) + sizeof(buddies_allocator_meta);
    }

    const unsigned char* region_begin(const void* trusted) noexcept
    {
        return reinterpret_cast<const unsigned char*>(trusted) + sizeof(buddies_allocator_meta);
    }

    size_t order_to_size(unsigned char order) noexcept
    {
        return static_cast<size_t>(1) << order;
    }

    void*& next_free_ref(void* block) noexcept
    {
        return *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(block) + sizeof(local_block_metadata));
    }

    void* const& next_free_ref(const void* block) noexcept
    {
        return *reinterpret_cast<void* const*>(reinterpret_cast<const unsigned char*>(block) + sizeof(local_block_metadata));
    }

    unsigned char required_order(size_t size) noexcept
    {
        return static_cast<unsigned char>(__detail::nearest_greater_k_of_2(size));
    }

    unsigned char normalize_order(size_t size) noexcept
    {
        if (size == 0)
        {
            return 0;
        }
        return static_cast<unsigned char>(__detail::nearest_greater_k_of_2(size));
    }

    void push_free_block(buddies_allocator_meta* meta, void* block, unsigned char order) noexcept
    {
        auto* header = get_block_metadata(block);
        header->occupied = false;
        header->size = order;
        next_free_ref(block) = meta->free_heads[order];
        meta->free_heads[order] = block;
    }

    void remove_free_block(buddies_allocator_meta* meta, void* block, unsigned char order) noexcept
    {
        void* previous = nullptr;
        void* current = meta->free_heads[order];

        while (current != nullptr && current != block)
        {
            previous = current;
            current = next_free_ref(current);
        }

        if (current == nullptr)
        {
            return;
        }

        if (previous == nullptr)
        {
            meta->free_heads[order] = next_free_ref(current);
        }
        else
        {
            next_free_ref(previous) = next_free_ref(current);
        }
    }

    void* clone_state(const void* source_trusted)
    {
        const auto* source_meta = get_meta(source_trusted);
        auto* parent_allocator = source_meta->parent_allocator == nullptr
            ? std::pmr::get_default_resource()
            : source_meta->parent_allocator;

        const auto total_size = sizeof(buddies_allocator_meta) + order_to_size(source_meta->max_order);
        void* new_memory = parent_allocator->allocate(total_size, alignof(std::max_align_t));
        std::memcpy(new_memory, source_trusted, total_size);

        auto* new_meta = get_meta(new_memory);
        new (&new_meta->mutex) std::mutex();
        new_meta->parent_allocator = parent_allocator;

        const auto offset = reinterpret_cast<unsigned char*>(new_memory)
                            - reinterpret_cast<const unsigned char*>(source_trusted);

        for (auto& head : new_meta->free_heads)
        {
            if (head != nullptr)
            {
                head = reinterpret_cast<unsigned char*>(head) + offset;
            }
        }

        auto* cursor = region_begin(new_memory);
        const auto* end = region_begin(new_memory) + order_to_size(new_meta->max_order);
        while (cursor < end)
        {
            auto* header = get_block_metadata(cursor);
            if (!header->occupied)
            {
                auto& next = next_free_ref(cursor);
                if (next != nullptr)
                {
                    next = reinterpret_cast<unsigned char*>(next) + offset;
                }
            }
            cursor += order_to_size(header->size);
        }

        return new_memory;
    }
}

allocator_buddies_system::~allocator_buddies_system()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    auto* meta = get_meta(_trusted_memory);
    auto* parent_allocator = meta->parent_allocator == nullptr
        ? std::pmr::get_default_resource()
        : meta->parent_allocator;
    const auto total_size = sizeof(buddies_allocator_meta) + order_to_size(meta->max_order);

    meta->mutex.~mutex();
    parent_allocator->deallocate(_trusted_memory, total_size, alignof(std::max_align_t));
    _trusted_memory = nullptr;
}

allocator_buddies_system::allocator_buddies_system(
    allocator_buddies_system&& other) noexcept :
    _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_buddies_system& allocator_buddies_system::operator=(
    allocator_buddies_system&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    this->~allocator_buddies_system();
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
    return *this;
}

allocator_buddies_system::allocator_buddies_system(
    size_t space_size,
    std::pmr::memory_resource* parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode) :
    _trusted_memory(nullptr)
{
    const auto min_block_size = order_to_size(min_k);
    if (space_size < min_block_size)
    {
        throw std::logic_error("allocator_buddies_system: space size is too small");
    }

    const auto max_order = normalize_order(space_size);
    auto* real_parent_allocator = parent_allocator == nullptr
        ? std::pmr::get_default_resource()
        : parent_allocator;

    const auto total_size = sizeof(buddies_allocator_meta) + order_to_size(max_order);
    _trusted_memory = real_parent_allocator->allocate(total_size, alignof(std::max_align_t));

    auto* meta = new (_trusted_memory) buddies_allocator_meta{
        real_parent_allocator,
        allocate_fit_mode,
        max_order,
        static_cast<unsigned char>(min_k),
        std::mutex{},
        {}
    };

    auto* first_block = region_begin(_trusted_memory);
    auto* header = get_block_metadata(first_block);
    header->occupied = false;
    header->size = max_order;
    next_free_ref(first_block) = nullptr;
    meta->free_heads[max_order] = first_block;
}

[[nodiscard]] void* allocator_buddies_system::do_allocate_sm(
    size_t size)
{
    std::lock_guard lock(get_meta(_trusted_memory)->mutex);

    if (size == 0)
    {
        size = 1;
    }

    auto* meta = get_meta(_trusted_memory);
    const auto needed_order = std::max(
        meta->min_order,
        required_order(sizeof(block_metadata) + sizeof(void*) + size));

    void* block = nullptr;
    unsigned char selected_order = 0;

    if (meta->fit_mode == fit_mode::the_worst_fit)
    {
        for (int order = meta->max_order; order >= needed_order; --order)
        {
            if (meta->free_heads[order] != nullptr)
            {
                selected_order = static_cast<unsigned char>(order);
                block = meta->free_heads[order];
                break;
            }
        }
    }
    else
    {
        for (unsigned char order = needed_order; order <= meta->max_order; ++order)
        {
            if (meta->free_heads[order] != nullptr)
            {
                selected_order = order;
                block = meta->free_heads[order];
                break;
            }
            if (order == meta->max_order)
            {
                break;
            }
        }
    }

    if (block == nullptr)
    {
        throw std::bad_alloc();
    }

    remove_free_block(meta, block, selected_order);

    while (selected_order > needed_order)
    {
        --selected_order;
        const auto half_size = order_to_size(selected_order);
        auto* right_buddy = reinterpret_cast<unsigned char*>(block) + half_size;
        push_free_block(meta, right_buddy, selected_order);

        auto* left_header = get_block_metadata(block);
        left_header->occupied = false;
        left_header->size = selected_order;
    }

    auto* header = get_block_metadata(block);
    header->occupied = true;
    header->size = selected_order;

    return reinterpret_cast<unsigned char*>(block) + sizeof(block_metadata) + sizeof(void*);
}

void allocator_buddies_system::do_deallocate_sm(void* at)
{
    if (at == nullptr)
    {
        return;
    }

    auto* meta = get_meta(_trusted_memory);
    std::lock_guard lock(meta->mutex);

    auto* block = reinterpret_cast<unsigned char*>(at) - (sizeof(block_metadata) + sizeof(void*));
    if (block < region_begin(_trusted_memory)
        || block >= region_begin(_trusted_memory) + order_to_size(meta->max_order))
    {
        return;
    }

    auto* header = get_block_metadata(block);
    if (!header->occupied)
    {
        return;
    }

    auto order = header->size;
    header->occupied = false;

    auto* base = region_begin(_trusted_memory);
    while (order < meta->max_order)
    {
        const auto block_size = order_to_size(order);
        const auto offset = static_cast<size_t>(block - base);
        auto* buddy = base + (offset ^ block_size);

        if (buddy < base || buddy >= base + order_to_size(meta->max_order))
        {
            break;
        }

        auto* buddy_header = get_block_metadata(buddy);
        if (buddy_header->occupied || buddy_header->size != order)
        {
            break;
        }

        remove_free_block(meta, buddy, order);
        block = std::min(block, buddy);
        ++order;
        auto* merged_header = get_block_metadata(block);
        merged_header->occupied = false;
        merged_header->size = order;
    }

    push_free_block(meta, block, order);
}

allocator_buddies_system::allocator_buddies_system(const allocator_buddies_system& other) :
    _trusted_memory(nullptr)
{
    std::lock_guard lock(get_meta(other._trusted_memory)->mutex);
    _trusted_memory = clone_state(other._trusted_memory);
}

allocator_buddies_system& allocator_buddies_system::operator=(const allocator_buddies_system& other)
{
    if (this == &other)
    {
        return *this;
    }

    auto* other_meta = get_meta(other._trusted_memory);
    std::lock_guard lock(other_meta->mutex);
    void* cloned = clone_state(other._trusted_memory);

    this->~allocator_buddies_system();
    _trusted_memory = cloned;
    return *this;
}

bool allocator_buddies_system::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    if (this == &other)
    {
        return true;
    }

    auto* other_ptr = dynamic_cast<const allocator_buddies_system*>(&other);
    return other_ptr != nullptr && other_ptr == this;
}

inline void allocator_buddies_system::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard lock(get_meta(_trusted_memory)->mutex);
    get_meta(_trusted_memory)->fit_mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info() const noexcept
{
    std::lock_guard lock(get_meta(_trusted_memory)->mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;
    for (auto it = begin(); it != end(); ++it)
    {
        result.push_back({it.size(), it.occupied()});
    }
    return result;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::begin() const noexcept
{
    return buddy_iterator(region_begin(_trusted_memory));
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::end() const noexcept
{
    return buddy_iterator(region_begin(_trusted_memory) + order_to_size(get_meta(_trusted_memory)->max_order));
}

bool allocator_buddies_system::buddy_iterator::operator==(const allocator_buddies_system::buddy_iterator& other) const noexcept
{
    return _block == other._block;
}

bool allocator_buddies_system::buddy_iterator::operator!=(const allocator_buddies_system::buddy_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_buddies_system::buddy_iterator& allocator_buddies_system::buddy_iterator::operator++() & noexcept
{
    if (_block != nullptr)
    {
        auto* header = get_block_metadata(_block);
        _block = reinterpret_cast<unsigned char*>(_block) + order_to_size(header->size);
    }
    return *this;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::buddy_iterator::operator++(int)
{
    auto copy = *this;
    ++(*this);
    return copy;
}

size_t allocator_buddies_system::buddy_iterator::size() const noexcept
{
    return _block == nullptr ? 0 : order_to_size(get_block_metadata(_block)->size);
}

bool allocator_buddies_system::buddy_iterator::occupied() const noexcept
{
    return _block != nullptr && get_block_metadata(_block)->occupied;
}

void* allocator_buddies_system::buddy_iterator::operator*() const noexcept
{
    return _block == nullptr
        ? nullptr
        : reinterpret_cast<unsigned char*>(_block) + sizeof(block_metadata) + sizeof(void*);
}

allocator_buddies_system::buddy_iterator::buddy_iterator(void* start) :
    _block(start)
{
}

allocator_buddies_system::buddy_iterator::buddy_iterator() :
    _block(nullptr)
{
}
