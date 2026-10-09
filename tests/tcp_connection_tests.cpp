#include "tcp_test_utils.hpp"
#include <fcntl.h>
#include <gtest/gtest.h>
#include <memory>
#include <type_traits>

using tcp_test::bytes;
static_assert(!std::is_copy_constructible_v<snet::TcpConnection>);
static_assert(!std::is_move_constructible_v<snet::TcpConnection>);
static_assert(!std::is_copy_assignable_v<snet::TcpConnection>);
static_assert(!std::is_move_assignable_v<snet::TcpConnection>);
static_assert(std::is_nothrow_destructible_v<snet::TcpConnection>);

TEST(TcpConnectionTest, InvalidOptionsCloseSocket)
{
    snet::EventLoop loop;
    using Field = std::size_t snet::ConnectionOptions::*;
    const Field fields[]{&snet::ConnectionOptions::input_limit,       &snet::ConnectionOptions::output_limit,
                         &snet::ConnectionOptions::read_byte_budget,  &snet::ConnectionOptions::read_call_budget,
                         &snet::ConnectionOptions::write_byte_budget, &snet::ConnectionOptions::write_call_budget};
    for (auto field : fields) {
        tcp_test::Pair pair;
        int fd = pair.accepted.get_fd();
        snet::ConnectionOptions options;
        options.*field = 0;
        EXPECT_THROW(snet::TcpConnection(loop, std::move(pair.accepted), options), std::invalid_argument);
        errno = 0;
        EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
        EXPECT_EQ(errno, EBADF);
    }
    for (auto threshold : {std::size_t{65536}, std::size_t{65537}}) {
        tcp_test::Pair pair;
        snet::ConnectionOptions options;
        options.output_low_watermark = threshold;
        EXPECT_THROW(snet::TcpConnection(loop, std::move(pair.accepted), options), std::invalid_argument);
    }
}

TEST(TcpConnectionTest, EmptySocketRejected)
{
    snet::EventLoop loop;
    EXPECT_THROW(snet::TcpConnection(loop, snet::Socket{}), std::invalid_argument);
}

TEST(TcpConnectionTest, ConstructedButNotStarted)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    int fd = pair.accepted.get_fd();
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    int calls = 0;
    connection.on_data([&](auto &) { ++calls; });
    pair.send("abc");
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(calls, 0);
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
}

TEST(TcpConnectionTest, StartAndDestructorRemoveRegistration)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    int fd = pair.accepted.get_fd();
    {
        snet::TcpConnection connection(loop, std::move(pair.accepted));
        connection.start();
    }
    errno = 0;
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    int replacement = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    tcp_test::check(replacement, "socket");
    snet::Socket owner(replacement);
    tcp_test::check(::dup2(replacement, fd), "dup2");
    snet::Socket reused;
    if (replacement != fd)
        reused = snet::Socket(fd);
    snet::Channel channel(fd);
    channel.set_events(EPOLLIN);
    EXPECT_NO_THROW(loop.update_channel(&channel));
    EXPECT_NO_THROW(loop.remove_channel(&channel));
}

TEST(TcpConnectionTest, RepeatStartRejected)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    EXPECT_THROW(connection.start(), std::logic_error);
}

TEST(TcpConnectionTest, StartAfterCloseRejected)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.close();
    EXPECT_THROW(connection.start(), std::logic_error);
    EXPECT_EQ(connection.send({}).status, snet::SendStatus::closed);
}

TEST(TcpConnectionTest, SendAndFinishBeforeStartRejected)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    EXPECT_THROW(connection.send(bytes("abc")), std::logic_error);
    EXPECT_THROW(connection.send({}), std::logic_error);
    EXPECT_THROW(connection.finish_sending(), std::logic_error);
}

TEST(TcpConnectionTest, CloseBeforeStartDefersNotification)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    int fd = pair.accepted.get_fd();
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    int calls = 0;
    connection.on_closed([&](auto &, std::error_code error) {
        EXPECT_FALSE(error);
        ++calls;
    });
    connection.close();
    connection.close();
    EXPECT_EQ(calls, 0);
    errno = 0;
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    tcp_test::drive(loop, [&] { return calls == 1; });
    EXPECT_EQ(calls, 1);
}

TEST(TcpConnectionTest, DestructorCancelsPendingClose)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    int calls = 0;
    {
        snet::TcpConnection connection(loop, std::move(pair.accepted));
        connection.on_closed([&](auto &, std::error_code) { ++calls; });
        connection.close();
    }
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(calls, 0);
}

TEST(TcpConnectionTest, ClosedCallbackRetainedDuringSelfClear)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    auto token = std::make_shared<int>(7);
    std::weak_ptr<int> observed(token);
    bool called = false;
    connection.on_closed([&, token](auto &current, std::error_code) {
        current.on_closed({});
        EXPECT_FALSE(observed.expired());
        called = true;
    });
    token.reset();
    connection.close();
    tcp_test::drive(loop, [&] { return called; });
    EXPECT_TRUE(observed.expired());
}

TEST(TcpConnectionTest, OutputLimitAcceptsPrefix)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::ConnectionOptions options;
    options.output_limit = 8;
    options.output_low_watermark = 3;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    auto result = connection.send(bytes("0123456789"));
    EXPECT_EQ(result.status, snet::SendStatus::would_block);
    EXPECT_EQ(result.accepted_bytes, 8u);
    EXPECT_FALSE(result.error);
    auto next = connection.send(bytes("ab"));
    EXPECT_EQ(next.status, snet::SendStatus::would_block);
    EXPECT_EQ(next.accepted_bytes, 0u);
    EXPECT_TRUE(pair.read().empty());
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received.size() == 8;
    });
    EXPECT_EQ(received, "01234567");
}

TEST(TcpConnectionTest, AcceptedCopiesSource)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    {
        std::string source = "original";
        auto result = connection.send(bytes(source));
        EXPECT_EQ(result.status, snet::SendStatus::accepted);
        EXPECT_EQ(result.accepted_bytes, 8u);
        source.assign("changed!");
    }
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received.size() == 8;
    });
    EXPECT_EQ(received, "original");
}

TEST(TcpConnectionTest, EmptySendHasNoWorkOrNotification)
{
    snet::EventLoop loop;
    loop.set_work_budget(1);
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    int calls = 0;
    connection.on_output_available([&](auto &) { ++calls; });
    auto result = connection.send({});
    EXPECT_EQ(result.status, snet::SendStatus::accepted);
    EXPECT_EQ(result.accepted_bytes, 0u);
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(loop.iteration_id(), 1u);
    EXPECT_EQ(calls, 0);
}

TEST(TcpConnectionTest, OutputThresholdDownCross)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::ConnectionOptions options;
    options.output_limit = 8;
    options.output_low_watermark = 3;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    int notices = 0;
    connection.on_output_available([&](auto &) { ++notices; });
    EXPECT_EQ(connection.send(bytes("ABCDEFGH")).accepted_bytes, 8u);
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received.size() == 8 && notices == 1;
    });
    EXPECT_EQ(connection.send(bytes("ij")).accepted_bytes, 2u);
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received.size() == 10;
    });
    EXPECT_EQ(notices, 1);
    EXPECT_EQ(connection.send(bytes("KLMNOPQR")).accepted_bytes, 8u);
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received.size() == 18 && notices == 2;
    });
    EXPECT_EQ(received, "ABCDEFGHijKLMNOPQR");
}

TEST(TcpConnectionTest, ZeroThresholdNotifiesOnDrain)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::ConnectionOptions options;
    options.output_low_watermark = 0;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    int notices = 0;
    connection.on_output_available([&](auto &) { ++notices; });
    EXPECT_EQ(connection.send(bytes("x")).accepted_bytes, 1u);
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received == "x" && notices == 1;
    });
}

TEST(TcpConnectionTest, LargeStreamProgresses)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::ConnectionOptions options;
    options.output_limit = 8;
    options.output_low_watermark = 3;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    const std::string source(129, 'X');
    std::size_t offset = 0;
    auto produce = [&] {
        offset += connection.send(bytes(source).subspan(offset)).accepted_bytes;
        if (offset == source.size())
            connection.finish_sending();
    };
    connection.on_output_available([&](auto &) { produce(); });
    produce();
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return pair.eof;
    });
    EXPECT_EQ(offset, source.size());
    EXPECT_EQ(received, source);
}

TEST(TcpConnectionTest, FinishDrainsBeforeShutdown)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    EXPECT_EQ(connection.send(bytes("response")).accepted_bytes, 8u);
    connection.finish_sending();
    connection.finish_sending();
    EXPECT_EQ(connection.send(bytes("late")).status, snet::SendStatus::sending_finished);
    EXPECT_EQ(connection.send({}).status, snet::SendStatus::sending_finished);
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return pair.eof;
    });
    EXPECT_EQ(received, "response");
    pair.send("our receive side is still open");
}

TEST(TcpConnectionTest, InputLimitStopsReadAndConsumptionResumes)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::ConnectionOptions options;
    options.input_limit = 4;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    int calls = 0;
    connection.on_data([&](auto &) { ++calls; });
    pair.send("abcdef");
    tcp_test::drive(loop, [&] { return calls == 1; });
    EXPECT_EQ(tcp_test::text(connection.input_data()), "abcd");
    EXPECT_THROW(connection.consume_input(5), std::out_of_range);
    EXPECT_EQ(tcp_test::text(connection.input_data()), "abcd");
    connection.consume_input(2);
    EXPECT_EQ(calls, 1);
    tcp_test::drive(loop, [&] { return calls == 2; });
    EXPECT_EQ(tcp_test::text(connection.input_data()), "cdef");
}

TEST(TcpConnectionTest, ExplicitPauseSurvivesConsume)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::ConnectionOptions options;
    options.input_limit = 4;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    int calls = 0;
    connection.on_data([&](auto &current) {
        ++calls;
        if (calls == 1) {
            current.pause_reading();
            current.consume_input(2);
        }
    });
    pair.send("abcdef");
    tcp_test::drive(loop, [&] { return calls == 1; });
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(tcp_test::text(connection.input_data()), "cd");
    connection.resume_reading();
    EXPECT_EQ(calls, 1);
    tcp_test::drive(loop, [&] { return calls == 2; });
    EXPECT_EQ(tcp_test::text(connection.input_data()), "cdef");
}

TEST(TcpConnectionTest, ResumeCannotOverrideFullInput)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::ConnectionOptions options;
    options.input_limit = 4;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    int calls = 0;
    connection.on_data([&](auto &) { ++calls; });
    pair.send("abcdef");
    tcp_test::drive(loop, [&] { return calls == 1; });
    connection.pause_reading();
    connection.resume_reading();
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(tcp_test::text(connection.input_data()), "abcd");
}

TEST(TcpConnectionTest, PartialProtocolBytesRetained)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    int calls = 0;
    std::string full;
    connection.on_data([&](auto &current) {
        ++calls;
        std::string data = tcp_test::text(current.input_data());
        auto end = data.find('\n');
        if (end != std::string::npos) {
            full = data.substr(0, end + 1);
            current.consume_input(end + 1);
        }
    });
    pair.send("HEL");
    tcp_test::drive(loop, [&] { return calls == 1; });
    EXPECT_EQ(tcp_test::text(connection.input_data()), "HEL");
    pair.send("LO\nNE");
    tcp_test::drive(loop, [&] { return calls == 2; });
    EXPECT_EQ(full, "HELLO\n");
    EXPECT_EQ(tcp_test::text(connection.input_data()), "NE");
}

TEST(TcpConnectionTest, ReceiveByteBudgetCapsGroup)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::ConnectionOptions options;
    options.read_byte_budget = 2;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    std::vector<std::string> groups;
    connection.on_data([&](auto &current) {
        groups.push_back(tcp_test::text(current.input_data()));
        current.consume_input(current.input_data().size());
    });
    pair.send("abcdef");
    tcp_test::drive(loop, [&] { return groups.size() == 3; });
    EXPECT_EQ(groups, (std::vector<std::string>{"ab", "cd", "ef"}));
}

TEST(TcpConnectionTest, Ipv6InputAndOutput)
{
    snet::EventLoop loop;
    tcp_test::Pair pair(AF_INET6);
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    connection.on_data([&](auto &current) {
        auto result = current.send(current.input_data());
        current.consume_input(result.accepted_bytes);
    });
    pair.send("ipv6");
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received == "ipv6";
    });
}

TEST(TcpConnectionTest, DataThenEof)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    std::vector<std::string> events;
    connection.on_data([&](auto &current) {
        EXPECT_EQ(tcp_test::text(current.input_data()), "last");
        events.push_back("data");
    });
    connection.on_eof([&](auto &) { events.push_back("eof"); });
    pair.send("last");
    tcp_test::check(::shutdown(pair.peer.get_fd(), SHUT_WR), "shutdown");
    tcp_test::drive(loop, [&] { return events.size() == 2; });
    EXPECT_EQ(events, (std::vector<std::string>{"data", "eof"}));
}

TEST(TcpConnectionTest, EofDoesNotFinishOurOutput)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    bool eof = false;
    connection.on_eof([&](auto &) { eof = true; });
    tcp_test::check(::shutdown(pair.peer.get_fd(), SHUT_WR), "shutdown");
    tcp_test::drive(loop, [&] { return eof; });
    EXPECT_EQ(connection.send(bytes("response")).status, snet::SendStatus::accepted);
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received == "response";
    });
    EXPECT_FALSE(pair.eof);
    connection.finish_sending();
    tcp_test::drive(loop, [&] {
        (void)pair.read();
        return pair.eof;
    });
}

TEST(TcpConnectionTest, BothDirectionsFinishedAutoCloseKeepsInput)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    std::vector<std::string> events;
    connection.on_data([&](auto &current) {
        events.push_back("data");
        current.finish_sending();
    });
    connection.on_eof([&](auto &) { events.push_back("eof"); });
    connection.on_closed([&](auto &current, std::error_code error) {
        EXPECT_FALSE(error);
        EXPECT_EQ(tcp_test::text(current.input_data()), "last");
        events.push_back("closed");
    });
    pair.send("last");
    tcp_test::check(::shutdown(pair.peer.get_fd(), SHUT_WR), "shutdown");
    tcp_test::drive(loop, [&] { return events.size() == 3; });
    EXPECT_EQ(events, (std::vector<std::string>{"data", "eof", "closed"}));
    EXPECT_EQ(connection.send({}).status, snet::SendStatus::closed);
}

TEST(TcpConnectionTest, OnDataCloseSuppressesEof)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    int eof = 0, closed = 0;
    connection.on_data([&](auto &current) { current.close(); });
    connection.on_eof([&](auto &) { ++eof; });
    connection.on_closed([&](auto &, std::error_code) { ++closed; });
    pair.send("last");
    tcp_test::check(::shutdown(pair.peer.get_fd(), SHUT_WR), "shutdown");
    tcp_test::drive(loop, [&] { return closed == 1; });
    EXPECT_EQ(eof, 0);
}
