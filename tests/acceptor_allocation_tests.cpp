#include "acceptor_test_utils.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <sys/eventfd.h>

namespace
{
bool measuring = false;
std::size_t allocations = 0;
std::size_t permitted = std::numeric_limits<std::size_t>::max();
void *allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t))
{
    if (measuring && ++allocations > permitted)
        throw std::bad_alloc();
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

namespace
{
class AllocationGate
{
public:
    explicit AllocationGate(std::size_t allow = std::numeric_limits<std::size_t>::max())
    {
        allocations = 0;
        permitted = allow;
        measuring = true;
    }
    ~AllocationGate() { measuring = false; }
    AllocationGate(const AllocationGate &) = delete;
    AllocationGate &operator=(const AllocationGate &) = delete;
};
} // namespace

namespace
{
void warm_batch(snet::EventLoop &loop)
{
    snet::Socket event(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::Channel warm(event.get_fd());
    warm.set_events(EPOLLIN);
    warm.set_read_callback([&] {
        std::uint64_t value;
        tcp_test::check(static_cast<int>(::read(event.get_fd(), &value, sizeof(value))), "eventfd read");
        loop.quit();
    });
    loop.update_channel(&warm);
    std::uint64_t one = 1;
    tcp_test::check(static_cast<int>(::write(event.get_fd(), &one, sizeof(one))), "eventfd write");
    loop.loop();
    loop.remove_channel(&warm); // Batch storage is prepared before the allocation gate.
}
} // namespace

TEST(AcceptorAllocationTest, StartBadAllocRetainsCreatedAndHandler)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    int fd = listener.socket.get_fd();
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int received = 0;
    acceptor.on_accept([&](snet::Socket) { ++received; });
    bool failed = false;
    try {
        AllocationGate gate(0);
        acceptor.start();
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
    EXPECT_NO_THROW(acceptor.start());
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return received == 1; });
}
TEST(AcceptorAllocationTest, ResumeBadAllocRemainsPaused)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int received = 0;
    acceptor.on_accept([&](snet::Socket) { ++received; });
    acceptor.start();
    acceptor.pause_accepting();
    auto peer = listener.connect();
    bool failed = false;
    try {
        AllocationGate gate(0);
        acceptor.resume_accepting();
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    accept_test::once(loop);
    EXPECT_EQ(received, 0);
    EXPECT_NO_THROW(acceptor.resume_accepting());
    tcp_test::drive(loop, [&] { return received == 1; });
}
TEST(AcceptorAllocationTest, CallbackArgumentCopyFailureKeepsOldHandler)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int original = 0, next = 0;
    acceptor.on_accept([&](snet::Socket) { ++original; });
    std::array<char, 4096> large{};
    snet::Acceptor::AcceptCallback candidate = [&, large](snet::Socket) { next += 1 + large[0]; };
    bool failed = false;
    try {
        AllocationGate gate(0);
        acceptor.on_accept(candidate);
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    acceptor.start();
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return original == 1; });
    EXPECT_EQ(next, 0);
}
TEST(AcceptorAllocationTest, ErrorHandlerCopyFailureKeepsOldHandler)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    // Lifetime observation does not need an injected resource syscall in this target.
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> weak = token;
    acceptor.on_error([token](auto &, std::error_code) {});
    token.reset();
    std::array<char, 4096> large{};
    snet::Acceptor::ErrorCallback candidate = [large](auto &, std::error_code) { (void)large; };
    bool failed = false;
    try {
        AllocationGate gate(0);
        acceptor.on_error(candidate);
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    EXPECT_FALSE(weak.expired());
    acceptor.on_error({});
    EXPECT_TRUE(weak.expired());
}
TEST(AcceptorAllocationTest, PreparedPauseCloseDestructorDoNotAllocate)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    auto acceptor = std::make_unique<snet::Acceptor>(loop, std::move(listener.socket));
    int callbacks = 0;
    acceptor->on_accept([&](snet::Socket) { ++callbacks; });
    acceptor->on_error([&](auto &, std::error_code) { ++callbacks; });
    acceptor->start();
    {
        AllocationGate gate(0);
        acceptor->pause_accepting();
        acceptor->close();
        acceptor.reset();
    }
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(callbacks, 0);
}
TEST(AcceptorAllocationTest, ReceiverConstructionFailureClosesAcceptedFd)
{
    snet::EventLoop loop;
    warm_batch(loop);
    accept_test::Listener listener;
    int listener_fd = listener.socket.get_fd();
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int accepted = -1;
    acceptor.on_accept([&](snet::Socket socket) {
        accepted = socket.get_fd();
        AllocationGate gate(0);
        auto connection = std::make_unique<snet::TcpConnection>(loop, std::move(socket));
    });
    acceptor.start();
    auto peer = listener.connect();
    EXPECT_THROW(accept_test::once(loop), std::bad_alloc);
    ASSERT_GE(accepted, 0);
    EXPECT_EQ(::fcntl(accepted, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_NE(::fcntl(listener_fd, F_GETFD), -1);
}
