#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BOUNDARY_TAGS_H
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BOUNDARY_TAGS_H

#include <allocator_test_utils.h>
#include <allocator_with_fit_mode.h>
#include <pp_allocator.h>
#include <iterator>
#include <mutex>

class allocator_boundary_tags final :
    public smart_mem_resource,
    public allocator_test_utils,
    public allocator_with_fit_mode
{

private:

    struct block_choice
    {
        void* block;
        size_t size;
    };

    static constexpr const size_t allocator_metadata_size = sizeof(memory_resource*) + sizeof(allocator_with_fit_mode::fit_mode) +
                                                            sizeof(size_t) + sizeof(std::mutex) + sizeof(void*);

    static constexpr const size_t occupied_block_metadata_size = sizeof(size_t) + sizeof(void*) + sizeof(void*) + sizeof(void*);

    static constexpr const size_t free_block_metadata_size = 0;

    void *_trusted_memory;

public:
    
    ~allocator_boundary_tags() override;
    
    allocator_boundary_tags(allocator_boundary_tags const &other) = delete;
    
    allocator_boundary_tags &operator=(allocator_boundary_tags const &other) = delete;
    
    allocator_boundary_tags(
        allocator_boundary_tags &&other) noexcept = delete;
    
    allocator_boundary_tags &operator=(
        allocator_boundary_tags &&other) noexcept = delete;

public:
    
    explicit allocator_boundary_tags(
            size_t space_size,
            std::pmr::memory_resource *parent_allocator = nullptr,
            allocator_with_fit_mode::fit_mode allocate_fit_mode = allocator_with_fit_mode::fit_mode::first_fit);

private:
    
    [[nodiscard]] void *do_allocate_sm(
        size_t bytes) override;
    
    void do_deallocate_sm(
        void *at) override;

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

public:
    
    inline void set_fit_mode(
        allocator_with_fit_mode::fit_mode mode) override;

public:
    
    std::vector<allocator_test_utils::block_info> get_blocks_info() const override;
private:
    [[nodiscard]] auto get_parent_alloc() const;
    [[nodiscard]] auto get_fit_mode() const;
    [[nodiscard]] auto get_full_size() const;
    [[nodiscard]] auto get_mutex() const;
    [[nodiscard]] auto get_head() const;
    [[nodiscard]] auto get_memory_start() const;
    [[nodiscard]] auto get_memory_end() const;

    static auto get_free_prev_block(void* block);
    static auto get_free_next_block(void* block);

    static auto get_prev_block(void* block);
    static auto set_prev_block(void* block, void* next_block);

    static auto get_size_block_ptr(void* block);
    static auto get_size_block(void* block);

    void insert_free_block(void* block) const;
    void remove_free_block(void* block) const;
    [[nodiscard]] block_choice find_block(size_t needed) const;

    static bool is_block_free(void* block);
private:

    std::vector<allocator_test_utils::block_info> get_blocks_info_inner() const override;

    class boundary_iterator
    {
        void* _occupied_ptr;
        bool _occupied;
        void* _trusted_memory;

    public:

        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = void*;
        using reference = void*&;
        using pointer = void**;
        using difference_type = ptrdiff_t;

        bool operator==(const boundary_iterator&) const noexcept;

        bool operator!=(const boundary_iterator&) const noexcept;

        boundary_iterator& operator++() & noexcept;

        boundary_iterator& operator--() & noexcept;

        boundary_iterator operator++(int n);

        boundary_iterator operator--(int n);

        size_t size() const noexcept;

        bool occupied() const noexcept;

        void* operator*() const noexcept;

        void* get_ptr() const noexcept;

        boundary_iterator();

        boundary_iterator(void* trusted);
    };

    friend class boundary_iterator;

    boundary_iterator begin() const noexcept;

    static boundary_iterator end() noexcept;
};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BOUNDARY_TAGS_H
