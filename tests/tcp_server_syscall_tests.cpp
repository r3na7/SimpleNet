#include "acceptor_test_utils.hpp"
#include <array>
#include <gtest/gtest.h>
extern "C" int __real_epoll_wait(int, epoll_event *, int, int);
extern "C" int __real_accept4(int, sockaddr *, socklen_t *, int);
extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);
namespace
{
int listener_fd = -1, adds = 0, client_adds = 0, last_accepted = -1;
bool fail_listener_add = false, fail_client_add = false;
int observed_epoll = -1, accept_error = 0, accept_attempts = 0;
bool listener_registered = false;
void *listener_pointer = nullptr;
std::vector<std::pair<int, void *>> clients;
std::array<epoll_event, 3> scripted_events{};
int scripted_count = 0;
struct Intercept {
    explicit Intercept(int fd)
    {
        listener_fd = fd;
        adds = client_adds = 0;
        last_accepted = -1;
        fail_listener_add = fail_client_add = false;
        observed_epoll = -1;
        accept_error = accept_attempts = 0;
        listener_registered = false;
        listener_pointer = nullptr;
        scripted_count = 0;
        clients.clear();
        clients.reserve(1024);
    }
    ~Intercept() { listener_fd = -1; }
};
} // namespace
extern "C" int __wrap_epoll_ctl(int epoll, int op, int fd, epoll_event *event)
{
    if (fd == listener_fd && op == EPOLL_CTL_ADD && fail_listener_add) {
        fail_listener_add = false;
        errno = ENOMEM;
        return -1;
    }
    if (listener_fd >= 0 && fd != listener_fd && op == EPOLL_CTL_ADD && fail_client_add) {
        fail_client_add = false;
        errno = ENOMEM;
        return -1;
    }
    int result = __real_epoll_ctl(epoll, op, fd, event);
    if (fd == listener_fd && op == EPOLL_CTL_ADD && result == 0) {
        ++adds;
        observed_epoll = epoll;
        listener_pointer = event->data.ptr;
        listener_registered = true;
    }
    if (listener_fd >= 0 && fd != listener_fd && op == EPOLL_CTL_ADD && result == 0) {
        ++client_adds;
        void *pointer = event->data.ptr;
        clients.emplace_back(fd, pointer);
    }
    if (fd == listener_fd && op == EPOLL_CTL_DEL && result == 0)
        listener_registered = false;
    return result;
}
TEST(TcpServerSyscallTest, StartFailureRetainsConfigurationAndListener)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0;
    server.on_connection([&](auto &) { ++configured; });
    fail_listener_add = true;
    EXPECT_THROW(server.start(), std::system_error);
    EXPECT_EQ(adds, 0);
    EXPECT_NE(::fcntl(listener_fd, F_GETFD), -1);
    EXPECT_NO_THROW(server.start());
    EXPECT_EQ(adds, 1);
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
}
TEST(TcpServerSyscallTest, StopAcceptingBeforeStartIsPermanent)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    server.on_connection([](auto &) {});
    server.stop_accepting();
    server.stop_accepting();
    EXPECT_EQ(::fcntl(listener_fd, F_GETFD), -1);
    EXPECT_EQ(adds, 0);
    EXPECT_THROW(server.start(), std::logic_error);
}

extern "C" int __wrap_accept4(int fd, sockaddr *address, socklen_t *size, int flags)
{
    if (fd == listener_fd) {
        ++accept_attempts;
        if (accept_error) {
            errno = std::exchange(accept_error, 0);
            return -1;
        }
    }
    int accepted = __real_accept4(fd, address, size, flags);
    if (fd == listener_fd && accepted >= 0)
        last_accepted = accepted;
    return accepted;
}
TEST(TcpServerSyscallTest, ConfigurationCloseSkipsClientRegistration)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0;
    server.on_connection([&](auto &connection) {
        ++configured;
        connection.close();
    });
    server.start();
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
    EXPECT_EQ(client_adds, 0);
    ASSERT_GE(last_accepted, 0);
    EXPECT_EQ(::fcntl(last_accepted, F_GETFD), -1);
}

extern "C" int __wrap_epoll_wait(int epoll, epoll_event *events, int maximum, int timeout)
{
    if (epoll == observed_epoll && scripted_count > 0) {
        int count = std::exchange(scripted_count, 0);
        if (count > maximum)
            std::terminate();
        for (int i = 0; i < count; ++i)
            events[i] = scripted_events[static_cast<std::size_t>(i)];
        return count;
    }
    return __real_epoll_wait(epoll, events, maximum, timeout);
}
TEST(TcpServerSyscallTest, ClientStartFailureReleasesOnlyNewOwner)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0, closed = 0;
    std::weak_ptr<int> weak;
    server.on_connection([&](auto &connection) {
        ++configured;
        if (configured == 1)
            connection.on_data([](auto &current) {
                current.send(tcp_test::bytes("ok"));
                current.consume_input(current.input_data().size());
            });
        else {
            auto token = std::make_shared<int>(0);
            weak = token;
            connection.on_closed([&, token](auto &, std::error_code) { ++closed; });
        }
    });
    server.start();
    auto first = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
    fail_client_add = true;
    auto second = listener.connect();
    EXPECT_THROW(accept_test::once(loop), std::system_error);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(closed, 0);
    EXPECT_EQ(::fcntl(last_accepted, F_GETFD), -1);
    ASSERT_EQ(::send(first.get_fd(), "q", 1, MSG_NOSIGNAL), 1);
    std::string reply;
    tcp_test::drive(loop, [&] {
        char data[8];
        auto n = ::recv(first.get_fd(), data, sizeof(data), MSG_DONTWAIT);
        if (n > 0)
            reply.append(data, static_cast<std::size_t>(n));
        return reply == "ok";
    });
}
TEST(TcpServerSyscallTest, FdReuseBeforeCleanupKeepsNewOwner)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    std::vector<snet::TcpConnection *> connections;
    std::vector<std::weak_ptr<int>> weak;
    int old_calls = 0;
    server.on_connection([&](auto &connection) {
        connections.push_back(&connection);
        auto token = std::make_shared<int>(0);
        weak.push_back(token);
        int index = static_cast<int>(connections.size());
        if (index == 1)
            connection.on_data([&, token](auto &current) {
                connections[1]->close();
                current.consume_input(current.input_data().size());
            });
        else if (index == 2)
            connection.on_data([&, token](auto &) { ++old_calls; });
        else
            connection.on_data([token](auto &current) {
                current.send(tcp_test::bytes("new"));
                current.consume_input(current.input_data().size());
            });
    });
    server.start();
    auto a = listener.connect(), b = listener.connect();
    tcp_test::drive(loop, [&] { return connections.size() == 2; });
    ASSERT_EQ(clients.size(), 2u);
    int old_fd = clients[1].first;
    auto third = listener.connect();
    ASSERT_EQ(::send(a.get_fd(), "a", 1, MSG_NOSIGNAL), 1);
    ASSERT_EQ(::send(b.get_fd(), "b", 1, MSG_NOSIGNAL), 1);
    scripted_events[0].events = EPOLLIN;
    scripted_events[0].data.ptr = clients[0].second;
    scripted_events[1].events = EPOLLIN;
    scripted_events[1].data.ptr = listener_pointer;
    scripted_events[2].events = EPOLLIN;
    scripted_events[2].data.ptr = clients[1].second;
    scripted_count = 3;
    accept_test::once(loop);
    ASSERT_EQ(connections.size(), 3u);
    EXPECT_EQ(last_accepted, old_fd);
    EXPECT_TRUE(weak[1].expired());
    EXPECT_FALSE(weak[2].expired());
    EXPECT_EQ(old_calls, 0);
    ASSERT_EQ(::send(third.get_fd(), "q", 1, MSG_NOSIGNAL), 1);
    std::string reply;
    tcp_test::drive(loop, [&] {
        char data[8];
        auto n = ::recv(third.get_fd(), data, sizeof(data), MSG_DONTWAIT);
        if (n > 0)
            reply.append(data, static_cast<std::size_t>(n));
        return reply == "new";
    });
}
TEST(TcpServerSyscallTest, SavedBatchRestartSkipsClosedOtherClient)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    std::vector<snet::TcpConnection *> connections;
    std::weak_ptr<int> weak;
    int other_calls = 0;
    server.on_connection([&](auto &connection) {
        connections.push_back(&connection);
        if (connections.size() == 1)
            connection.on_data([&](auto &current) {
                connections[1]->close();
                current.consume_input(current.input_data().size());
                throw std::runtime_error("first");
            });
        else {
            auto token = std::make_shared<int>(0);
            weak = token;
            connection.on_data([&, token](auto &) { ++other_calls; });
        }
    });
    server.start();
    auto a = listener.connect(), b = listener.connect();
    tcp_test::drive(loop, [&] { return connections.size() == 2; });
    scripted_events[0].events = EPOLLIN;
    scripted_events[0].data.ptr = clients[0].second;
    scripted_events[1].events = EPOLLIN;
    scripted_events[1].data.ptr = clients[1].second;
    scripted_count = 2;
    ASSERT_EQ(::send(a.get_fd(), "a", 1, MSG_NOSIGNAL), 1);
    ASSERT_EQ(::send(b.get_fd(), "b", 1, MSG_NOSIGNAL), 1);
    EXPECT_THROW(accept_test::once(loop), std::runtime_error);
    EXPECT_TRUE(weak.expired());
    accept_test::once(loop);
    EXPECT_EQ(other_calls, 0);
}

class ServerResourceTest : public testing::TestWithParam<int>
{
};
TEST_P(ServerResourceTest, ForwardsReasonAfterPauseAndKeepsLiveClient)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0, errors = 0;
    server.on_connection([&](auto &connection) {
        ++configured;
        connection.on_data([](auto &current) {
            current.send(tcp_test::bytes("ok"));
            current.consume_input(current.input_data().size());
        });
    });
    server.on_accept_error([&](auto &, std::error_code reason) {
        EXPECT_EQ(reason.value(), GetParam());
        EXPECT_FALSE(listener_registered);
        ++errors;
    });
    server.start();
    auto first = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
    auto second = listener.connect();
    accept_error = GetParam();
    EXPECT_NO_THROW(accept_test::once(loop));
    EXPECT_EQ(errors, 1);
    EXPECT_EQ(configured, 1);
    int attempts = accept_attempts;
    accept_test::once(loop);
    EXPECT_EQ(accept_attempts, attempts);
    ASSERT_EQ(::send(first.get_fd(), "q", 1, MSG_NOSIGNAL), 1);
    std::string reply;
    tcp_test::drive(loop, [&] {
        char data[8];
        auto n = ::recv(first.get_fd(), data, sizeof(data), MSG_DONTWAIT);
        if (n > 0)
            reply.append(data, static_cast<std::size_t>(n));
        return reply == "ok";
    });
    server.resume_accepting();
    tcp_test::drive(loop, [&] { return configured == 2; });
}
INSTANTIATE_TEST_SUITE_P(Limits, ServerResourceTest, testing::Values(EMFILE, ENFILE, ENOMEM, ENOBUFS));
TEST(TcpServerSyscallTest, ResourceCallbackResumesOnlyNextDispatch)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0, errors = 0;
    server.on_connection([&](auto &) { ++configured; });
    server.on_accept_error([&](auto &current, std::error_code) {
        ++errors;
        EXPECT_FALSE(listener_registered);
        current.resume_accepting();
    });
    server.start();
    auto peer = listener.connect();
    accept_error = EMFILE;
    accept_test::once(loop);
    EXPECT_EQ(accept_attempts, 1);
    EXPECT_EQ(configured, 0);
    EXPECT_EQ(errors, 1);
    tcp_test::drive(loop, [&] { return configured == 1; });
}
TEST(TcpServerSyscallTest, EmptyResourceHandlerKeepsPause)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    server.on_connection([](auto &) {});
    server.start();
    auto peer = listener.connect();
    accept_error = EMFILE;
    accept_test::once(loop);
    EXPECT_FALSE(listener_registered);
    int attempts = accept_attempts;
    accept_test::once(loop);
    EXPECT_EQ(accept_attempts, attempts);
}
TEST(TcpServerSyscallTest, ResourceReplacementAndClearSurviveThrow)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    int first = 0, next = 0;
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> weak = token;
    server.on_connection([](auto &) {});
    server.on_accept_error([&, token](auto &current, std::error_code) {
        ++first;
        current.on_accept_error([&](auto &self, std::error_code) {
            ++next;
            self.on_accept_error({});
        });
        EXPECT_FALSE(weak.expired());
        throw std::runtime_error("resource");
    });
    token.reset();
    server.start();
    auto peer = listener.connect();
    accept_error = EMFILE;
    EXPECT_THROW(accept_test::once(loop), std::runtime_error);
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(listener_registered);
    accept_test::once(loop);
    EXPECT_EQ(first, 1);
    server.resume_accepting();
    accept_error = ENFILE;
    accept_test::once(loop);
    EXPECT_EQ(next, 1);
    server.resume_accepting();
    accept_error = ENOMEM;
    accept_test::once(loop);
    EXPECT_EQ(next, 1);
}
TEST(TcpServerSyscallTest, ResourceCallbackClearConfigurationRequiresNewHandler)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0;
    bool notified = false;
    server.on_connection([&](auto &) { ++configured; });
    server.on_accept_error([&](auto &current, std::error_code) {
        notified = true;
        current.on_connection({});
        EXPECT_THROW(current.resume_accepting(), std::logic_error);
    });
    server.start();
    auto peer = listener.connect();
    accept_error = EMFILE;
    EXPECT_NO_THROW(accept_test::once(loop));
    EXPECT_TRUE(notified);
    EXPECT_EQ(configured, 0);
    server.on_connection([&](auto &) { ++configured; });
    accept_test::once(loop);
    EXPECT_EQ(configured, 0);
    server.resume_accepting();
    tcp_test::drive(loop, [&] { return configured == 1; });
    server.stop_accepting();
    EXPECT_THROW(server.resume_accepting(), std::logic_error);
}
