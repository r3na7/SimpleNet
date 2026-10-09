#pragma once
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <exception>
#include <functional>
#include <simplenet/Simplenet.hpp>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <utility>

namespace demo
{
// Example-local owner: the signal mask and fd survive Channel registration.
class SignalStop
{
    struct Mask {
        sigset_t watched{}, previous{};
        Mask()
        {
            ::sigemptyset(&watched);
            ::sigaddset(&watched, SIGINT);
            ::sigaddset(&watched, SIGTERM);
            if (::sigprocmask(SIG_BLOCK, &watched, &previous) == -1)
                throw std::system_error(errno, std::generic_category(), "block stop signals");
        }
        ~Mask() noexcept
        {
            if (::sigprocmask(SIG_SETMASK, &previous, nullptr) == -1) {
                std::fprintf(stderr, "restore signal mask failed: errno=%d\n", errno);
                std::terminate();
            }
        }
    };
    struct Descriptor {
        int fd;
        explicit Descriptor(const sigset_t &mask) : fd(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC))
        {
            if (fd == -1)
                throw std::system_error(errno, std::generic_category(), "signalfd");
        }
        ~Descriptor() noexcept
        {
            const int saved = errno;
            ::close(fd); // Linux: one attempt, including EINTR.
            errno = saved;
        }
    };

public:
    SignalStop(snet::EventLoop &loop, std::function<void()> action)
        : loop_(loop), descriptor_(mask_.watched), channel_(descriptor_.fd), action_(std::move(action))
    {
        channel_.set_read_callback([this] { receive(); });
        channel_.set_events(EPOLLIN);
        loop_.update_channel(&channel_);
    }
    ~SignalStop() noexcept
    {
        try {
            loop_.remove_channel(&channel_);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "remove signal Channel fd=%d: %s\n", descriptor_.fd, error.what());
            std::terminate();
        }
    }
    SignalStop(const SignalStop &) = delete;
    SignalStop &operator=(const SignalStop &) = delete;
    SignalStop(SignalStop &&) = delete;
    SignalStop &operator=(SignalStop &&) = delete;

private:
    void receive()
    {
        bool requested = false;
        // Drain queued records before restoring the mask on normal shutdown.
        for (;;) {
            signalfd_siginfo info{};
            const auto count = ::read(descriptor_.fd, &info, sizeof(info));
            if (count == sizeof(info)) {
                requested = true;
                continue;
            }
            if (count == -1 && errno == EINTR)
                continue;
            if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (count == -1)
                throw std::system_error(errno, std::generic_category(), "read signalfd");
            throw std::runtime_error("Incomplete signalfd record");
        }
        if (requested && action_)
            action_();
    }
    snet::EventLoop &loop_;
    Mask mask_;
    Descriptor descriptor_;
    snet::Channel channel_;
    std::function<void()> action_;
};
} // namespace demo
