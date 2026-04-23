#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_GLOBAL_HEAP_H
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_GLOBAL_HEAP_H

#include <allocator_dbg_helper.h>
#include <pp_allocator.h>

class allocator_global_heap final:
    private allocator_dbg_helper,
    public smart_mem_resource
{

private:
    std::mutex mutex;

    static constexpr const size_t size_t_size = sizeof(size_t);

public:
    
    explicit allocator_global_heap() = default;
    
    ~allocator_global_heap() override = default;
    
    allocator_global_heap(
        allocator_global_heap const &other) = delete;
    
    allocator_global_heap &operator=(
        allocator_global_heap const &other) = delete;
    
    allocator_global_heap(
        allocator_global_heap &&other) noexcept = delete;
    
    allocator_global_heap &operator=(
        allocator_global_heap &&other) noexcept = delete;

private:
    
    [[nodiscard]] void *do_allocate_sm(
        size_t size) override;
    
    void do_deallocate_sm(
        void *at) override;

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_GLOBAL_HEAP_H