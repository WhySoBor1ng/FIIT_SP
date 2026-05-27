#include "../include/allocator_buddies_system.h"

#include <new>
#include <stdexcept>

constexpr size_t rank_to_size(const uint8_t rank) noexcept
{
    return static_cast<size_t>(1) << rank;
}

auto allocator_buddies_system::get_parent_alloc() const
{
    return &static_cast<allocator_buddies_header*>(_trusted_memory)->parent_alloc;
}

auto allocator_buddies_system::get_fit_mode() const
{
    return &static_cast<allocator_buddies_header*>(_trusted_memory)->fit_mode;
}

auto allocator_buddies_system::get_max_pow() const
{
    return &static_cast<allocator_buddies_header*>(_trusted_memory)->max_pow;
}

auto allocator_buddies_system::get_full_size() const
{
    return rank_to_size(*get_max_pow());
}

auto allocator_buddies_system::get_free_lists_count() const
{
    return static_cast<size_t>(*get_max_pow() - min_k + 1);
}

auto allocator_buddies_system::get_reserved_size() const
{
    return sizeof(allocator_buddies_header) + get_free_lists_count() * sizeof(void*) + get_full_size();
}

auto allocator_buddies_system::get_mutex() const
{
    return &static_cast<allocator_buddies_header*>(_trusted_memory)->mutex;
}

auto allocator_buddies_system::get_head() const
{
    return static_cast<allocator_buddies_header*>(_trusted_memory)->free_blocks;
}

auto allocator_buddies_system::get_memory_start() const
{
    return static_cast<char*>(static_cast<allocator_buddies_header*>(_trusted_memory)->mem_st);
}

auto allocator_buddies_system::get_memory_end() const
{
    return get_memory_start() + get_full_size();
}

auto allocator_buddies_system::get_metadata(void* block)
{
    return static_cast<block_metadata*>(block);
}

auto allocator_buddies_system::get_next_block(void* block)
{
    return reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(block_metadata));
}

auto allocator_buddies_system::get_block_size(void* block)
{
    return rank_to_size(get_metadata(block)->size);
}

auto allocator_buddies_system::get_buddy(void* block, uint8_t rank, void* memory_start)
{
    auto offset = static_cast<size_t>(static_cast<char*>(block) - static_cast<char*>(memory_start));
    size_t buddy_offset = offset ^ rank_to_size(rank);
    return static_cast<void*>(static_cast<char*>(memory_start) + buddy_offset);
}

auto allocator_buddies_system::choose_rank(size_t size)
{
    if (size == 0)
    {
        return min_k;
    }

    auto rank = static_cast<uint8_t>(__detail::nearest_greater_k_of_2(size));
    return rank < min_k ? min_k : rank;
}

void allocator_buddies_system::insert_free_block(void* block, uint8_t rank) const
{
    get_metadata(block)->occupied = false;
    get_metadata(block)->size = rank;
    *get_next_block(block) = get_head()[rank - min_k];
    get_head()[rank - min_k] = block;
}

void allocator_buddies_system::remove_free_block(void* block, uint8_t rank) const
{
    void** head = get_head() + (rank - min_k);
    void* current = *head;
    void* previous = nullptr;

    while (current != nullptr && current != block)
    {
        previous = current;
        current = *get_next_block(current);
    }

    if (current == nullptr)
    {
        return;
    }

    void* next = *get_next_block(current);
    if (previous == nullptr)
    {
        *head = next;
    }
    else
    {
        *get_next_block(previous) = next;
    }
}

auto allocator_buddies_system::find_suitable_block(uint8_t required_rank) const
{
    switch (*get_fit_mode())
    {
        case fit_mode::first_fit:
        case fit_mode::the_best_fit:
            for (uint8_t rank = required_rank; rank <= *get_max_pow(); ++rank)
            {
                if (get_head()[rank - min_k] != nullptr)
                {
                    return rank;
                }
            }
            break;
        case fit_mode::the_worst_fit:
            for (int rank = *get_max_pow(); rank >= required_rank; --rank)
            {
                if (get_head()[rank - min_k] != nullptr)
                {
                    return static_cast<uint8_t>(rank);
                }
            }
            break;
    }

    return static_cast<uint8_t>(0);
}

auto allocator_buddies_system::split_block(void* block, uint8_t current_rank, uint8_t target_rank) const
{
    remove_free_block(block, current_rank);

    while (current_rank > target_rank)
    {
        --current_rank;
        size_t half_size = rank_to_size(current_rank);
        void* buddy = static_cast<char*>(block) + half_size;

        get_metadata(block)->occupied = false;
        get_metadata(block)->size = current_rank;
        insert_free_block(buddy, current_rank);
    }

    return block;
}

allocator_buddies_system::~allocator_buddies_system()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    auto* header = static_cast<allocator_buddies_header*>(_trusted_memory);
    header->mutex.~mutex();

    if (header->parent_alloc != nullptr)
    {
        header->parent_alloc->deallocate(_trusted_memory, get_reserved_size());
    }
    else
    {
        ::operator delete(_trusted_memory);
    }
}

allocator_buddies_system::allocator_buddies_system(
    size_t space_size,
    std::pmr::memory_resource* parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
    : _trusted_memory(nullptr)
{
    uint8_t max_k = choose_rank(space_size);
    if (space_size < rank_to_size(min_k))
    {
        throw std::logic_error("space size is too small");
    }

    auto free_lists_count = max_k - min_k + 1;
    size_t reserved_size = sizeof(allocator_buddies_header) + free_lists_count * sizeof(void*) + rank_to_size(max_k);

    _trusted_memory = parent_allocator != nullptr
        ? parent_allocator->allocate(reserved_size)
        : ::operator new(reserved_size);

    auto* header = static_cast<allocator_buddies_header*>(_trusted_memory);
    header->parent_alloc = parent_allocator;
    header->fit_mode = allocate_fit_mode;
    header->max_pow = max_k;
    new (&header->mutex) std::mutex();

    header->free_blocks = reinterpret_cast<void**>(static_cast<char*>(_trusted_memory) + sizeof(allocator_buddies_header));
    header->mem_st = static_cast<char*>(static_cast<void*>(header->free_blocks + free_lists_count));

    for (size_t i = 0; i < free_lists_count; ++i)
    {
        header->free_blocks[i] = nullptr;
    }

    void* initial_block = header->mem_st;
    get_metadata(initial_block)->occupied = false;
    get_metadata(initial_block)->size = max_k;
    *get_next_block(initial_block) = nullptr;
    header->free_blocks[max_k - min_k] = initial_block;
}

[[nodiscard]] void* allocator_buddies_system::do_allocate_sm(size_t size)
{
    std::lock_guard lock(*get_mutex());

    uint8_t required_rank = choose_rank(size + occupied_block_metadata_size);
    if (required_rank > *get_max_pow())
    {
        throw std::bad_alloc();
    }

    uint8_t selected_rank = find_suitable_block(required_rank);
    if (selected_rank < required_rank)
    {
        throw std::bad_alloc();
    }

    void* block = get_head()[selected_rank - min_k];
    block = split_block(block, selected_rank, required_rank);

    get_metadata(block)->occupied = true;
    get_metadata(block)->size = required_rank;
    *get_next_block(block) = nullptr;

    return static_cast<char*>(block) + occupied_block_metadata_size;
}

void allocator_buddies_system::do_deallocate_sm(void* at)
{
    std::lock_guard lock(*get_mutex());

    if (at == nullptr)
    {
        return;
    }

    char* memory_start = get_memory_start();
    char* memory_end = get_memory_end();
    char* block = static_cast<char*>(at) - occupied_block_metadata_size;

    if (block < memory_start || block >= memory_end)
    {
        throw std::invalid_argument("block bounds are invalid");
    }

    auto* metadata = get_metadata(block);
    if (!metadata->occupied)
    {
        throw std::invalid_argument("block is already free");
    }

    uint8_t rank = metadata->size;
    metadata->occupied = false;

    while (rank < *get_max_pow())
    {
        void* buddy = get_buddy(block, rank, memory_start);
        if (buddy < memory_start || buddy >= memory_end)
        {
            break;
        }

        auto* buddy_metadata = get_metadata(buddy);
        if (buddy_metadata->occupied || buddy_metadata->size != rank)
        {
            break;
        }

        remove_free_block(buddy, rank);
        block = static_cast<char*>(buddy < block ? buddy : block);
        ++rank;
        metadata = get_metadata(block);
        metadata->occupied = false;
        metadata->size = rank;
    }

    insert_free_block(block, rank);
}

bool allocator_buddies_system::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    if (this == &other)
    {
        return true;
    }

    auto* rhs = dynamic_cast<const allocator_buddies_system*>(&other);
    return rhs != nullptr && _trusted_memory == rhs->_trusted_memory;
}

inline void allocator_buddies_system::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard lock(*get_mutex());
    *get_fit_mode() = mode;
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info() const noexcept
{
    std::lock_guard lock(*get_mutex());
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info_inner() const
{
    std::vector<block_info> result;

    for (auto it = begin(); it != end(); ++it)
    {
        result.push_back({it.size(), it.occupied()});
    }

    return result;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::begin() const noexcept
{
    return {get_memory_start(), get_memory_end()};
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::end() const noexcept
{
    return {get_memory_end(), get_memory_end()};
}

bool allocator_buddies_system::buddy_iterator::operator==(const allocator_buddies_system::buddy_iterator& other) const noexcept
{
    return _block == other._block && _end == other._end;
}

bool allocator_buddies_system::buddy_iterator::operator!=(const allocator_buddies_system::buddy_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_buddies_system::buddy_iterator& allocator_buddies_system::buddy_iterator::operator++() & noexcept
{
    if (_block == nullptr || _block == _end)
    {
        return *this;
    }

    _block = static_cast<char*>(_block) + get_block_size(_block);
    if (_block >= _end)
    {
        _block = _end;
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
    return _block == nullptr || _block == _end ? 0 : get_block_size(_block);
}

bool allocator_buddies_system::buddy_iterator::occupied() const noexcept
{
    return _block != nullptr && _block != _end && get_metadata(_block)->occupied;
}

void* allocator_buddies_system::buddy_iterator::operator*() const noexcept
{
    return _block == nullptr || _block == _end
        ? nullptr
        : static_cast<char*>(_block) + occupied_block_metadata_size;
}

allocator_buddies_system::buddy_iterator::buddy_iterator(void* start, void* end)
    : _block(start), _end(end)
{
}

allocator_buddies_system::buddy_iterator::buddy_iterator()
    : _block(nullptr), _end(nullptr)
{
}
