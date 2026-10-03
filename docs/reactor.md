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

Channel and Poller do not check the last two restrictions. EventLoop checks
only reentry into its own `loop()`.

## Using EventLoop

The following fragment assumes an already open fd supported by epoll.
The caller configures its mode and handles I/O errors.

```cpp
#include <simplenet/Simplenet.hpp>
#include <sys/epoll.h>

// The caller created fd and keeps it open.
snet::EventLoop loop;
snet::Channel channel(fd);
channel.set_events(EPOLLIN);
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
Registrations remain; the loop can be restarted, but redelivery of unprocessed
events is not guaranteed.

## Using Poller for a custom loop

Poller does not invoke callbacks. Use the public
snet::Channel::handle_event() for standard dispatch:

```cpp
snet::Poller poller;
poller.set_timeout(100); // Milliseconds; -1 waits indefinitely, 0 does not wait.
bool running = true;
snet::Channel channel(fd); // The caller has already opened fd.
channel.add_event(EPOLLIN);
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
(default: 1024) is set through `set_max_events()` and does not limit registrations.

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
callbacks in **error → read → write** order for `EPOLLERR`, `EPOLLIN`, and
`EPOLLOUT`. Successful `remove_channel()` cancels the remaining callbacks for
the current event; `update_channel()` and changes to the requested mask do not.
A callback exception propagates to the caller and stops dispatch.

The received mask is not cleared after dispatch or removal.
`handle_event()` does not check registration, event freshness, or repeated
dispatch, and resets the cancellation flag at the start of each call. Do not
call it manually for an unregistered channel using a saved pointer.

`EPOLLHUP`, `EPOLLRDHUP`, and `EPOLLPRI` alone do not invoke callbacks.
A custom loop can inspect them through `get_revents()`, while the standard
EventLoop does not dispatch them. If only `EPOLLHUP` arrives, the loop may
repeatedly receive the event without invoking a callback; unread data may remain
after hangup. The library does not validate mask combinations or provide
separate guarantees for all combinations of `EPOLLET`, `EPOLLONESHOT`, and other flags.
