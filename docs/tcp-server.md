# TCP server assembly and ownership

`TcpServer` assembles an Acceptor and server-owned TcpConnection objects on a caller's
EventLoop. Linux, C++20, TCP IPv4/IPv6, non-blocking sockets, and one Reactor thread
remain the supported scope. It does not run a thread, establish sockets, frame a
protocol, or quit the shared loop.

## Construct, configure, start

The application prepares a non-blocking TCP listening Socket (socket/options/bind/
listen), then transfers it exactly once. These are caller preconditions; an empty
Socket and invalid options are rejected. Socket creation is described in the
[Acceptor guide](acceptor.md).

```cpp
snet::TcpServerOptions options;
options.connection.output_limit = 65536;
options.connection.output_low_watermark = 32768;
options.acceptor.accept_call_budget = 32;
snet::TcpServer server(loop, std::move(listener), options);
server.on_connection([](snet::TcpConnection& client) {
    // Set on_data/on_eof/on_output_available/on_closed here.
});
server.start();
loop.loop(); // Application controls execution and when to quit.
```

ConnectionOptions defaults and validation are unchanged. They are checked during
server construction and copied to every new connection. AcceptorOptions also keep
their default acceptance budget32. Options are fixed for the server's lifetime.

Construction prepares internal Acceptor callbacks and one LoopCleanup registration,
but does not register the listener for network readiness. start requires on_connection.
Failure preserves an unstarted server, listener, and handlers for explicit retry.
Repeated successful start or start after permanent stop throws logic_error. Resume
also requires configuration; before start it only clears a preliminary pause.

Server and its connections stay at stable addresses. Copy/move of server objects is
forbidden; moving unique_ptr owners is allowed. All methods/handlers run in one loop
thread; EventLoop must outlive the server.

## New client configuration

For each accepted Socket, the server creates a TcpConnection, stores its unique_ptr
in a stable owner record, installs an internal close mark, invokes on_connection,
then starts the connection unless it was closed during configuration.

`ConnectionCallback = std::function<void(TcpConnection&)>` runs **before activation**.
Install app handlers and state here; do not call start yourself. Sending or finishing
before activation still throws logic_error. A configuration-time close skips start
and allows ordinary deferred on_closed before deletion.

If configuration or connection start throws, only that new client is closed. The
original exception exits loop; exception cleanup cancels its pending notification
and releases it after unwinding. Other live clients and their queued output survive.
The server does not retain an unactivated client for an unspecified retry policy.

Callbacks for future connections can be replaced/cleared, including self-replacement.
The current callable stays alive until return, mutable state is retained, and
replacement survives throw. Clearing an active configuration pauses acceptance;
installing a replacement does not resume it automatically. Use resume_accepting.
Replacing a nonempty handler does not itself pause. Existing client handlers are
unchanged by later server configuration replacement.

## Ownership and safe deletion

Each owner record contains unique_ptr<TcpConnection> and a never-reused uint64 id
separate from Linux fd numbers. Map rehash preserves node/connection addresses;
iterators are not retained across insertion. Closed fd reuse cannot redirect an old
candidate to a new connection. Identifier exhaustion throws rather than wrapping.

Closing performs socket/registration teardown, links the already prepared owner
record into the candidate list, and requests cleanup without allocating. Repeated
close does not duplicate candidates. The server processes candidates, not every live
connection on each loop phase. Explicit close_connections may visit all current owners.

Normal cleanup removes only closed objects with no active service/callback/work and
no pending work. A pending on_closed retains its object. Unconsumed input does not
prevent deletion; buffer drain belongs to finish_sending, while close drops output.

Exception cleanup runs after callback unwinding, cancels remaining work for closed
candidates and releases ready objects before the original exception leaves loop.
It never calls app handlers and does not close live connections merely because
another callback threw. Internal marking is independent of user on_closed: empty,
replaced, or throwing app handlers cannot prevent resource release.

A TcpConnection reference is borrowed. It is valid throughout its callback,
including on_closed, but may be deleted afterward. Do not retain/dereference it past
closure; on_closed can be skipped on exception, so it is not an ownership guarantee.
App state can live in callback captures; shared_ptr may share that **state**, not
ownership of TcpConnection.

With limited work budget or quit, some close notifications may remain pending. Those
objects survive until subsequent loop execution or cancellation during server
destruction. Stop does not promise every notification has completed before returning.

## Stop and resource recovery

| Method | Behavior |
|---|---|
| stop_accepting | Permanently closes listener; current clients continue. |
| close_connections | Immediately closes current clients; listener keeps its current state. |
| stop | Stops accepting, then closes current clients. |
| resume_accepting | Resumes temporary pause with configured handler and open listener. |

Stops are idempotent and invoke no app handlers inline. None calls EventLoop::quit.
Stop before start permanently disables start/resume; close_connections before start
has no clients to close. Stop_accepting during on_connection still allows the already
accepted client to finish configuration/start. Close_connections/stop during that
callback also close the saved current client, which is then not activated.

Temporary pause stops extracting queued clients, not necessarily TCP handshakes.
EMFILE/ENFILE/ENOMEM/ENOBUFS pause the internal Acceptor before notifying
`AcceptErrorCallback = std::function<void(TcpServer&, std::error_code)>` through
on_accept_error. Existing clients keep working. Free resources and explicitly resume.
Missing handler leaves acceptance paused; a throwing handler propagates through loop,
retaining the pause unless the application explicitly changed it. Handler replacement/
clear/self-replacement is allowed. Immediate resume still ends the current error
dispatch; later LT readiness continues acceptance.

EAGAIN/EINTR/pending-client errors are handled by Acceptor; unexpected listener,
registration, and allocation errors propagate separately. No automatic timers,
reserve fd, or retry machinery are added. Resume after permanent stop throws.

## Server lifetime

Keep the server alive through its methods, Acceptor dispatch, owned connection
callbacks, and cleanup. Do not delete the server inside these frames. stop is allowed;
destruction must wait until processing returns, for example after loop returns.
Destructor stops listener and clients, cancels work and releases owners/cleanup
registration without application notifications. Existing unexpected DEL failure
policy diagnoses syscall/fd/errno then terminates rather than freeing registered memory.

The persistent runnable server is in [the demonstration guide](examples.md). It uses
signal-based immediate stopping; the following fragment deliberately chooses a
different application policy.

## Two-client echo example

The caller supplies a prepared listening Socket. This example's **application
policy** is to stop service and quit after two clients close. Each peer must finish
its outgoing direction after sending. Resource exhaustion aborts the example via
exception; another application could free resources and resume instead. This is not
a universal graceful-shutdown policy or a guarantee of delivery of accepted bytes.

```cpp
#include <simplenet/Simplenet.hpp>
#include <memory>
#include <system_error>
#include <utility>

void echo_two(snet::Socket listener)
{
    snet::EventLoop loop;
    snet::TcpServer server(loop, std::move(listener));
    int closed = 0;
    server.on_connection([&](snet::TcpConnection& client) {
        auto eof = std::make_shared<bool>(false);
        auto pump = [eof](snet::TcpConnection& current) {
            auto sent = current.send(current.input_data());
            current.consume_input(sent.accepted_bytes);
            if (!current.input_data().empty()) {
                current.pause_reading();
            } else {
                current.resume_reading();
                if (*eof)
                    current.finish_sending();
            }
        };
        client.on_data(pump);
        client.on_output_available(pump); // Retry old input even without new network data.
        client.on_eof([eof, pump](auto& current) { *eof = true; pump(current); });
        client.on_closed([&](auto&, std::error_code) {
            if (++closed == 2) {
                server.stop();
                loop.quit(); // Application chooses this, not TcpServer.
            }
        });
    });
    server.on_accept_error([](auto&, std::error_code reason) {
        throw std::system_error(reason, "listener resource exhaustion");
    });
    server.start();
    loop.loop();
} // Server destroyed outside dispatch; loop remains alive through owner teardown.
```

The same pumping pattern is tested with two clients and tiny input/output limits,
including retained-input retries. See [TcpConnection](tcp-connection.md) for stream
semantics, queue acceptance, independent EOF/finish, and protocol backpressure.
