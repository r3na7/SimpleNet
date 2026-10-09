# Established TCP connections

`TcpConnection` is a C++20, Linux, single-threaded component for an already
established non-blocking IPv4 or IPv6 TCP socket. It owns the `Socket`, one internal
`Channel`, incoming and outgoing `Buffer`s, and one prepared `LoopWork` record.
It uses level-triggered epoll. It does not listen, accept, connect, frame messages,
retry a protocol transaction, or run threads. Socket establishment is currently
performed by the application; no Acceptor or Connector is implemented yet.

## Ownership and activation

Construct with `TcpConnection(loop, std::move(socket), options)`. The socket must
be exclusively owned, connected TCP, and non-blocking; these are caller
preconditions, not runtime socket-probing checks. An empty wrapper and invalid
options are rejected. Acquired resources are released if construction fails.
Never close or adopt a borrowed fd independently.

Construction prepares handlers but does not register or receive anything. Install
application handlers and establish a stable owner before `start()`. Start failure
leaves the connection unactivated and permits retry. Calling start twice or after
close throws `std::logic_error`. Sending or finishing before start also throws.
A pre-start read pause is allowed: start activates the object without a useless
registration, and resume subsequently registers it.

Connections cannot be copied or moved. Keep the object at a stable address, for
example behind a `unique_ptr`. The loop must outlive it. Every operation, including
handler installation, takes place in the one loop thread. Do not destroy the
connection inside its Channel dispatch, application callback, or prepared work.
`close()` and destruction are different: close ends I/O; the owner removes the
object only after active frames and pending references are gone.

## Two independent byte streams

Incoming: kernel receive queue → bounded `recv` group → input buffer → `on_data`.
The group stops at EAGAIN, EOF, a terminal error, full input, or a read budget.
`on_data` is called once if new bytes were added and sees all unconsumed input,
including older bytes. It is not a complete-message notification. The application
frames its protocol and calls `consume_input(n)` for processed bytes. No callback
is generated merely because older unconsumed bytes remain.

`input_data()` is a borrowed read-only span. Reacquire it after any input
modification and do not retain it past object destruction. It remains readable
after close. Over-consumption throws before changing input. `consume_input(0)` is
a no-op. Consuming bytes may reconcile interest, but never calls recv or a user
handler synchronously.

Outgoing: application `send(span)` → copied prefix in output buffer → deferred
send attempts → kernel send queue → peer. `send` performs no synchronous syscall,
epoll change, or application callback. Its copied prefix need not survive the
call; the application retains responsibility for the unaccepted suffix.

| Status | Meaning |
|---|---|
| `accepted` | The whole supplied span was copied into the library queue. |
| `would_block` | Capacity allowed only a prefix, possibly zero bytes. Retry the suffix later. |
| `sending_finished` | Finish was requested; no new bytes can be queued. |
| `closed` | The local socket is already closed. |
| `io_error` | A terminal socket error is known during final-data notification before closure. |

`accepted_bytes` describes queue acceptance, never peer delivery. In this
queue-only implementation terminal statuses have zero accepted bytes;
`io_error` also carries the first socket error. Sending an empty span returns
accepted only while active and accepting output; lifecycle checks still apply.
Allocation failure throws and accepts no new bytes, preserving earlier output.
Preparing input memory precedes recv, so its allocation failure leaves kernel
bytes available for a later attempt.

Partial writes preserve the unsent suffix. EAGAIN enables EPOLLOUT interest; a
later writable event retries. Once the queue drains, unnecessary write interest
is removed. Initial attempts and budget continuations use prepared work, so
sending does not depend on a new socket event. Each work action delivers at most
one application notification.

## Limits and backpressure

Options are fixed for the lifetime of each connection:

| Option | Default |
|---|---:|
| `input_limit`, `output_limit` | 65536 useful bytes each |
| `output_low_watermark` | 32768 useful bytes |
| `read_byte_budget`, `write_byte_budget` | 65536 bytes per loop iteration each |
| `read_call_budget`, `write_call_budget` | 16 attempts per iteration each |

Limits and budgets must be positive. The threshold may be zero and must be less
than the output limit. Limits count useful queued bytes, not buffer allocation
capacity. Send and shutdown share the write attempt quota; EINTR consumes an
attempt. Channel dispatch and prepared work share the same per-iteration quotas.

Full input automatically stops reading. Consumption restores capacity only if
there is no explicit pause and no EOF. `pause_reading()` sets the application
pause; `resume_reading()` clears only that pause. Neither receives synchronously.
While stopped, peer data can accumulate in the kernel; TCP flow control eventually
slows the peer. Reading pauses do not pause outgoing sends.

`on_output_available` reports a queue down-cross from **above** the threshold to
**at or below** it. Repeated writes below it do not notify again until another
crossing; pending notifications coalesce. This is neither raw EPOLLOUT nor a
promise that an arbitrary suffix now fits. The application checks `accepted_bytes`
on retry. A closed connection cancels an ordinary pending output notification.

If read interest is paused/full/finished and no blocked output needs monitoring,
the Channel is removed rather than registered with zero useful interest. Idle
EOF/reset detection then waits for restored useful interest. A registered
EPOLLERR is checked with SO_ERROR even when reading is paused. When reading
is permitted, an error allows one bounded final receive group before closure;
the known error is preserved even though getsockopt consumes SO_ERROR. HUP alone is not
EOF and does not override the read pause or capacity limit.

## EOF, finish and close

`recv == 0` ends only the incoming direction. `on_data` receives final bytes before
`on_eof`; each EOF is reported at most once. The application may send a response
after EOF, retain the outgoing direction, or explicitly finish/close it.
When input is full or paused, EOF is observed only after reading becomes possible.

`finish_sending()` immediately rejects further output, then asynchronously drains
the queued bytes and performs `shutdown(SHUT_WR)`. Receiving continues. Repeated
finish requests are harmless; finishing an already closed/failing connection does
nothing. Once EOF is handled and our queue/shutdown are complete, the connection
automatically closes. Incoming bytes remain available until owner destruction.

`close()` immediately unregisters and closes the socket, discards output, retains
input, and schedules one `on_closed(connection, error)` notification. An explicit
close reason is empty unless a terminal socket error was already retained.
Repeated close is harmless. A terminal recv/send/shutdown error closes only that
connection. Positive bytes before a receive error are offered through a final
`on_data` while new I/O is disabled, then closure follows even if that callback
throws. A socket error is not thrown through the loop as an application exception.

Unexpected epoll deregistration failure is an invariant failure: diagnostics
include the syscall, fd, and errno, then terminate. Destruction cannot proceed
while Poller retains a pointer to freed Channel memory. Other Reactor bookkeeping
failures propagate as exceptions; they are distinct from ordinary peer I/O errors.
An internal ADD/MOD failure preserves prepared reconciliation work, so restarting
the loop retries the unapplied mask rather than stranding accepted output.

## Application callbacks and exceptions

Internal Channel callbacks perform socket I/O. Application handlers installed with
`on_data`, `on_eof`, `on_output_available`, and `on_closed` run in loop processing,
never directly from public methods or destructors. They may send, consume, pause,
resume, finish, close, and replace/clear themselves or another handler. The current
callable stays alive until return. Replacement does not replay an already delivered
EOF/close. Installing on_closed after close but before its deferred dispatch is
allowed. Mutable state in an unchanged callable survives successive calls.

Application exceptions propagate out of `loop()` after the handler unwinds and
owner cleanup runs. The throwing notification is not replayed; later ordinary
notifications in that processing are skipped. Self-replacement survives a throw.
Live connections and queued output are preserved. If the connection closed during
that processing, its pending close work is canceled, so on_closed is not delivered
on restart. A throwing on_closed is also never retried. Owners must release closed
objects after the safe boundary even if their app notification was skipped.

Destructor cancels work, unregisters, and closes without application code. A
standalone owner normally retains its object through loop return and destroys it
outside dispatch. The private future TcpServer integration marks closed candidates
and checks cleanup eligibility; users do not manipulate these internal hooks.
EventLoop never owns or deletes connections. See [owner cleanup](reactor.md).

## Standalone echo example

This function consumes an **already established non-blocking** socket, such as an
`accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)` result adopted exactly once into Socket.
Socket creation/listening/accepting is outside this component. The stack owner keeps
the connection alive through loop execution and deferred close.

```cpp
#include <simplenet/Simplenet.hpp>
#include <utility>

void echo_established(snet::Socket socket)
{
    snet::EventLoop loop;
    snet::TcpConnection connection(loop, std::move(socket));
    bool eof = false;
    auto pump = [&](snet::TcpConnection& current) {
        auto input = current.input_data();
        auto sent = current.send(input);
        current.consume_input(sent.accepted_bytes);
        if (!current.input_data().empty()) {
            current.pause_reading();
        } else {
            current.resume_reading();
            if (eof)
                current.finish_sending();
        }
    };
    connection.on_data(pump);
    connection.on_output_available(pump); // Retry old input even without new network data.
    connection.on_eof([&](auto& current) { eof = true; pump(current); });
    connection.on_closed([&](auto&, std::error_code) { loop.quit(); });
    connection.start();
    loop.loop();
} // Connection destroyed after callback/work frames have returned, before the loop.
```

A production application should interpret close reasons and application exceptions
according to its protocol. Local queue acceptance does not make retry on a new
connection safe: request identities/acknowledgements belong to that protocol.
