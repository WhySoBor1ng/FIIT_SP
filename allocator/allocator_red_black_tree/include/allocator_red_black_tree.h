#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_RED_BLACK_TREE_H
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_RED_BLACK_TREE_H

#include <pp_allocator.h>
#include <allocator_test_utils.h>
#include <allocator_with_fit_mode.h>
#include <iterator>
#include <mutex>

class allocator_red_black_tree final:
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

    enum class block_color : unsigned char
    { RED, BLACK };

    struct block_data
    {
        bool occupied : 4;
        block_color color : 4;
    };

    void *_trusted_memory;

    static constexpr const size_t allocator_metadata_size = sizeof(allocator_dbg_helper*) + sizeof(fit_mode) + sizeof(size_t) + sizeof(std::mutex) + sizeof(void*);
    static constexpr const size_t occupied_block_metadata_size = sizeof(block_data) + 3 * sizeof(void*);
    static constexpr const size_t free_block_metadata_size = sizeof(block_data) + 5 * sizeof(void*);

public:
    
    ~allocator_red_black_tree() override;
    
    allocator_red_black_tree(
        allocator_red_black_tree const &other) = delete;
    
    allocator_red_black_tree &operator=(
        allocator_red_black_tree const &other) = delete;
    
    allocator_red_black_tree(
        allocator_red_black_tree &&other) noexcept = delete;
    
    allocator_red_black_tree &operator=(
        allocator_red_black_tree &&other) noexcept = delete;

public:
    
    explicit allocator_red_black_tree(
            size_t space_size,
            std::pmr::memory_resource *parent_allocator = nullptr,
            allocator_with_fit_mode::fit_mode allocate_fit_mode = allocator_with_fit_mode::fit_mode::first_fit);

private:
    
    [[nodiscard]] void *do_allocate_sm(
        size_t size) override;
    
    void do_deallocate_sm(
        void *at) override;

    bool do_is_equal(const std::pmr::memory_resource&) const noexcept override;

    std::vector<allocator_test_utils::block_info> get_blocks_info() const override;
    
    inline void set_fit_mode(allocator_with_fit_mode::fit_mode mode) override;

private:
    [[nodiscard]] auto get_parent_alloc() const;
    [[nodiscard]] auto get_fit_mode() const;
    [[nodiscard]] auto get_full_size() const;
    [[nodiscard]] auto get_mutex() const;
    [[nodiscard]] auto get_root() const;
    [[nodiscard]] auto get_memory_start() const;
    [[nodiscard]] auto get_memory_end() const;

    static auto get_metadata(void* block);
    static auto get_size_ptr(void* block);
    static auto get_prev_block(void* block);
    static auto get_parent_node(void* block);
    static auto get_left_node(void* block);
    static auto get_right_node(void* block);

    static auto get_block_size(void* block);
    static auto is_block_free(void* block);
    static void set_prev_block(void* block, void* prev);
    static void set_color(void* block, block_color color);
    static auto get_color(void* block);

    void insert_free_block(void* block) const;
    void remove_free_block(void* block) const;
    [[nodiscard]] block_choice find_block(size_t needed) const;
    void collect_suitable_block(void* block, size_t needed, block_choice& choice) const;
    [[nodiscard]] static auto minimum(void* block);

    std::vector<allocator_test_utils::block_info> get_blocks_info_inner() const override;

    class rb_iterator
    {
        void* _block_ptr;
        void* _trusted;

    public:

        using iterator_category = std::forward_iterator_tag;
        using value_type = void*;
        using reference = void*&;
        using pointer = void**;
        using difference_type = ptrdiff_t;

        bool operator==(const rb_iterator&) const noexcept;

        bool operator!=(const rb_iterator&) const noexcept;

        rb_iterator& operator++() & noexcept;

        rb_iterator operator++(int n);

        size_t size() const noexcept;

        void* operator*() const noexcept;

        bool occupied()const noexcept;

        rb_iterator();

        rb_iterator(void* trusted);
    };

    friend class rb_iterator;

    rb_iterator begin() const noexcept;
    rb_iterator end() const noexcept;

};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_RED_BLACK_TREE_H
