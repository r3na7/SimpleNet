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

TEST(TcpConnectionTest, SelfReplaceDataKeepsOldCallableAlive)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    auto token = std::make_shared<int>(1);
    std::weak_ptr<int> lifetime = token;
    int first = 0, second = 0;
    connection.on_data([&, token](auto &current) {
        ++first;
        current.consume_input(current.input_data().size());
        current.on_data([&](auto &) { ++second; });
        EXPECT_FALSE(lifetime.expired());
    });
    token.reset();
    connection.start();
    pair.send("one");
    tcp_test::drive(loop, [&] { return first == 1; });
    EXPECT_TRUE(lifetime.expired());
    pair.send("two");
    tcp_test::drive(loop, [&] { return second == 1; });
    EXPECT_EQ(first, 1);
}

TEST(TcpConnectionTest, SelfClearDataPreventsLaterCalls)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    int calls = 0;
    connection.on_data([&](auto &current) {
        ++calls;
        current.on_data({});
    });
    connection.start();
    pair.send("one");
    tcp_test::drive(loop, [&] { return calls == 1; });
    pair.send("two");
    tcp_test::drive(loop, [&] { return connection.input_data().size() == 6; });
    EXPECT_EQ(calls, 1);
}

TEST(TcpConnectionTest, ReplaceLaterEofAndNoReplay)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    int old = 0, next = 0;
    connection.on_eof([&](auto &) { ++old; });
    connection.on_data([&](auto &current) {
        current.on_eof([&](auto &self) {
            ++next;
            self.on_eof([&](auto &) { ++old; });
        });
    });
    connection.start();
    pair.send("end");
    tcp_test::check(::shutdown(pair.peer.get_fd(), SHUT_WR), "shutdown");
    tcp_test::drive(loop, [&] { return next == 1; });
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(old, 0);
}

TEST(TcpConnectionTest, InstallClosedAfterCloseAndNoReplay)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.close();
    int calls = 0;
    connection.on_closed([&](auto &self, std::error_code) {
        ++calls;
        self.on_closed([&](auto &, std::error_code) { calls += 10; });
    });
    tcp_test::drive(loop, [&] { return calls == 1; });
    connection.close();
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(calls, 1);
}

TEST(TcpConnectionTest, ReplacementSurvivesThrow)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    int first = 0, replacement = 0;
    connection.on_data([&](auto &current) {
        ++first;
        current.on_data([&](auto &) { ++replacement; });
        throw std::runtime_error("application");
    });
    connection.start();
    pair.send("one");
    EXPECT_THROW(loop.loop(), std::runtime_error);
    pair.send("two");
    tcp_test::drive(loop, [&] { return replacement == 1; });
    EXPECT_EQ(first, 1);
    EXPECT_EQ(tcp_test::text(connection.input_data()), "onetwo");
}

TEST(TcpConnectionTest, CloseInsideDataNeverNestsClosed)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    bool inside = false;
    int closed = 0;
    connection.on_data([&](auto &current) {
        inside = true;
        current.close();
        EXPECT_EQ(closed, 0);
        inside = false;
    });
    connection.on_closed([&](auto &, std::error_code) {
        EXPECT_FALSE(inside);
        ++closed;
    });
    connection.start();
    pair.send("one");
    tcp_test::drive(loop, [&] { return closed == 1; });
}

TEST(TcpConnectionTest, CloseThenThrowSkipsClosedAndDestructorCallsNothing)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    int closed = 0;
    {
        snet::TcpConnection connection(loop, std::move(pair.accepted));
        connection.on_closed([&](auto &, std::error_code) { ++closed; });
        connection.on_data([](auto &current) {
            current.close();
            throw std::runtime_error("application");
        });
        connection.start();
        pair.send("one");
        EXPECT_THROW(loop.loop(), std::runtime_error);
        snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
        stop.schedule();
        loop.loop();
        EXPECT_EQ(closed, 0);
    }
    EXPECT_EQ(closed, 0);
}

TEST(TcpConnectionTest, ThrowingClosedNotRetried)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    int calls = 0;
    connection.on_closed([&](auto &, std::error_code) {
        ++calls;
        throw std::runtime_error("closed");
    });
    connection.close();
    EXPECT_THROW(loop.loop(), std::runtime_error);
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(calls, 1);
}

TEST(TcpConnectionTest, OutputSelfReplaceAndOneNotificationPerWork)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::ConnectionOptions options;
    options.output_limit = 8;
    options.output_low_watermark = 3;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    int first = 0, next = 0, closed = 0;
    bool inside = false;
    connection.on_output_available([&](auto &current) {
        ++first;
        current.on_output_available([&](auto &self) {
            ++next;
            inside = true;
            self.close();
            loop.quit();
            EXPECT_EQ(closed, 0);
            inside = false;
        });
    });
    connection.on_closed([&](auto &, std::error_code) {
        EXPECT_FALSE(inside);
        ++closed;
    });
    connection.start();
    connection.send(tcp_test::bytes("12345678"));
    tcp_test::drive(loop, [&] { return first == 1; });
    connection.send(tcp_test::bytes("abcdefgh"));
    loop.loop();
    EXPECT_EQ(next, 1);
    EXPECT_EQ(closed, 0);
    tcp_test::drive(loop, [&] { return closed == 1; });
}

TEST(TcpConnectionTest, CallbackErrorDoesNotCloseLivePeerOrLoseOutput)
{
    snet::EventLoop loop;
    tcp_test::Pair bad, good;
    snet::TcpConnection first(loop, std::move(bad.accepted)), second(loop, std::move(good.accepted));
    first.on_data([](auto &self) {
        self.send(tcp_test::bytes("reply"));
        throw std::runtime_error("app");
    });
    first.start();
    second.start();
    bad.send("request");
    EXPECT_THROW(loop.loop(), std::runtime_error);
    EXPECT_EQ(second.send(tcp_test::bytes("alive")).status, snet::SendStatus::accepted);
    std::string a, b;
    tcp_test::drive(loop, [&] {
        a += bad.read();
        b += good.read();
        return a == "reply" && b == "alive";
    });
}

TEST(TcpConnectionTest, FullInputHalfCloseWaitsForCapacity)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::ConnectionOptions options;
    options.input_limit = 4;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    int data = 0, eof = 0;
    connection.on_data([&](auto &) { ++data; });
    connection.on_eof([&](auto &) { ++eof; });
    connection.start();
    pair.send("last");
    tcp_test::check(::shutdown(pair.peer.get_fd(), SHUT_WR), "shutdown");
    tcp_test::drive(loop, [&] { return data == 1; });
    for (int i = 0; i < 3; ++i) {
        snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
        stop.schedule();
        loop.loop();
    }
    EXPECT_EQ(data, 1);
    EXPECT_EQ(eof, 0);
    EXPECT_EQ(tcp_test::text(connection.input_data()), "last");
    connection.consume_input(1);
    tcp_test::drive(loop, [&] { return eof == 1; });
    EXPECT_EQ(tcp_test::text(connection.input_data()), "ast");
}

TEST(TcpConnectionTest, UnregisteredPauseResume)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.pause_reading();
    connection.start();
    pair.send("queued");
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_TRUE(connection.input_data().empty());
    connection.resume_reading();
    tcp_test::drive(loop, [&] { return connection.input_data().size() == 6; });
}

TEST(TcpConnectionTest, PausedReadAndPeerReset)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    connection.pause_reading();
    // A pending output attempt supplies useful interest even while input is paused.
    connection.send(tcp_test::bytes("reply"));
    linger reset{1, 0};
    tcp_test::check(::setsockopt(pair.peer.get_fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)), "linger");
    pair.peer.close();
    int closed = 0;
    connection.on_closed([&](auto &, std::error_code error) {
        EXPECT_TRUE(error);
        ++closed;
    });
    tcp_test::drive(loop, [&] { return closed == 1; });
}

TEST(TcpConnectionTest, CloseSavedBatchReuseFdThenThrow)
{
    snet::EventLoop loop;
    tcp_test::Pair a, b;
    int old_fd = b.accepted.get_fd();
    snet::TcpConnection first(loop, std::move(a.accepted)), second(loop, std::move(b.accepted));
    snet::Socket replacement;
    int second_calls = 0, closed = 0;
    second.on_data([&](auto &) { ++second_calls; });
    second.on_closed([&](auto &, std::error_code) { ++closed; });
    first.on_data([&](auto &self) {
        second.close();
        auto fresh = tcp_test::make_socket(AF_INET);
        if (fresh.get_fd() != old_fd) {
            tcp_test::check(::dup2(fresh.get_fd(), old_fd), "dup2");
            replacement = snet::Socket(old_fd);
        } else
            replacement = std::move(fresh);
        self.pause_reading();
        throw std::runtime_error("app");
    });
    first.start();
    second.start();
    a.send("a");
    b.send("b");
    EXPECT_THROW(loop.loop(), std::runtime_error);
    // If second happened to dispatch first, at most that one legitimate delivery is allowed.
    int before = second_calls;
    tcp_test::drive(loop, [&] { return closed == 1; });
    EXPECT_EQ(second_calls, before);
    EXPECT_NE(::fcntl(replacement.get_fd(), F_GETFD), -1);
}

TEST(TcpConnectionTest, SlowEchoPumpsRetainedInputOnOutputAvailability)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::ConnectionOptions options;
    options.input_limit = 8;
    options.output_limit = 4;
    options.output_low_watermark = 0;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    bool eof = false, closed = false;
    int pauses = 0;
    auto pump = [&](auto &current) {
        auto sent = current.send(current.input_data());
        current.consume_input(sent.accepted_bytes);
        if (!current.input_data().empty()) {
            ++pauses;
            current.pause_reading();
        } else {
            current.resume_reading();
            if (eof)
                current.finish_sending();
        }
    };
    connection.on_data(pump);
    connection.on_output_available(pump);
    connection.on_eof([&](auto &current) {
        eof = true;
        pump(current);
    });
    connection.on_closed([&](auto &, std::error_code error) {
        EXPECT_FALSE(error);
        closed = true;
    });
    connection.start();
    const std::string source(129, 'x');
    pair.send(source);
    tcp_test::check(::shutdown(pair.peer.get_fd(), SHUT_WR), "shutdown");
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return closed && pair.eof;
    });
    EXPECT_EQ(received, source);
    EXPECT_GT(pauses, 0);
}

TEST(TcpConnectionTest, ResetOffersReadableFinalBytesBeforeClosure)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    int fd = pair.accepted.get_fd();
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    const std::string payload = "before-reset-1234";
    pair.send(payload);
    linger reset{1, 0};
    tcp_test::check(::setsockopt(pair.peer.get_fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)), "linger");
    pair.peer.close();
    char peek[32];
    ASSERT_EQ(::recv(fd, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT), static_cast<ssize_t>(payload.size()));
    std::vector<std::string> events;
    connection.on_data([&](auto &self) {
        events.push_back("data");
        EXPECT_EQ(tcp_test::text(self.input_data()), payload);
        auto result = self.send(tcp_test::bytes("reply"));
        EXPECT_EQ(result.status, snet::SendStatus::io_error);
        EXPECT_EQ(result.error.value(), ECONNRESET);
    });
    connection.on_eof([&](auto &) { events.push_back("eof"); });
    connection.on_closed([&](auto &, std::error_code error) {
        EXPECT_EQ(error.value(), ECONNRESET);
        events.push_back("closed");
    });
    tcp_test::drive(loop, [&] { return !events.empty() && events.back() == "closed"; });
    EXPECT_EQ(events, (std::vector<std::string>{"data", "closed"}));
    EXPECT_EQ(tcp_test::text(connection.input_data()), payload);
}

TEST(TcpConnectionTest, FinalResetDataThrowStillClosesAndSkipsNotifications)
{
    snet::EventLoop loop;
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
    int data = 0, closed = 0;
    connection.on_data([&](auto &self) {
        ++data;
        EXPECT_EQ(tcp_test::text(self.input_data()), "final");
        throw std::runtime_error("app");
    });
    connection.on_closed([&](auto &, std::error_code) { ++closed; });
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    EXPECT_THROW(loop.loop(), std::runtime_error);
    EXPECT_EQ(data, 1);
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    stop.schedule();
    loop.loop();
    EXPECT_EQ(closed, 0);
}
