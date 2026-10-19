#include "jobs.h"
#include "platform.h"

#include <deque>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace jobs {

struct Job {
    std::function<void()> work;
    std::function<void()> done;
};

static std::deque<Job> g_queue;
static std::vector<std::function<void()>> g_main;
static std::mutex g_mtx;
static std::condition_variable g_cv;

static void worker()
{
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return !g_queue.empty(); });
            j = std::move(g_queue.front());
            g_queue.pop_front();
        }
        if (j.work) j.work();
        if (j.done) post(std::move(j.done));
    }
}

void init(int threads)
{
    for (int i = 0; i < threads; i++) platform::startThread(worker, 512 * 1024);
}

void run(std::function<void()> work, std::function<void()> done)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_queue.push_back({std::move(work), std::move(done)});
    }
    g_cv.notify_one();
}

void post(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_main.push_back(std::move(fn));
}

int pump()
{
    std::vector<std::function<void()>> fns;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        fns.swap(g_main);
    }
    for (auto& f : fns) f();
    return (int)fns.size();
}

} // namespace jobs
