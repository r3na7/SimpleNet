#include "tcp_test_utils.hpp"
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

extern "C" ssize_t __real_recv(int, void *, std::size_t, int);
namespace
{
int observed_fd = -1;
int receives = 0;
} // namespace

extern "C" ssize_t __wrap_recv(int fd, void *data, std::size_t size, int flags)
{
    if (fd == observed_fd)
        ++receives;

    return __real_recv(fd, data, size, flags);
}

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

TEST(TcpAllocationTest, SendBadAllocAcceptsNothingAndPreservesEarlierPrefix)
{
    snet::EventLoop loop;

    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));

    connection.start();
    connection.send(tcp_test::bytes("old"));
    const std::string extra(4096, 'x');
    bool failed = false;

    try {
        AllocationGate gate(0);

        (void)connection.send(tcp_test::bytes(extra));
    } catch (const std::bad_alloc &) {
        failed = true;
    }

    EXPECT_TRUE(failed);
    std::string received;

    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received.size() >= 3;
    });

    EXPECT_EQ(received, "old");
    EXPECT_EQ(connection.send(tcp_test::bytes(extra)).accepted_bytes, extra.size());
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received.size() == 3 + extra.size();
    });

    EXPECT_EQ(received, "old" + extra);
}

TEST(TcpAllocationTest, RecvBadAllocLeavesKernelBytes)
{
    snet::EventLoop loop;

    warm_batch(loop);
    tcp_test::Pair pair;
    observed_fd = pair.accepted.get_fd();
    receives = 0;
    snet::TcpConnection connection(loop, std::move(pair.accepted));

    connection.start();
    pair.send("kernel");
    bool failed = false;

    try {
        AllocationGate gate(0);

        loop.loop();
    } catch (const std::bad_alloc &) {
        failed = true;
    }

    int attempts = receives;

    observed_fd = -1;

    EXPECT_TRUE(failed);
    EXPECT_EQ(attempts, 0);
    EXPECT_TRUE(connection.input_data().empty());
    tcp_test::drive(loop, [&] { return connection.input_data().size() == 6; });

    EXPECT_EQ(tcp_test::text(connection.input_data()), "kernel");
}

TEST(TcpAllocationTest, PreparedCloseCancelAndDestructorDoNotAllocate)
{
    snet::EventLoop loop;

    tcp_test::Pair pair;
    auto connection = std::make_unique<snet::TcpConnection>(loop, std::move(pair.accepted));
    int closed = 0;

    connection->on_closed([&](auto &, std::error_code) { ++closed; });
    connection->start();
    connection->send(tcp_test::bytes("discard"));
    {
        AllocationGate gate(0);

        connection->close();
        connection->close();
        connection.reset();
    }

    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(closed, 0);
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });

    stop.schedule();
    loop.loop();

    EXPECT_EQ(closed, 0);
}

TEST(TcpAllocationTest, StartBadAllocLeavesCreatedAndCallbacksIntact)
{
    snet::EventLoop loop;

    tcp_test::Pair pair;
    int fd = pair.accepted.get_fd();
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    int data = 0;

    connection.on_data([&](auto &) { ++data; });
    bool failed = false;

    try {
        AllocationGate gate(0);

        connection.start();
    } catch (const std::bad_alloc &) {
        failed = true;
    }

    EXPECT_TRUE(failed);
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
    EXPECT_THROW(connection.send({}), std::logic_error);
    EXPECT_NO_THROW(connection.start());
    pair.send("ok");
    tcp_test::drive(loop, [&] { return data == 1; });
}

TEST(TcpAllocationTest, ConstructorFailureClosesSocket)
{
    snet::EventLoop loop;

    tcp_test::Pair pair;
    int fd = pair.accepted.get_fd();
    snet::ConnectionOptions options;

    options.input_limit = 0;

    EXPECT_THROW(snet::TcpConnection(loop, std::move(pair.accepted), options), std::invalid_argument);
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(TcpAllocationTest, CallbackSetterAllocationFailurePreservesHandler)
{
    snet::EventLoop loop;

    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    int original = 0, replacement = 0;

    connection.on_data([&](auto &) { ++original; });
    std::array<char, 4096> large{};
    snet::TcpConnection::Callback candidate = [&, large](auto &) { replacement += 1 + large[0]; };
    bool failed = false;

    try {
        AllocationGate gate(0);

        connection.on_data(candidate);
    } catch (const std::bad_alloc &) {
        failed = true;
    }

    EXPECT_TRUE(failed);
    connection.start();
    pair.send("ok");
    tcp_test::drive(loop, [&] { return original == 1; });

    EXPECT_EQ(replacement, 0);
}

TEST(TcpAllocationTest, FinalResetReadPreparationFailureStillCloses)
{
    snet::EventLoop loop;

    warm_batch(loop);
    tcp_test::Pair pair;
    int fd = pair.accepted.get_fd();
    snet::TcpConnection connection(loop, std::move(pair.accepted));

    connection.start();
    pair.send("final");
    linger reset{1, 0};
    tcp_test::check(::setsockopt(pair.peer.get_fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)), "linger");
    pair.peer.close();
    char peek[8];

    ASSERT_EQ(::recv(fd, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT), 5);
    bool failed = false;
    int closed = 0;

    connection.on_closed([&](auto &, std::error_code) { ++closed; });

    try {
        AllocationGate gate(0);

        loop.loop();
    } catch (const std::bad_alloc &) {
        failed = true;
    }

    EXPECT_TRUE(failed);
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_TRUE(connection.input_data().empty());
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });

    stop.schedule();
    loop.loop();

    EXPECT_EQ(closed, 0);
}
