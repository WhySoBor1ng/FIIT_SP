#include <not_implemented.h>
#include "../include/allocator_global_heap.h"
#include <mutex>

[[nodiscard]] void *allocator_global_heap::do_allocate_sm(
    size_t size)
{
    std::lock_guard lock(mutex);

    if (size == 0)
    {
        size = 1;
    }

    return ::operator new(size);
}

void allocator_global_heap::do_deallocate_sm(
    void *at)
{
    std::lock_guard lock(mutex);

    ::operator delete(at);
}

bool allocator_global_heap::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    if (this == &other)
    {
        return true;
    }
    auto other_ptr = dynamic_cast<const allocator_global_heap *>(&other);
    if (other_ptr == nullptr)
    {
        return false;
    }
    return other_ptr == this;
}