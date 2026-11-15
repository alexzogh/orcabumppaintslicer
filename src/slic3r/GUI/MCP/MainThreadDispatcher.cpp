#include "MainThreadDispatcher.hpp"

#include <algorithm>
#include <utility>

namespace Slic3r { namespace GUI {

MainThreadDispatcher::MainThreadDispatcher(Poster poster, IsMainThread is_main_thread)
    : m_poster(std::move(poster)), m_is_main_thread(std::move(is_main_thread))
{}

MainThreadDispatcher::~MainThreadDispatcher() { shutdown(); }

MainThreadDispatcher::Status MainThreadDispatcher::call(std::function<void()> fn, std::chrono::milliseconds timeout)
{
    if (!fn)
        return Status::Ok;

    // Already on the thread that would run the work: posting and then waiting would block the very
    // thread the queue is drained by, and nothing would ever run it. Run it where we stand.
    if (m_is_main_thread && m_is_main_thread()) {
        fn();
        return Status::Ok;
    }

    auto call = std::make_shared<Call>();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown)
            return Status::ShuttingDown;
        // Drop the states both sides have already let go of, so a long-lived dispatcher does not
        // keep one slot per call ever made.
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                       [](const std::weak_ptr<Call> &w) { return w.expired(); }),
                        m_pending.end());
        m_pending.push_back(call);
    }

    // `call` is captured by shared_ptr, which is the point: the queue entry outlives a waiter that
    // has given up, so a late run finds live state to look at rather than a dangling reference.
    m_poster([call, fn = std::move(fn)]() {
        {
            std::lock_guard<std::mutex> lock(call->mutex);
            // The waiter gave up, or the application is shutting down. Running now would change the
            // model for a request nobody is listening to any more - and one the client may well
            // have retried in the meantime.
            if (call->abandoned)
                return;
            call->started = true;
        }

        fn();

        {
            std::lock_guard<std::mutex> lock(call->mutex);
            call->done = true;
        }
        call->cv.notify_all();
    });

    std::unique_lock<std::mutex> lock(call->mutex);
    if (call->cv.wait_for(lock, timeout, [&call] { return call->done || call->abandoned; })) {
        if (call->done)
            return Status::Ok;
        // Woken by shutdown() rather than by the work finishing.
        return call->started ? Status::TimedOutMaybeRan : Status::ShuttingDown;
    }

    // Timed out. Abandoning it under the same mutex the queue entry takes before it sets `started`
    // is what makes the two outcomes exclusive: either the entry has not begun and never will, or
    // it had already begun and there is nothing left to cancel.
    const bool started = call->started;
    call->abandoned    = true;
    return started ? Status::TimedOutMaybeRan : Status::TimedOutNotRun;
}

void MainThreadDispatcher::shutdown()
{
    std::vector<std::shared_ptr<Call>> pending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown)
            return;
        m_shutdown = true;
        for (const std::weak_ptr<Call> &w : m_pending)
            if (std::shared_ptr<Call> c = w.lock())
                pending.push_back(std::move(c));
        m_pending.clear();
    }

    for (const std::shared_ptr<Call> &c : pending) {
        {
            std::lock_guard<std::mutex> lock(c->mutex);
            c->abandoned = true;
        }
        c->cv.notify_all();
    }
}

}} // namespace Slic3r::GUI
