#include "signal_stop.hpp"
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace
{
int signal_fd = -1;
bool fail_create = false, fail_add = false, registered = false, removed = false, close_before_remove = false;
class SignalStopTest : public ::testing::Test
{
protected:
    sigset_t original{}, before{};
    void SetUp() override
    {
        signal_fd = -1;
        fail_create = fail_add = registered = removed = close_before_remove = false;
        ASSERT_EQ(::sigprocmask(SIG_SETMASK, nullptr, &original), 0);
        before = original;
        ::sigaddset(&before, SIGINT);
        ::sigaddset(&before, SIGUSR1);
        ::sigdelset(&before, SIGTERM);
        ASSERT_EQ(::sigprocmask(SIG_SETMASK, &before, nullptr), 0);
    }
    void TearDown() override { EXPECT_EQ(::sigprocmask(SIG_SETMASK, &original, nullptr), 0); }
    void expect_restored()
    {
        sigset_t now{};
        ASSERT_EQ(::sigprocmask(SIG_SETMASK, nullptr, &now), 0);
        for (int sig = 1; sig < NSIG; ++sig)
            EXPECT_EQ(::sigismember(&now, sig), ::sigismember(&before, sig)) << sig;
    }
    void handles(int selected)
    {
        snet::EventLoop loop;
        int called = 0;
        {
            demo::SignalStop stop(loop, [&] {
                ++called;
                loop.quit();
            });
            ASSERT_EQ(::kill(::getpid(), selected), 0);
            loop.loop();
            EXPECT_EQ(called, 1);
        }
        expect_restored();
    }
};
} // namespace
extern "C" int __real_signalfd(int, const sigset_t *, int);
extern "C" int __wrap_signalfd(int fd, const sigset_t *mask, int flags)
{
    if (fail_create) {
        errno = EMFILE;
        return -1;
    }
    const int result = __real_signalfd(fd, mask, flags);
    if (result >= 0)
        signal_fd = result;
    return result;
}
extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);
extern "C" int __wrap_epoll_ctl(int epoll, int operation, int fd, epoll_event *event)
{
    if (fd == signal_fd && operation == EPOLL_CTL_ADD && fail_add) {
        errno = ENOMEM;
        return -1;
    }
    const int result = __real_epoll_ctl(epoll, operation, fd, event);
    if (result == 0 && fd == signal_fd) {
        if (operation == EPOLL_CTL_ADD)
            registered = true;
        if (operation == EPOLL_CTL_DEL)
            removed = true;
    }
    return result;
}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd)
{
    if (fd == signal_fd && registered && !removed)
        close_before_remove = true;
    return __real_close(fd);
}
TEST_F(SignalStopTest, HandlesIntInLoop) { handles(SIGINT); }
TEST_F(SignalStopTest, HandlesTermInLoop) { handles(SIGTERM); }
TEST_F(SignalStopTest, HandlesQueuedSignals)
{
    snet::EventLoop loop;
    int called = 0;
    {
        demo::SignalStop stop(loop, [&] {
            ++called;
            loop.quit();
        });
        ASSERT_EQ(::kill(::getpid(), SIGINT), 0);
        ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);
        loop.loop();
        EXPECT_GE(called, 1);
        sigset_t pending{};
        ASSERT_EQ(::sigpending(&pending), 0);
        EXPECT_EQ(::sigismember(&pending, SIGINT), 0);
        EXPECT_EQ(::sigismember(&pending, SIGTERM), 0);
    }
    expect_restored();
}
TEST_F(SignalStopTest, RestoresPreviouslyMixedMask)
{
    snet::EventLoop loop;
    {
        demo::SignalStop stop(loop, [] {});
        sigset_t blocked{};
        ASSERT_EQ(::sigprocmask(SIG_SETMASK, nullptr, &blocked), 0);
        EXPECT_EQ(::sigismember(&blocked, SIGINT), 1);
        EXPECT_EQ(::sigismember(&blocked, SIGTERM), 1);
    }
    expect_restored();
}
TEST_F(SignalStopTest, SignalfdFailureRestoresMask)
{
    snet::EventLoop loop;
    fail_create = true;
    EXPECT_THROW(demo::SignalStop(loop, [] {}), std::system_error);
    fail_create = false;
    expect_restored();
}
TEST_F(SignalStopTest, RegistrationFailureClosesFdAndRestoresMask)
{
    snet::EventLoop loop;
    fail_add = true;
    EXPECT_THROW(demo::SignalStop(loop, [] {}), std::system_error);
    fail_add = false;
    ASSERT_GE(signal_fd, 0);
    EXPECT_EQ(::fcntl(signal_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    expect_restored();
}
TEST_F(SignalStopTest, UnregistersBeforeClose)
{
    snet::EventLoop loop;
    {
        demo::SignalStop stop(loop, [] {});
        EXPECT_TRUE(registered);
    }
    EXPECT_TRUE(removed);
    EXPECT_FALSE(close_before_remove);
    ASSERT_GE(signal_fd, 0);
    EXPECT_EQ(::fcntl(signal_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    expect_restored();
}
