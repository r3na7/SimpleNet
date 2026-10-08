#pragma once

namespace snet
{
class EventLoop;
namespace detail
{
/// @brief The safe point at which an owner's internal cleanup is called.
enum class CleanupReason {
    normal,   ///< The socket batch and bounded work phase finished normally.
    exception ///< Dispatch unwound before the original exception leaves loop().
};

/**
 * @brief Stable-address registration of a non-owning, single-threaded owner cleanup hook.
 * @pre The owner/context and registration outlive every invocation; EventLoop outlives the registration.
 * @warning Hooks cannot invoke application callbacks, reenter loop(), or create/destroy cleanup registrations.
 */
class LoopCleanup
{
public:
    /// @brief A nonthrowing internal hook; context is never owned by EventLoop.
    using Action = void (*)(void *, CleanupReason) noexcept;
    /**
     * @brief Registers a prepared owner hook without requesting a loop iteration.
     * @param loop The associated loop, used in one thread.
     * @param context A stable non-owning pointer; nullptr is allowed if the hook supports it.
     * @param action A non-null noexcept function.
     * @throws std::invalid_argument The action is null.
     * @throws std::logic_error Construction was attempted during cleanup dispatch.
     */
    LoopCleanup(EventLoop &loop, void *context, Action action);
    /// @brief Unlinks this hook without invoking it; forbidden during cleanup dispatch.
    ~LoopCleanup() noexcept;
    /// @brief Registered addresses cannot be copied.
    LoopCleanup(const LoopCleanup &) = delete;
    /// @brief Registered addresses cannot be copy-assigned.
    LoopCleanup &operator=(const LoopCleanup &) = delete;
    /// @brief Registered addresses cannot be moved.
    LoopCleanup(LoopCleanup &&) = delete;
    /// @brief Registered addresses cannot be move-assigned.
    LoopCleanup &operator=(LoopCleanup &&) = delete;

private:
    friend class snet::EventLoop;
    EventLoop &loop_;
    void *context_;
    Action action_;
    LoopCleanup *prev_ = nullptr;
    LoopCleanup *next_ = nullptr;
};
} // namespace detail
} // namespace snet
