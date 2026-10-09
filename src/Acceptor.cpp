#include "Acceptor.hpp"
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
struct FlagGuard {
    bool &flag;
    explicit FlagGuard(bool &value) noexcept : flag(value)
    {
        assert(!flag);
        flag = true;
    }
    ~FlagGuard() noexcept { flag = false; }
};
snet::AcceptorOptions checked_options(snet::AcceptorOptions options)
{
    if (!options.accept_call_budget)
        throw std::invalid_argument("Accept budget must be positive");
    return options;
}
int checked_fd(const snet::Socket &socket)
{
    if (!socket.is_open())
        throw std::invalid_argument("Empty listening socket");
    return socket.get_fd();
}
[[noreturn]] void del_failure(int fd, int error) noexcept
{
    char message[128];
    auto *at = message;
    const char prefix[] = "Acceptor EPOLL_CTL_DEL fd=";
    std::memcpy(at, prefix, sizeof(prefix) - 1);
    at += sizeof(prefix) - 1;
    at = std::to_chars(at, message + sizeof(message) - 16, fd).ptr;
    const char suffix[] = " errno=";
    std::memcpy(at, suffix, sizeof(suffix) - 1);
    at += sizeof(suffix) - 1;
    at = std::to_chars(at, message + sizeof(message) - 1, error).ptr;
    *at++ = '\n';
    ::write(STDERR_FILENO, message, static_cast<std::size_t>(at - message));
    std::terminate();
}
} // namespace
snet::Acceptor::Acceptor(EventLoop &loop, Socket socket, AcceptorOptions options)
    : loop_(loop), socket_(std::move(socket)), options_(checked_options(options)), channel_(checked_fd(socket_))
{
    channel_.set_read_callback([this] { handle_accept(); });
    channel_.set_error_callback([this] { handle_error(); });
}
snet::Acceptor::~Acceptor() noexcept
{
    assert(!servicing_ && !callback_active_);
    close();
}
void snet::Acceptor::on_accept(AcceptCallback callback)
{
    replace(accept_callback_, std::move(callback));
    if (state_ == State::active && !has_receiver())
        pause_accepting();
}
void snet::Acceptor::on_error(ErrorCallback callback) { replace(error_callback_, std::move(callback)); }
void snet::Acceptor::start()
{
    if (state_ != State::created)
        throw std::logic_error("Acceptor cannot be activated twice or after closure");
    if (!has_receiver())
        throw std::logic_error("Acceptance requires a receiver");
    if (!paused_) {
        channel_.set_events(EPOLLIN);
        loop_.update_channel(&channel_);
        registered_ = true;
    }
    state_ = State::active;
}
void snet::Acceptor::pause_accepting()
{
    paused_ = true;
    detach();
}
void snet::Acceptor::resume_accepting()
{
    if (state_ == State::closed)
        throw std::logic_error("Closed acceptor cannot resume");
    if (!has_receiver())
        throw std::logic_error("Acceptance requires a receiver");
    if (state_ == State::active && paused_) {
        channel_.set_events(EPOLLIN);
        loop_.update_channel(&channel_);
        registered_ = true;
    }
    paused_ = false;
}
void snet::Acceptor::close()
{
    if (state_ == State::closed)
        return;
    state_ = State::closed;
    detach();
    socket_.close();
}
void snet::Acceptor::detach() noexcept
{
    if (!registered_)
        return;
    try {
        loop_.remove_channel(&channel_);
    } catch (const std::system_error &error) {
        del_failure(socket_.get_fd(), error.code().value());
    } catch (...) {
        del_failure(socket_.get_fd(), errno);
    }
    registered_ = false;
}
bool snet::Acceptor::has_receiver() const noexcept
{
    return static_cast<bool>(accept_callback_.callback) || (accept_callback_.executing && !accept_callback_.replaced);
}
bool snet::Acceptor::accepting() const noexcept { return state_ == State::active && !paused_ && has_receiver(); }
void snet::Acceptor::handle_accept()
{
    if (!accepting())
        return;
    FlagGuard guard(servicing_);
    for (std::size_t calls = 0; calls < options_.accept_call_budget && accepting(); ++calls) {
        const int fd = ::accept4(socket_.get_fd(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) {
            Socket client(fd);
            invoke(accept_callback_, std::move(client));
            continue;
        }
        const int error = errno;
        if (error == EAGAIN || error == EWOULDBLOCK)
            return;
        if (error == EINTR)
            continue;
        throw std::system_error(error, std::system_category(), "accept4");
    }
}
void snet::Acceptor::handle_error() {}
