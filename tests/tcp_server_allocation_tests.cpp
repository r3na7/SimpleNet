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

extern "C" int __real_accept4(int, sockaddr *, socklen_t *, int);
namespace
{
int selected_listener = -1, last_fd = -1, resource_error = 0;
}
extern "C" int __wrap_accept4(int fd, sockaddr *address, socklen_t *size, int flags)
{
    if (fd == selected_listener && resource_error) {
        errno = std::exchange(resource_error, 0);
        return -1;
    }
    int result = __real_accept4(fd, address, size, flags);
    if (fd == selected_listener && result >= 0)
        last_fd = result;
    return result;
}
namespace
{
struct Intercept {
    explicit Intercept(int fd)
    {
        selected_listener = fd;
        last_fd = -1;
        resource_error = 0;
    }
    ~Intercept()
    {
        selected_listener = -1;
        resource_error = 0;
    }
};
} // namespace
TEST(TcpServerAllocationTest, ConstructorFailureClosesListener)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    int fd = listener.socket.get_fd();
    snet::TcpServerOptions options;
    options.connection.input_limit = 0;
    EXPECT_THROW(snet::TcpServer(loop, std::move(listener.socket), options), std::invalid_argument);
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
}
TEST(TcpServerAllocationTest, StartBadAllocRetainsConfigurationAndListener)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    int fd = listener.socket.get_fd();
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0;
    server.on_connection([&](auto &) { ++configured; });
    bool failed = false;
    try {
        AllocationGate gate(0);
        server.start();
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
    EXPECT_NO_THROW(server.start());
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
}
TEST(TcpServerAllocationTest, ResumeBadAllocLeavesPaused)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0;
    server.on_connection([](auto &) {});
    server.start();
    server.on_connection({});
    server.on_connection([&](auto &) { ++configured; });
    auto peer = listener.connect();
    bool failed = false;
    try {
        AllocationGate gate(0);
        server.resume_accepting();
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    accept_test::once(loop);
    EXPECT_EQ(configured, 0);
    server.resume_accepting();
    tcp_test::drive(loop, [&] { return configured == 1; });
}
class AcceptedOwnerAllocationTest : public testing::TestWithParam<std::size_t>
{
};
TEST_P(AcceptedOwnerAllocationTest, FailurePreservesExistingClient)
{
    snet::EventLoop loop;
    warm_batch(loop);
    accept_test::Listener listener;
    Intercept interception(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0, closed = 0;
    server.on_connection([&](auto &connection) {
        if (++configured == 1)
            connection.on_data([](auto &current) {
                current.send(tcp_test::bytes("ok"));
                current.consume_input(current.input_data().size());
            });
        else
            connection.on_closed([&](auto &, std::error_code) { ++closed; });
    });
    server.start();
    auto first = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
    auto second = listener.connect();
    bool failed = false;
    try {
        AllocationGate gate(GetParam());
        loop.loop();
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    ASSERT_GE(last_fd, 0);
    EXPECT_EQ(::fcntl(last_fd, F_GETFD), -1);
    EXPECT_EQ(closed, 0);
    RecordProperty("configuration_reached", configured == 2 ? "yes" : "no");
    ASSERT_EQ(::send(first.get_fd(), "q", 1, MSG_NOSIGNAL), 1);
    std::string reply;
    tcp_test::drive(loop, [&] {
        char bytes[8];
        auto n = ::recv(first.get_fd(), bytes, sizeof(bytes), MSG_DONTWAIT);
        if (n > 0)
            reply.append(bytes, static_cast<std::size_t>(n));
        return reply == "ok";
    });
}
INSTANTIATE_TEST_SUITE_P(Boundaries, AcceptedOwnerAllocationTest, testing::Values(0u, 1u, 2u));
TEST(TcpServerAllocationTest, ConfigurationArgumentCopyFailureKeepsHandler)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    int original = 0, next = 0;
    server.on_connection([&](auto &) { ++original; });
    std::array<char, 4096> large{};
    snet::TcpServer::ConnectionCallback candidate = [&, large](auto &) { next += 1 + large[0]; };
    bool failed = false;
    try {
        AllocationGate gate(0);
        server.on_connection(candidate);
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    server.start();
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return original == 1; });
    EXPECT_EQ(next, 0);
}
TEST(TcpServerAllocationTest, ErrorArgumentCopyFailureKeepsHandler)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> weak = token;
    server.on_accept_error([token](auto &, std::error_code) {});
    token.reset();
    std::array<char, 4096> large{};
    snet::TcpServer::AcceptErrorCallback candidate = [large](auto &, std::error_code) { (void)large; };
    bool failed = false;
    try {
        AllocationGate gate(0);
        server.on_accept_error(candidate);
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    EXPECT_FALSE(weak.expired());
    server.on_accept_error({});
    EXPECT_TRUE(weak.expired());
}
TEST(TcpServerAllocationTest, PreparedStopMarkCleanupAndDestructorDoNotAllocate)
{
    snet::EventLoop loop;
    warm_batch(loop);
    accept_test::Listener listener;
    auto server = std::make_unique<snet::TcpServer>(loop, std::move(listener.socket));
    std::vector<std::weak_ptr<int>> weak;
    int closed = 0;
    server->on_connection([&](auto &connection) {
        auto token = std::make_shared<int>(0);
        weak.push_back(token);
        connection.on_data([token](auto &) {});
        connection.on_closed([&](auto &, std::error_code) { ++closed; });
    });
    server->start();
    auto a = listener.connect(), b = listener.connect();
    tcp_test::drive(loop, [&] { return weak.size() == 2; });
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    {
        AllocationGate gate(0);
        server->stop();
        stop.schedule();
        loop.loop();
        server.reset();
    }
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(closed, 2);
    for (auto &item : weak)
        EXPECT_TRUE(item.expired());
}
TEST(TcpServerAllocationTest, ResourceForwardingPausesWithoutAllocation)
{
    snet::EventLoop loop;
    warm_batch(loop);
    accept_test::Listener listener;
    Intercept interception(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    int error = 0;
    server.on_connection([](auto &) {});
    server.on_accept_error([&](auto &, std::error_code reason) {
        error = reason.value();
        loop.quit();
    });
    server.start();
    auto peer = listener.connect();
    resource_error = EMFILE;
    {
        AllocationGate gate(0);
        loop.loop();
    }
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(error, EMFILE);
}
