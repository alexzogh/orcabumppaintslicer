#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <stdexcept>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "slic3r/GUI/MCP/MainThreadDispatcher.hpp"

using Slic3r::GUI::MainThreadDispatcher;
using namespace std::chrono_literals;

namespace {

// A stand-in for the wx event loop: a queue drained when, and only when, the test chooses to. The
// dispatcher takes its poster and its main-thread predicate as constructor arguments precisely so
// the loop can be replaced here, which is what lets these tests run without a GUI.
class Loop
{
public:
    void post(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(fn));
    }

    std::size_t drain()
    {
        std::deque<std::function<void()>> local;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            local.swap(m_queue);
        }
        for (std::function<void()> &fn : local)
            fn();
        return local.size();
    }

    std::size_t queued()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

private:
    std::mutex                        m_mutex;
    std::deque<std::function<void()>> m_queue;
};

// Drains a Loop on a thread of its own, standing in for the GUI thread. A test that wants the
// opposite - a main thread that never gets around to the work - simply leaves its Loop without one.
class Pump
{
public:
    explicit Pump(Loop &loop) : m_loop(loop)
    {
        m_thread = std::thread([this] {
            m_id    = std::this_thread::get_id();
            m_ready = true;
            while (! m_stop)
                if (m_loop.drain() == 0)
                    std::this_thread::sleep_for(1ms);
            m_loop.drain();
        });
        while (! m_ready)
            std::this_thread::sleep_for(1ms);
    }

    ~Pump()
    {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
    }

    Pump(const Pump &)            = delete;
    Pump &operator=(const Pump &) = delete;

    std::thread::id id() const { return m_id; }

private:
    Loop             &m_loop;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_ready{false};
    std::thread::id   m_id;
    std::thread       m_thread;
};

MainThreadDispatcher::Poster poster(Loop &loop)
{
    return [&loop](std::function<void()> fn) { loop.post(std::move(fn)); };
}

} // namespace

TEST_CASE("work crosses to the main thread and its result comes back", "[MainThreadDispatcher]")
{
    Loop                 loop;
    Pump                 pump(loop);
    MainThreadDispatcher dispatcher(poster(loop), [] { return false; });

    int                               result = 0;
    std::thread::id                   ran_on;
    const MainThreadDispatcher::Status status =
        dispatcher.call([&result, &ran_on] { result = 42; ran_on = std::this_thread::get_id(); }, 10s);

    CHECK(status == MainThreadDispatcher::Status::Ok);
    CHECK(result == 42);
    CHECK(ran_on == pump.id());
    CHECK(ran_on != std::this_thread::get_id());
}

TEST_CASE("a call made from the main thread runs inline instead of deadlocking", "[MainThreadDispatcher]")
{
    // Posting and then waiting would block the very thread that drains the queue, so nothing would
    // ever run the work. There is deliberately no Pump here: were the call posted, it would hang.
    Loop                 loop;
    MainThreadDispatcher dispatcher(poster(loop), [] { return true; });

    bool                              ran = false;
    std::thread::id                   ran_on;
    const MainThreadDispatcher::Status status =
        dispatcher.call([&ran, &ran_on] { ran = true; ran_on = std::this_thread::get_id(); }, 10s);

    CHECK(status == MainThreadDispatcher::Status::Ok);
    CHECK(ran);
    CHECK(ran_on == std::this_thread::get_id());
    CHECK(loop.queued() == 0);
}

TEST_CASE("a call the main thread never reaches times out without running", "[MainThreadDispatcher]")
{
    Loop                 loop; // never drained, as though the GUI were stuck in a modal dialog
    MainThreadDispatcher dispatcher(poster(loop), [] { return false; });

    std::atomic<bool>                 ran{false};
    const auto                        started = std::chrono::steady_clock::now();
    const MainThreadDispatcher::Status status  = dispatcher.call([&ran] { ran = true; }, 100ms);
    const auto                        waited  = std::chrono::steady_clock::now() - started;

    CHECK(status == MainThreadDispatcher::Status::TimedOutNotRun);
    CHECK_FALSE(ran.load());
    CHECK(waited < 5s);

    // The caller has had its error by now and may well have retried, so the queued entry has to
    // stay unrun when the event loop finally catches up.
    loop.drain();
    CHECK_FALSE(ran.load());
}

TEST_CASE("a timeout while the work is already running reports an indeterminate outcome", "[MainThreadDispatcher]")
{
    Loop                 loop;
    Pump                 pump(loop);
    MainThreadDispatcher dispatcher(poster(loop), [] { return false; });

    std::atomic<bool>                 release{false};
    std::atomic<bool>                 finished{false};
    const MainThreadDispatcher::Status status = dispatcher.call(
        [&release, &finished] {
            for (int i = 0; i < 20000 && ! release; ++i)
                std::this_thread::sleep_for(1ms);
            finished = true;
        },
        500ms);
    release = true;

    CHECK(status == MainThreadDispatcher::Status::TimedOutMaybeRan);

    // Which is the whole reason this outcome is distinct from TimedOutNotRun: a main-thread call
    // that has begun cannot be cancelled, so the caller is told the outcome is unknown rather than
    // being told that nothing happened.
    for (int i = 0; i < 20000 && ! finished; ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(finished.load());
}

TEST_CASE("shutdown releases a call that is still waiting", "[MainThreadDispatcher]")
{
    Loop                 loop; // never drained
    MainThreadDispatcher dispatcher(poster(loop), [] { return false; });

    std::thread closer([&dispatcher] {
        std::this_thread::sleep_for(100ms);
        dispatcher.shutdown();
    });
    const auto                        started = std::chrono::steady_clock::now();
    const MainThreadDispatcher::Status status  = dispatcher.call([] {}, 60s);
    const auto                        waited  = std::chrono::steady_clock::now() - started;
    closer.join();

    CHECK(status == MainThreadDispatcher::Status::ShuttingDown);
    // Released by the shutdown rather than by its own timeout, which would have taken a minute.
    CHECK(waited < 30s);
}

TEST_CASE("a call made after shutdown is refused", "[MainThreadDispatcher]")
{
    Loop                 loop;
    MainThreadDispatcher dispatcher(poster(loop), [] { return false; });
    dispatcher.shutdown();

    std::atomic<bool>                 ran{false};
    const MainThreadDispatcher::Status status = dispatcher.call([&ran] { ran = true; }, 100ms);

    CHECK(status == MainThreadDispatcher::Status::ShuttingDown);
    CHECK(loop.queued() == 0);
    CHECK_FALSE(ran.load());
}

TEST_CASE("every concurrent caller gets its own result back", "[MainThreadDispatcher]")
{
    Loop                 loop;
    Pump                 pump(loop);
    MainThreadDispatcher dispatcher(poster(loop), [] { return false; });

    constexpr int            callers = 32;
    std::atomic<int>         succeeded{0};
    std::atomic<int>         ran{0};
    std::vector<std::thread> threads;
    threads.reserve(callers);
    for (int i = 0; i < callers; ++i)
        threads.emplace_back([&dispatcher, &succeeded, &ran] {
            // Deliberately no assertion in here: Catch2 macros are not thread-safe. The counts are
            // checked on the main thread once every caller has joined.
            if (dispatcher.call([&ran] { ++ran; }, 60s) == MainThreadDispatcher::Status::Ok)
                ++succeeded;
        });
    for (std::thread &thread : threads)
        thread.join();

    CHECK(succeeded.load() == callers);
    CHECK(ran.load() == callers);
}

TEST_CASE("destroying the dispatcher releases a call that is still waiting", "[MainThreadDispatcher]")
{
    Loop              loop; // never drained
    std::atomic<bool> released{false};
    std::thread       waiter;

    const auto started = std::chrono::steady_clock::now();
    {
        MainThreadDispatcher dispatcher(poster(loop), [] { return false; });
        waiter = std::thread([&dispatcher, &released] {
            dispatcher.call([] {}, 60s);
            released = true;
        });
        std::this_thread::sleep_for(100ms);
    } // ~MainThreadDispatcher, which shuts down
    waiter.join();
    const auto waited = std::chrono::steady_clock::now() - started;

    CHECK(released.load());
    CHECK(waited < 30s);
}

TEST_CASE("an exception thrown by the work reaches the caller", "[MainThreadDispatcher]")
{
    // Not a convenience. The work runs inside the event loop, which has no handler, so an
    // exception that escaped there would terminate the application rather than fail one call.
    Loop                 loop;
    Pump                 pump(loop);
    MainThreadDispatcher dispatcher(poster(loop), [] { return false; });

    CHECK_THROWS_AS(dispatcher.call([] { throw std::runtime_error("from the main thread"); }, 10s),
                    std::runtime_error);

    // The exception travelled rather than being swallowed: its message survives the trip.
    try {
        dispatcher.call([] { throw std::runtime_error("the original message"); }, 10s);
        FAIL("the exception did not reach the caller");
    } catch (const std::runtime_error &e) {
        CHECK(std::string(e.what()) == "the original message");
    }

    // And the dispatcher still works afterwards: a failed call is one failed call.
    int result = 0;
    CHECK(dispatcher.call([&result] { result = 7; }, 10s) == MainThreadDispatcher::Status::Ok);
    CHECK(result == 7);
}

TEST_CASE("an exception from a call made on the main thread reaches the caller too", "[MainThreadDispatcher]")
{
    Loop                 loop;
    MainThreadDispatcher dispatcher(poster(loop), [] { return true; });

    CHECK_THROWS_AS(dispatcher.call([] { throw std::runtime_error("inline"); }, 10s), std::runtime_error);
}

TEST_CASE("a call with nothing to run succeeds without reaching the event loop", "[MainThreadDispatcher]")
{
    Loop                 loop;
    MainThreadDispatcher dispatcher(poster(loop), [] { return false; });

    CHECK(dispatcher.call(nullptr, 100ms) == MainThreadDispatcher::Status::Ok);
    CHECK(loop.queued() == 0);
}
