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

TEST(LoopAllocationTest, PreparedOwnerCleanupDoesNotAllocate)
{
    bool destroyed = false;
    struct Owned {
        bool &d;
        ~Owned() noexcept { d = true; }
    };
    struct Owner {
        snet::EventLoop &loop;
        std::unique_ptr<Owned> object;
        bool cleaned = false;
    };
    auto loop = std::make_unique<snet::EventLoop>();
    auto work = std::make_unique<snet::detail::LoopWork>(*loop, [] {});
    Owner owner{*loop, std::make_unique<Owned>(destroyed)};
    auto cleanup = std::make_unique<snet::detail::LoopCleanup>(
        *loop, &owner, [](void *context, snet::detail::CleanupReason reason) noexcept {
            auto &owner = *static_cast<Owner *>(context);
            owner.cleaned = reason == snet::detail::CleanupReason::normal;
            owner.object.reset();
            owner.loop.quit();
        });
    allocations = 0;
    measuring = true;
    work->schedule();
    work->schedule();
    work->cancel();
    work.reset();
    loop->request_cleanup();
    loop->request_cleanup();
    loop->loop();
    cleanup.reset();
    loop.reset();
    measuring = false;
    EXPECT_EQ(allocations, 0u);
    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(owner.cleaned);
}
