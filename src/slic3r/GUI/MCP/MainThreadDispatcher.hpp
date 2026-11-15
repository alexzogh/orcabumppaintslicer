#ifndef slic3r_MainThreadDispatcher_hpp_
#define slic3r_MainThreadDispatcher_hpp_

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace Slic3r { namespace GUI {

// Runs work on the GUI thread on behalf of a worker thread, and waits for it.
//
// The MCP server listens on a worker thread, while Model, Plater, every gizmo and the GL context
// are wxWidgets-owned and may only be touched from the main thread. Every tool call therefore has
// to cross that boundary and come back with a result, which is what this does.
//
// Deliberately free of any wxWidgets dependency: it is constructed with a `Poster` that queues a
// callable onto the main thread (wxGetApp().CallAfter) and an `IsMainThread` predicate
// (wxThread::IsMain). That keeps the awkward part - the waiting, the timeout and the lifetime of a
// call nobody is waiting for any more - testable without an event loop, and it is the part most
// likely to go wrong.
//
// Four hazards it exists to handle:
//
//  - The main thread never gets to the work, because it is inside a modal dialog or a long
//    operation. A tool call must fail rather than hang the client forever, so every call carries a
//    timeout.
//  - A call that timed out may still be sitting in the main thread's queue. Running it then would
//    mutate the model for a request nobody is listening to, and the client may well have retried in
//    the meantime - so an abandoned call is skipped rather than run late.
//  - The call arrives from the main thread itself (a tool invoked from in-process code rather than
//    from the socket). Posting and then waiting would deadlock against the thread that is supposed
//    to run it, so that case runs inline instead.
//  - The application is shutting down with calls still in flight. Those waiters have to be released,
//    not left blocked on an event loop that is never going to run again.
class MainThreadDispatcher
{
public:
    // Queues a callable to run on the main thread. wxGetApp().CallAfter in the real application.
    using Poster = std::function<void(std::function<void()>)>;
    // True when the calling thread *is* the main thread. wxThread::IsMain in the real application.
    using IsMainThread = std::function<bool()>;

    enum class Status {
        // The work ran on the main thread (or inline) and has finished.
        Ok,
        // The timeout elapsed before the main thread started the work, and the work has been
        // abandoned: it is guaranteed never to run. The caller may safely retry.
        TimedOutNotRun,
        // The timeout elapsed while the work was already running. Whether it completed is unknown,
        // so the caller must treat the operation as indeterminate and must NOT blindly retry a
        // mutation. There is no way to cancel a main-thread call once it has begun.
        TimedOutMaybeRan,
        // shutdown() has been called; nothing was posted.
        ShuttingDown,
    };

    MainThreadDispatcher(Poster poster, IsMainThread is_main_thread);
    ~MainThreadDispatcher();

    MainThreadDispatcher(const MainThreadDispatcher &) = delete;
    MainThreadDispatcher &operator=(const MainThreadDispatcher &) = delete;

    // Runs `fn` on the main thread and waits up to `timeout` for it to finish.
    //
    // `fn` writes its result wherever the caller wants it; this returns only whether it ran. That
    // keeps the dispatcher free of any opinion about what a tool call returns, and the
    // happens-before edge the waiter needs is supplied by the same mutex that publishes `done`, so a
    // plain captured local is safe to read after a result of Ok.
    Status call(std::function<void()> fn, std::chrono::milliseconds timeout);

    // Releases every waiter and abandons every call still queued. Calls made afterwards return
    // ShuttingDown without posting anything. Idempotent.
    void shutdown();

private:
    // One in-flight call, shared between the waiting worker thread and the main thread's queue
    // entry. Held by shared_ptr precisely so the queue entry stays valid after the waiter has given
    // up and walked away.
    struct Call
    {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    started   = false;
        bool                    done      = false;
        bool                    abandoned = false;
    };

    Poster       m_poster;
    IsMainThread m_is_main_thread;

    std::mutex                        m_mutex;
    bool                              m_shutdown = false;
    // Weak, so a finished call's state is freed when both sides drop it rather than accumulating
    // here for the life of the application.
    std::vector<std::weak_ptr<Call>>  m_pending;
};

}} // namespace Slic3r::GUI

#endif // slic3r_MainThreadDispatcher_hpp_
