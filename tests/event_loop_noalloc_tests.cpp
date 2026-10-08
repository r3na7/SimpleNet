#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include <new>
#include <simplenet/EventLoop.hpp>

namespace
{
bool measuring = false;
std::size_t allocations = 0;
void *allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t))
{
    if (measuring)
        ++allocations;
    void *ptr = nullptr;
    if (alignment <= alignof(std::max_align_t))
        ptr = std::malloc(size ? size : 1);
    else if (posix_memalign(&ptr, alignment, size ? size : 1) != 0)
        ptr = nullptr;
    if (!ptr)
        throw std::bad_alloc();
    return ptr;
}
} // namespace
void *operator new(std::size_t n) { return allocate(n); }
void *operator new[](std::size_t n) { return allocate(n); }
void *operator new(std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void *operator new[](std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
    try {
        return allocate(n);
    } catch (...) {
        return nullptr;
    }
}
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept
{
    try {
        return allocate(n);
    } catch (...) {
        return nullptr;
    }
}
void *operator new(std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept
{
    try {
        return allocate(n, static_cast<std::size_t>(a));
    } catch (...) {
        return nullptr;
    }
}
void *operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept
{
    try {
        return allocate(n, static_cast<std::size_t>(a));
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { std::free(p); }

TEST(LoopAllocationTest, PreparedOperationsDoNotAllocate)
{
    bool destroyed = false;
    struct Owned {
        bool &d;
        ~Owned() noexcept { d = true; }
    };
    auto loop = std::make_unique<snet::EventLoop>();
    auto work = std::make_unique<snet::detail::LoopWork>(*loop, [] {});
    auto owner = std::make_unique<Owned>(destroyed);
    auto slot = loop->prepare_retirement<Owned>([](const Owned &) noexcept { return true; }, [](Owned &) noexcept {});
    allocations = 0;
    measuring = true;
    work->schedule();
    work->schedule();
    work->cancel();
    work.reset();
    loop->retire(std::move(slot), std::move(owner));
    loop.reset();
    measuring = false;
    EXPECT_EQ(allocations, 0u);
    EXPECT_TRUE(destroyed);
}
