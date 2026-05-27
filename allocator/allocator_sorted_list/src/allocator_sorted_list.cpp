#include "../include/allocator_sorted_list.h"

#include <new>
#include <stdexcept>

auto allocator_sorted_list::get_parent_alloc() const
{
    return static_cast<memory_resource**>(_trusted_memory);
}

auto allocator_sorted_list::get_fit_mode() const
{
    return reinterpret_cast<fit_mode*>(static_cast<char*>(_trusted_memory) + sizeof(memory_resource*));
}

auto allocator_sorted_list::get_full_size() const
{
    return reinterpret_cast<size_t*>(static_cast<char*>(_trusted_memory) + sizeof(memory_resource*) + sizeof(fit_mode));
}

auto allocator_sorted_list::get_mutex() const
{
    return reinterpret_cast<std::mutex*>(static_cast<char*>(_trusted_memory) + sizeof(memory_resource*) + sizeof(fit_mode) + sizeof(size_t));
}

auto allocator_sorted_list::get_head() const
{
    return reinterpret_cast<void**>(static_cast<char*>(_trusted_memory) + sizeof(memory_resource*) + sizeof(fit_mode) + sizeof(size_t) + sizeof(std::mutex));
}

auto allocator_sorted_list::get_memory_start() const
{
    return static_cast<char*>(_trusted_memory) + allocator_metadata_size;
}

auto allocator_sorted_list::get_memory_end() const
{
    return static_cast<char*>(_trusted_memory) + *get_full_size();
}

auto allocator_sorted_list::get_size_ptr(void* block)
{
    return static_cast<size_t*>(block);
}

auto allocator_sorted_list::get_next_ptr(void* block)
{
    return reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(size_t));
}

auto allocator_sorted_list::get_block_size(void* block)
{
    return *get_size_ptr(block) & ~size_t(1);
}

auto allocator_sorted_list::is_block_free(void* block)
{
    return (*get_size_ptr(block) & size_t(1)) == 0;
}

void allocator_sorted_list::insert_free_block(void* block) const
{
    void* prev = nullptr;
    void* current = *get_head();

    while (current != nullptr && current < block)
    {
        prev = current;
        current = *get_next_ptr(current);
    }

    *get_next_ptr(block) = current;
    if (prev == nullptr)
    {
        *get_head() = block;
    }
    else
    {
        *get_next_ptr(prev) = block;
    }
}

void allocator_sorted_list::remove_free_block(void* block, void* prev) const
{
    void* next = *get_next_ptr(block);
    if (prev == nullptr)
    {
        *get_head() = next;
    }
    else
    {
        *get_next_ptr(prev) = next;
    }
}

allocator_sorted_list::block_choice allocator_sorted_list::find_block(size_t needed) const
{
    block_choice choice{nullptr, nullptr, 0};
    void* prev = nullptr;

    for (void* block = *get_head(); block != nullptr; prev = block, block = *get_next_ptr(block))
    {
        size_t block_size = get_block_size(block);
        if (block_size < needed)
        {
            continue;
        }

        if (*get_fit_mode() == fit_mode::first_fit)
        {
            return {block, prev, block_size};
        }

        if (choice.block == nullptr)
        {
            choice = {block, prev, block_size};
            continue;
        }

        if (*get_fit_mode() == fit_mode::the_best_fit && block_size < choice.size)
        {
            choice = {block, prev, block_size};
        }

        if (*get_fit_mode() == fit_mode::the_worst_fit && block_size > choice.size)
        {
            choice = {block, prev, block_size};
        }
    }

    return choice;
}

allocator_sorted_list::~allocator_sorted_list()
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

allocator_sorted_list::allocator_sorted_list(
    size_t space_size,
    std::pmr::memory_resource* parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size < allocator_metadata_size + block_metadata_size)
    {
        throw std::logic_error("space size is too small");
    }

    _trusted_memory = parent_allocator != nullptr
        ? parent_allocator->allocate(space_size)
        : ::operator new(space_size);

    *get_parent_alloc() = parent_allocator;
    *get_fit_mode() = allocate_fit_mode;
    *get_full_size() = space_size;
    new (get_mutex()) std::mutex();

    void* first_block = get_memory_start();
    *get_size_ptr(first_block) = get_memory_end() - static_cast<char*>(first_block);
    *get_next_ptr(first_block) = nullptr;
    *get_head() = first_block;
}

[[nodiscard]] void* allocator_sorted_list::do_allocate_sm(size_t size)
{
    std::lock_guard lock(*get_mutex());

    size_t needed = size + sizeof(size_t);
    block_choice choice = find_block(needed);
    if (choice.block == nullptr)
    {
        throw std::bad_alloc();
    }

    remove_free_block(choice.block, choice.prev);

    size_t remainder = choice.size - needed;
    size_t allocated_size = choice.size;
    if (remainder >= block_metadata_size)
    {
        void* free_block = static_cast<char*>(choice.block) + needed;
        *get_size_ptr(free_block) = remainder;
        *get_next_ptr(free_block) = nullptr;
        insert_free_block(free_block);
        allocated_size = needed;
    }

    *get_size_ptr(choice.block) = allocated_size | size_t(1);
    return static_cast<char*>(choice.block) + sizeof(size_t);
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    if (this == &other)
    {
        return true;
    }

    auto* rhs = dynamic_cast<const allocator_sorted_list*>(&other);
    return rhs != nullptr && rhs->_trusted_memory == _trusted_memory;
}

void allocator_sorted_list::do_deallocate_sm(void* at)
{
    std::lock_guard lock(*get_mutex());

    if (at == nullptr)
    {
        return;
    }

    char* block = static_cast<char*>(at) - sizeof(size_t);
    if (block < get_memory_start() || block >= get_memory_end())
    {
        throw std::invalid_argument("block bounds are invalid");
    }

    size_t raw_size = *get_size_ptr(block);
    if ((raw_size & size_t(1)) == 0)
    {
        throw std::invalid_argument("block is free");
    }

    size_t block_size = raw_size & ~size_t(1);
    *get_size_ptr(block) = block_size;

    void* prev = nullptr;
    void* current = *get_head();
    while (current != nullptr && current < block)
    {
        prev = current;
        current = *get_next_ptr(current);
    }

    if (current != nullptr && block + block_size == current)
    {
        block_size += get_block_size(current);
        *get_size_ptr(block) = block_size;
        current = *get_next_ptr(current);
    }

    if (prev != nullptr && static_cast<char*>(prev) + get_block_size(prev) == block)
    {
        *get_size_ptr(prev) = get_block_size(prev) + block_size;
        *get_next_ptr(prev) = current;
        return;
    }

    *get_next_ptr(block) = current;
    if (prev == nullptr)
    {
        *get_head() = block;
    }
    else
    {
        *get_next_ptr(prev) = block;
    }
}

inline void allocator_sorted_list::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard lock(*get_mutex());
    *get_fit_mode() = mode;
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    std::lock_guard lock(*get_mutex());
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
{
    std::vector<block_info> result;

    for (char* block = get_memory_start(); block < get_memory_end(); block += get_block_size(block))
    {
        size_t block_size = get_block_size(block);
        if (block + block_size > get_memory_end())
        {
            break;
        }

        result.push_back({block_size, !is_block_free(block)});
    }

    return result;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept
{
    return {*get_head()};
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() noexcept
{
    return {};
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept
{
    return {const_cast<allocator_sorted_list*>(this)};
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept
{
    sorted_iterator it;
    it.set_trusted_memory(const_cast<allocator_sorted_list*>(this));
    return it;
}

bool allocator_sorted_list::sorted_free_iterator::operator==(const allocator_sorted_list::sorted_free_iterator& other) const noexcept
{
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(const allocator_sorted_list::sorted_free_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_free_iterator& allocator_sorted_list::sorted_free_iterator::operator++() & noexcept
{
    if (_free_ptr != nullptr)
    {
        _free_ptr = *allocator_sorted_list::get_next_ptr(_free_ptr);
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
    return _free_ptr == nullptr ? 0 : allocator_sorted_list::get_block_size(_free_ptr);
}

void* allocator_sorted_list::sorted_free_iterator::operator*() const noexcept
{
    return _free_ptr == nullptr ? nullptr : static_cast<char*>(_free_ptr) + sizeof(size_t);
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator() : _free_ptr(nullptr) {}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void* trusted) : _free_ptr(trusted) {}

bool allocator_sorted_list::sorted_iterator::operator==(const allocator_sorted_list::sorted_iterator& other) const noexcept
{
    return _current_ptr == other._current_ptr && _trusted_memory == other._trusted_memory;
}

bool allocator_sorted_list::sorted_iterator::operator!=(const allocator_sorted_list::sorted_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_iterator& allocator_sorted_list::sorted_iterator::operator++() & noexcept
{
    if (_current_ptr == nullptr || _trusted_memory == nullptr)
    {
        return *this;
    }

    auto* allocator = static_cast<allocator_sorted_list*>(_trusted_memory);
    _current_ptr = static_cast<char*>(_current_ptr) + allocator_sorted_list::get_block_size(_current_ptr);
    if (_current_ptr >= allocator->get_memory_end())
    {
        _current_ptr = nullptr;
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
    return _current_ptr == nullptr ? 0 : allocator_sorted_list::get_block_size(_current_ptr);
}

void* allocator_sorted_list::sorted_iterator::operator*() const noexcept
{
    return occupied() ? static_cast<char*>(_current_ptr) + sizeof(size_t) : nullptr;
}

allocator_sorted_list::sorted_iterator::sorted_iterator()
    : _free_ptr(nullptr), _current_ptr(nullptr), _trusted_memory(nullptr)
{
}

allocator_sorted_list::sorted_iterator::sorted_iterator(void* trusted)
    : _free_ptr(nullptr), _current_ptr(nullptr), _trusted_memory(trusted)
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    auto* allocator = static_cast<allocator_sorted_list*>(_trusted_memory);
    _current_ptr = allocator->get_memory_start() < allocator->get_memory_end()
        ? allocator->get_memory_start()
        : nullptr;
}

void allocator_sorted_list::sorted_iterator::set_trusted_memory(void* mem) noexcept
{
    _trusted_memory = mem;
    _free_ptr = nullptr;
    _current_ptr = nullptr;
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept
{
    return _current_ptr != nullptr && !allocator_sorted_list::is_block_free(_current_ptr);
}
