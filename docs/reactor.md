# Reactor guide

This guide describes the current `Channel`, `Poller`, and `EventLoop`.
Examples are fragments for an already open fd, not complete TCP applications.
They do not implement TCP lifecycle management, non-blocking I/O, or buffering.

## Components and ownership

| Component | Responsibility | Ownership |
|---|---|---|
| snet::Channel | fd, requested and received masks, callbacks, dispatch | Stores the fd number without owning the resource |
| snet::Poller | Channel registration and event waiting | Owns the epoll fd; stores non-owning `Channel*` pointers |
| snet::EventLoop | A ready-to-use event waiting and dispatch loop | Owns its Poller |
| Caller | fd and channel creation, I/O, lifetime management | Owns the monitored resources |

These three classes do not create sockets, enable non-blocking mode, or read
or write data. Callbacks perform I/O and handle its results.
They run synchronously, without arguments, in the dispatch thread.

## Common contract

- A set of channels and its Poller/EventLoop is used in one thread;
  there is no internal synchronization.
- Each Channel is used with one Poller or one EventLoop.
  Registration of the same object with multiple Pollers is not checked.
- A registered channel keeps a stable address. Copying and moving the Channel
  object itself are prohibited; moving a `std::unique_ptr<Channel>` is allowed.
- Before destroying a registered channel or closing its fd,
  successfully remove its registration.
- A channel remains alive until its `handle_event()` completes: do not destroy
  it inside its own callback, even after removing registration.
  Objects used by callbacks must also remain alive.
- Reentrant `handle_event()` calls on the same channel are prohibited.
- Do not call the next `poll()` while iterating over the previous batch,
  including from its callbacks.

Channel rejects reentry into its own `handle_event()` with `std::logic_error`.
Poller does not check the restriction on nested `poll()` calls. EventLoop also
rejects reentry into its own `loop()`.

## Using EventLoop

The following fragment assumes an already open fd supported by epoll.
The caller configures its mode and handles I/O errors.

```cpp
#include <simplenet/Simplenet.hpp>
#include <sys/epoll.h>

// The caller created fd and keeps it open.
snet::EventLoop loop;
snet::Channel channel(fd);
channel.set_events(EPOLLIN | EPOLLRDHUP);
channel.set_read_callback([&] {
    // Perform I/O and handle its result.
    loop.quit();
});
loop.update_channel(&channel);
try {
    loop.loop();
} catch (...) {
    // Removing registration may throw;
    // keep the channel and fd alive until it succeeds.
    loop.remove_channel(&channel);
    throw;
}
loop.remove_channel(&channel);
// The caller can now close fd and destroy channel.
```

`quit()` stops the loop after the current batch: the remaining callbacks of
this channel and the remaining channels in the batch are still dispatched.
It does not wake `epoll_wait()`; calls from another thread are unsupported.
Calling it before `loop()` does not prevent startup. An exception from Poller
or a callback stops iteration and exits `loop()` with its execution flags reset.
Registrations remain. After a callback exception, the next `loop()` resumes
the saved batch at the next channel before calling `poll()` again. For a batch
A, B, C, an exception from A leaves B and C pending. A is not retried, including
any remaining callbacks for its event. Further callback exceptions preserve
the new continuation position in the same way.

Pending channels must remain alive or be successfully unregistered before
destruction. Removing a channel between runs clears its entry in the saved
batch, so continuation skips it. New registrations are observed in a subsequent
poll, not inserted into the saved batch. Resuming dispatch does not restore
application state changed by the failed callback.

An exception while preparing a batch in `Poller::poll()` has no such continuation
guarantee: there is no successfully prepared batch to resume. If the last channel
throws, the saved batch is already exhausted and the next run proceeds to a new poll.

## Configuring EventLoop

Use `set_max_events(int)` and `get_max_events()` to configure and inspect the
maximum events per wait (default: 1024). Use `set_timeout(int)` and `get_timeout()`
for the wait timeout (default: -1, meaning an indefinite wait).

```cpp
snet::EventLoop loop;
loop.set_max_events(64);
loop.set_timeout(100); // Wait at most 100 milliseconds per poll.
```

All settings are accessed in the same thread as the loop. They may be changed
before startup, from callbacks, or between runs. Batch capacity must be positive;
invalid values throw `std::invalid_argument` before allocation. A failed capacity
change preserves the previous setting. Changes affect subsequent waits and do
not truncate the current batch or a batch saved after a callback exception.

Timeout values are stored without validation. The supported contract is -1
for an indefinite wait, 0 for an immediate check, and positive milliseconds for
a bounded wait. Expiration does not invoke a timeout callback or return from
`loop()`; the loop waits again. A zero timeout may cause busy polling and high
CPU usage. Changing the timeout does not interrupt an ongoing wait and is not
a mechanism for stopping the loop from another thread.

## Using Poller for a custom loop

Poller does not invoke callbacks. Use the public
snet::Channel::handle_event() for standard dispatch:

```cpp
snet::Poller poller;
poller.set_timeout(100); // Milliseconds; -1 waits indefinitely, 0 does not wait.
bool running = true;
snet::Channel channel(fd); // The caller has already opened fd.
channel.add_event(EPOLLIN | EPOLLRDHUP);
channel.set_read_callback([&] {
    // Perform I/O and handle its result.
    running = false;
});
poller.update_channel(&channel);
try {
    while (running) {
        const auto& channels = poller.poll();
        for (snet::Channel* ready : channels) {
            if (ready != nullptr)
                ready->handle_event();
        }
    }
} catch (...) {
    // Preserve the channel and fd lifetimes if removal fails.
    poller.remove_channel(&channel);
    throw;
}
poller.remove_channel(&channel);
// fd can now be closed.
```

This pattern stops after the current batch. A custom loop can choose different
policies for stopping, handling exceptions, and doing work between waits,
provided it respects the common contract. The maximum events per wait
(default: 1024) can be passed to the constructor, for example `snet::Poller poller(64)`,
and changed later through `set_max_events()`. It does not limit registrations.
Both operations accept positive `int` capacities from 1 through INT_MAX and reject
zero or negative values before converting the size for event buffer allocation.

**Iterate over the internal vector by reference:** `const auto& channels = poller.poll()`.
Successful `remove_channel()` clears the corresponding entries in that vector,
so checking for `nullptr` is required. Using `auto channels = poller.poll()`
creates a copy that does not receive these changes and may contain pointers to
channels already unregistered or destroyed. The next `poll()` replaces the
batch; do not use its previous iterators or references to elements.

## Masks and dispatch

Changing the requested mask does not perform a system call. To apply it,
call `update_channel()` on the appropriate Poller or EventLoop:

```cpp
channel.add_event(EPOLLOUT);
poller.update_channel(&channel);
```

Setting a callback does not change the requested mask. `clear_events()` only
sets that mask to zero: it does not unregister the channel or cancel remaining
callbacks. Even after applying a zero mask, `EPOLLERR` and `EPOLLHUP` may arrive
regardless of the requested events.

`handle_event()` uses a snapshot of the received mask and invokes non-empty
callbacks in **error → read → write** order. `EPOLLERR` invokes error;
any of `EPOLLIN`, `EPOLLRDHUP`, or `EPOLLHUP` invokes read once, even if multiple
read-side bits are present; `EPOLLOUT` invokes write.
Successful `remove_channel()` cancels the remaining callbacks for
the current event; `update_channel()` and changes to the requested mask do not.
A callback exception propagates to the caller and stops dispatch.

Calling `handle_event()` again while it is already executing on the same channel
throws `std::logic_error` before changing its dispatch state. A callback cannot
replace or clear itself through its setter: this also throws `std::logic_error`
and leaves the installed handler unchanged. Other handlers may be replaced;
later callbacks in the same event use their currently installed handlers.
Execution markers are restored on normal return, cancellation, and exceptions,
so handlers can be replaced after their calls have ended. These checks do not
provide thread safety or permit destroying a channel inside its own callback.

The received mask is not cleared after dispatch or removal.
`handle_event()` does not check registration, event freshness, or repeated
dispatch, and resets the cancellation flag at the start of each call. Do not
call it manually for an unregistered channel using a saved pointer.

The read callback checks the incoming side through non-blocking I/O, handling
available data, EAGAIN, EOF, and errors. Hangup may leave unread data; Channel
does not automatically close the fd or unregister the channel. For stream sockets,
`EPOLLRDHUP` indicates that the peer has closed or shut down its sending side;
local sending may still be possible. The caller decides how to finish the connection.

Request `EPOLLRDHUP` explicitly in the interest mask to receive it. `EPOLLHUP`
is reported regardless of the requested mask. `EPOLLPRI` alone does not invoke
callbacks; a custom loop can inspect it through `get_revents()`.
The library does not validate mask combinations or provide
separate guarantees for all combinations of `EPOLLET`, `EPOLLONESHOT`, and other flags.

## Prepared work and owner cleanup

`detail::LoopWork` is a stable-address registration for internal same-thread
work. Construct it before dispatch and keep it alive until its action returns.
`schedule()` coalesces repeated requests; `cancel()` and destruction unlink
pending requests without allocating. Scheduling from its action is allowed,
but does not repeat that action in the current work phase. Destroying or moving
the executing registration is prohibited. The action is fixed at construction;
an empty action is rejected.

When work exists, EventLoop polls sockets with an effective zero timeout without
changing `get_timeout()`. It dispatches the current socket batch, runs at most
`get_work_budget()` prepared records (64 initially), and calls registered owner
cleanup hooks. New work waits for another phase; unfinished older work stays ahead
of newer requests. `set_work_budget(0)` throws without changing the budget.
`iteration_id()` lets components share an I/O budget across socket dispatch and
prepared work in the same iteration; compare ids only for equality.

Closing a socket and destroying its owner are separate operations. EventLoop does
not own connections or accept object ownership. An owner keeps its `unique_ptr`
until the object is unregistered, no handler is executing, and pending references
are finished or canceled. A closed object with a pending close notification stays
alive. Input/output buffer emptiness is not a destruction criterion: draining is
part of the connection's shutdown policy, not generic owner cleanup.

Register one `detail::LoopCleanup` per owner. Its fixed action has signature
`void(void*, detail::CleanupReason) noexcept`, with a non-owning stable context.
The owner can keep a list of marked closed candidates and process only those;
EventLoop invokes owner hooks, never scans their connections. Registration and
removal use embedded links without allocating. A null action throws
`std::invalid_argument`; a null context is allowed when supported by the action.

```cpp
struct CleanupState {
    snet::EventLoop& loop;
    int passes = 0;
};
snet::EventLoop loop;
CleanupState state{loop};
snet::detail::LoopCleanup cleanup(loop, &state,
    [](void* context, snet::detail::CleanupReason reason) noexcept {
        auto& state = *static_cast<CleanupState*>(context);
        // A real owner checks its marked objects here.
        ++state.passes;
        state.loop.quit();
    });
loop.request_cleanup();
loop.loop(); // No socket event is required to reach the cleanup phase.
```

`request_cleanup()` is nonthrowing and coalesces requests into a pending flag.
It never invokes a hook synchronously. It forces an effective zero poll timeout
until the next cleanup phase. The flag is consumed before hooks run, so a request
from a hook survives for another iteration. Hooks run once per safe phase, after
the entire current Channel batch and a bounded work phase, even if quit was
requested. Hook order across owners is unspecified.

After an exception unwinds the handler, hooks run with `CleanupReason::exception`
before the original exception leaves loop(). Owners cancel references to their
closed objects and remove those objects when safe. Live objects and their work
are preserved. During normal completion the reason is `CleanupReason::normal`;
owners retain objects that still require notifications. Throwing work invocations
are not retried; removed pending Channels are skipped on saved-batch continuation.

Hooks are internal lifecycle operations: they must not throw, invoke application
callbacks, or reenter loop. They must not create or destroy cleanup registrations
during a phase; construction is rejected with `std::logic_error`, and destruction
violates a checked precondition. A hook may cancel object-local LoopWork and destroy
unregistered owned objects after their handlers have unwound. It must not destroy
its owner/registration. Owners must avoid repeatedly requesting phases without
progress: a deliberate repeated request causes nonblocking polling.

Before destroying EventLoop, destroy all external owners and their work/cleanup
registrations, including unstarted ones. Owners unregister Channels, close fds,
cancel work and destroy their objects outside their own callbacks. Neither owner
destruction nor EventLoop destruction delivers application notifications.
EventLoop's destructor invokes no work action or cleanup hook, destroys no external
object, and closes only its Poller; debug assertions diagnose surviving
registrations. `TcpServer::stop` will stop service without destroying the server;
the server owner must wait for its callbacks/cleanup to return before destruction.

## Established high-level TCP

`TcpConnection` now implements bounded non-blocking input/output, independent EOF
and outgoing shutdown, and deferred application notifications above this Reactor.
Its Channel and prepared work are internal. It uses level-triggered registration
and shares I/O budgets between socket dispatch and work via `iteration_id()`.
Standalone users own the connection until processing has returned; the loop does
not destroy it. See the [TCP connection guide](tcp-connection.md) for activation,
backpressure, handler replacement, exception and teardown contracts.
