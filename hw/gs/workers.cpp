#include "workers.h"

#include <algorithm>
#include <cstdlib>

namespace Gs
{
Workers::Workers()
{
    unsigned cores = std::thread::hardware_concurrency();
    if (const char* setting = std::getenv("TWIN_GS_THREADS"))
    {
        cores = static_cast<unsigned>(std::atoi(setting));
    }

    // Leave a core to the game's own thread
    unsigned workers = cores > 2 ? std::min(cores - 2, 7u) : 0;
    for (unsigned i = 0; i < workers; i++)
    {
        threads_.emplace_back([this, i] { Run(i + 1); });
    }
}

Workers::~Workers()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }

    start_.notify_all();
    for (std::thread& thread : threads_)
    {
        thread.join();
    }
}

void Workers::TakeRows(const std::function<void(int, int)>& work, int end)
{
    for (;;)
    {
        int first = next_.fetch_add(chunk_, std::memory_order_relaxed);
        if (first >= end)
        {
            return;
        }

        work(first, std::min(first + chunk_, end));
    }
}

void Workers::Run(unsigned)
{
    unsigned seen = 0;
    for (;;)
    {
        const std::function<void(int, int)>* work;
        int end;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            start_.wait(lock, [&] { return stopping_ || job_ != seen; });
            if (stopping_)
            {
                return;
            }

            seen = job_;
            work = work_;
            end = end_;
        }

        TakeRows(*work, end);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (--pending_ == 0)
            {
                done_.notify_one();
            }
        }
    }
}

void Workers::ForRows(int begin, int end, const std::function<void(int, int)>& work)
{
    if (threads_.empty() || end - begin < 2)
    {
        work(begin, end);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        work_ = &work;
        begin_ = begin;
        end_ = end;
        // About four pieces a thread
        chunk_ = std::max(1, (end - begin) / static_cast<int>(count() * 4));
        next_.store(begin, std::memory_order_relaxed);
        pending_ = static_cast<unsigned>(threads_.size());
        job_++;
    }

    start_.notify_all();
    TakeRows(work, end);
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&] { return pending_ == 0; });
}

Workers& GetWorkers()
{
    static Workers workers;
    return workers;
}
}
