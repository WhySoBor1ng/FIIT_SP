#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BUDDIES_SYSTEM_H
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BUDDIES_SYSTEM_H

#include <allocator_with_fit_mode.h>
#include <allocator_test_utils.h>
#include <pp_allocator.h>
#include <cstdint>
#include <iterator>
#include <mutex>

struct allocator_buddies_header
{
    std::pmr::memory_resource* parent_alloc;
    allocator_with_fit_mode::fit_mode fit_mode;
    uint8_t max_pow;
    std::mutex mutex;
    void** free_blocks;
    void* mem_st;
};

namespace __detail
{
    constexpr size_t nearest_greater_k_of_2(size_t size) noexcept
    {
        size_t exponent = 0;
        size_t current = 1;
        while (current < size)
        {
            current <<= 1;
            ++exponent;
        }
        return exponent;
    }
}

class allocator_buddies_system final:
    public smart_mem_resource,
    public allocator_test_utils,
    public allocator_with_fit_mode
{
    struct block_metadata
    {
        bool occupied : 1;
        unsigned char size : 7;
    };

    void *_trusted_memory;

    static constexpr size_t occupied_block_metadata_size = sizeof(block_metadata);
    static constexpr size_t free_block_metadata_size = sizeof(block_metadata) + sizeof(void*);
    static constexpr size_t min_k = __detail::nearest_greater_k_of_2(free_block_metadata_size);

public:

    explicit allocator_buddies_system(
            size_t space_size_power_of_two,
            std::pmr::memory_resource *parent_allocator = nullptr,
            allocator_with_fit_mode::fit_mode allocate_fit_mode = allocator_with_fit_mode::fit_mode::first_fit);

    allocator_buddies_system(
        allocator_buddies_system const &other) = delete;
    
    allocator_buddies_system &operator=(
        allocator_buddies_system const &other) = delete;
    
    allocator_buddies_system(
        allocator_buddies_system &&other) noexcept = delete;
    
    allocator_buddies_system &operator=(
        allocator_buddies_system &&other) noexcept = delete;

    ~allocator_buddies_system() override;

private:
    
    [[nodiscard]] void *do_allocate_sm(
        size_t size) override;
    
    void do_deallocate_sm(
        void *at) override;

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    inline void set_fit_mode(
        allocator_with_fit_mode::fit_mode mode) override;


    std::vector<allocator_test_utils::block_info> get_blocks_info() const noexcept override;

private:
    [[nodiscard]] auto get_parent_alloc() const;
    [[nodiscard]] auto get_fit_mode() const;
    [[nodiscard]] auto get_max_pow() const;
    [[nodiscard]] auto get_full_size() const;
    [[nodiscard]] auto get_free_lists_count() const;
    [[nodiscard]] auto get_reserved_size() const;
    [[nodiscard]] auto get_mutex() const;
    [[nodiscard]] auto get_head() const;
    [[nodiscard]] auto get_memory_start() const;
    [[nodiscard]] auto get_memory_end() const;

    static auto get_metadata(void* block);
    static auto get_next_block(void* block);
    static auto get_block_size(void* block);
    static auto get_buddy(void* block, uint8_t rank, void* memory_start);
    static auto choose_rank(size_t size);

    void insert_free_block(void* block, uint8_t rank) const;
    void remove_free_block(void* block, uint8_t rank) const;

    [[nodiscard]] auto find_suitable_block(uint8_t required_rank) const;
    [[nodiscard]] auto split_block(void* block, uint8_t current_rank, uint8_t target_rank) const;

    std::vector<allocator_test_utils::block_info> get_blocks_info_inner() const override;

    class buddy_iterator
    {
        void* _block;
        void* _end;

    public:

        using iterator_category = std::forward_iterator_tag;
        using value_type = void*;
        using reference = void*&;
        using pointer = void**;
        using difference_type = ptrdiff_t;

        bool operator==(const buddy_iterator&) const noexcept;

        bool operator!=(const buddy_iterator&) const noexcept;

        buddy_iterator& operator++() & noexcept;

        buddy_iterator operator++(int n);

        size_t size() const noexcept;

        bool occupied() const noexcept;

        void* operator*() const noexcept;

        buddy_iterator();

        buddy_iterator(void* start, void* end);
    };

    friend class buddy_iterator;

    buddy_iterator begin() const noexcept;

    buddy_iterator end() const noexcept;
    
};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BUDDIES_SYSTEM_H
