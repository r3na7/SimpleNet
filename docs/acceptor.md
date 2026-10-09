# Accepting TCP connections

`Acceptor` services an **already prepared non-blocking TCP listening Socket** in one
EventLoop thread. It owns the listener and its stable Channel, accepts IPv4/IPv6
connections, and transfers each new Socket to a receiver. It does not create the
listener, own client connections, frame a protocol, run threads, or depend on TcpServer.

## Listener and client sockets

The application creates a TCP socket, sets its options, binds an address, and calls
listen. Non-blocking mode is mandatory; CLOEXEC is recommended at creation. Backlog,
SO_REUSEADDR, IPv6-only mode, and address selection belong to the creator. TCP,
listening, and non-blocking properties are preconditions, not runtime-probing checks.
Empty Socket and zero acceptance budget are rejected with invalid_argument.

Each successful accept4 creates a **different fd**. Acceptor immediately owns it
through Socket and transfers that Socket by value to on_accept. Accepted descriptors
always have SOCK_NONBLOCK | SOCK_CLOEXEC. The listener remains separate. A receiver
that drops its Socket closes the connection through RAII; a receiver that moves it
into TcpConnection transfers ownership. Acceptor keeps no client list.

Construction owns resources and prepares internal handlers without registering.
Configure on_accept and stable ownership before start. The object cannot copy/move;
a unique_ptr may move while the object stays at its address. EventLoop must outlive
it. Never independently close or adopt a borrowed fd.

## API and lifecycle

```cpp
snet::Acceptor acceptor(loop, std::move(listening_socket), {32});
acceptor.on_accept([](snet::Socket client) { /* Take or deliberately decline ownership. */ });
acceptor.on_error([](snet::Acceptor& self, std::error_code reason) { /* Resource policy. */ });
acceptor.start();
```

`AcceptorOptions::accept_call_budget` defaults to 32 attempts per dispatch, must be
positive, and is fixed at construction. start requires a receiver. It throws
logic_error for a missing handler, repeated activation, or a closed object. Failed
registration preserves the created object, its listener, and handlers for retry.

| Operation | Behavior |
|---|---|
| pause_accepting | Unregisters, retaining the listener; idempotent and allowed before start. |
| resume_accepting | Requires receiver, restores active registration; failed registration stays paused. |
| close | Idempotently unregisters and closes listener; handed-off clients remain independent. |

A pre-start pause is retained by start. Pre-start resume only clears that pause,
without registration. Resume after close throws logic_error; pause after close does
nothing. Public methods never synchronously accept or call application handlers.

Pause prevents our accept4 calls. The kernel may still complete handshakes and queue
connections up to its limits; it is not a guarantee of immediate client rejection.
Close permanently ends this listener's service, without closing handed-off sockets.

## Callbacks and exceptions

`AcceptCallback = std::function<void(Socket)>` receives one exclusive client owner.
`ErrorCallback = std::function<void(Acceptor&, std::error_code)>` reports an accept4
resource error **after** pausing. Both are invoked from Channel dispatch in the loop
thread. They may pause/resume/close and replace/clear themselves or another handler.
Current callable lifetime and mutable state are preserved until return; replacement
survives exceptions. Clearing an active receiver pauses acceptance. Installing a
new receiver does not resume automatically; explicit resume is required. Missing
receiver prevents start/resume. Clearing on_error leaves resource-pause behavior intact.

After each accepted-client callback, state is checked before another accept4. If an
error callback resumes immediately, that error dispatch still ends; later LT readiness
can accept clients. Each attempted syscall, including EINTR or a failed pending
connection, consumes the dispatch budget. Remaining queued clients produce another
LT event. No prepared LoopWork is needed for continuation.

Application exceptions propagate out of loop after unwinding. The delivered client
is not retried. An untransferred Socket is released on every exception path; clients
already retained by the receiver remain its responsibility. Listener ownership and
handler replacement survive. A throwing receiver does not automatically pause/close
the listener; a throwing resource handler leaves the already-established pause unless
it explicitly changed the state first.

## Error policy

| accept4 result | Behavior |
|---|---|
| EAGAIN/EWOULDBLOCK | End this dispatch; no error callback. |
| EINTR | Retry within the attempt budget. |
| ECONNABORTED, ENETDOWN, EPROTO, ENOPROTOOPT, EHOSTDOWN, ENONET, EHOSTUNREACH, EOPNOTSUPP, ENETUNREACH | Skip failed pending connection, continue within budget. |
| EMFILE, ENFILE, ENOMEM, ENOBUFS | Pause first, notify on_error with preserved errno, end dispatch. |
| Other errno | Throw system_error, preserving the listener. |

After resource exhaustion, the application frees resources and calls resume. There
is no automatic timer, retry, or reserve-fd trick. Empty on_error still leaves the
listener paused. Registration/allocation failures propagate separately.

Listener EPOLLERR checks SO_ERROR; a nonzero reason or getsockopt failure propagates
system_error. HUP without a nonzero SO_ERROR propagates EIO as an unexpected listener
state. These are not client EOF and do not call the resource-error handler. After
catching such an exception, the application chooses pause/close or another policy.

Unexpected EPOLL_CTL_DEL failure diagnoses syscall, fd, and errno then terminates:
freeing a Channel still retained by Poller is unsafe. Normal close/destruction
unregister before closing fd and invalidate saved pending event references.

## Lifetime

Keep Acceptor alive through its Channel dispatch and user callbacks. Do not destroy
it from its own executing callback; close changes service state, not object lifetime.
A standalone owner can destroy it after loop returns/unwinds. Destructor unregisters
and closes without calling user code. EventLoop owns neither Acceptor nor clients.
Acceptor has no deferred on_closed; connections have their own independent lifecycle.

## One-client example

This example demonstrates assembly of the components. Its **application policy** is
to accept one client, read a one-byte request, queue `ack`, and finish sending. The
peer must finish its sending direction so that TcpConnection can complete both
sides and notify on_closed. Resource exhaustion fails the example via exception.
This is not a TcpServer implementation and is not a general message protocol.

```cpp
#include <simplenet/Simplenet.hpp>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <utility>

void serve_one(snet::Address address)
{
    auto check = [](int result, const char* call) {
        if (result == -1)
            throw std::system_error(errno, std::system_category(), call);
    };
    if (address.family() != AF_INET && address.family() != AF_INET6)
        throw std::invalid_argument("TCP IPv4/IPv6 address required");
    int fd = ::socket(address.family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    check(fd, "socket");
    snet::Socket listener(fd);
    int reuse = 1;
    check(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)), "setsockopt");
    check(::bind(fd, address.data(), address.size()), "bind");
    check(::listen(fd, 64), "listen");

    snet::EventLoop loop;
    std::unique_ptr<snet::TcpConnection> connection;
    snet::Acceptor acceptor(loop, std::move(listener));
    acceptor.on_accept([&](snet::Socket client) {
        acceptor.close(); // One-client policy; the accepted socket is independent.
        connection = std::make_unique<snet::TcpConnection>(loop, std::move(client));
        connection->on_data([](auto& current) {
            current.consume_input(current.input_data().size());
            constexpr std::string_view reply = "ack";
            current.send({reply.data(), reply.size()}); // Fits the default empty output queue.
            current.finish_sending();
        });
        connection->on_closed([&](auto&, std::error_code) { loop.quit(); });
        connection->start();
    });
    acceptor.on_error([](auto&, std::error_code reason) {
        throw std::system_error(reason, "listener resource exhaustion");
    });
    acceptor.start();
    loop.loop();
} // All objects are destroyed outside their own dispatch; the loop outlives them.
```

See [TcpConnection](tcp-connection.md) for queue acceptance, EOF, and protocol
backpressure, and [Reactor](reactor.md) for loop and ownership contracts.
