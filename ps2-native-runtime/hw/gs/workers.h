#pragma once

// Worker threads the rasteriser shares a primitive's rows out to (a big primitive's rows are drawn at once on several cores: a
// primitive writes each of its pixels once, so its rows don't depend on each other)

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Gs
{
class Workers
{
public:
    Workers();
    ~Workers();

    // Calls work(first, end) on parts of [begin, end) on every worker and this thread, and returns once all are done
    void ForRows(int begin, int end, const std::function<void(int, int)>& work);
    unsigned count() const { return static_cast<unsigned>(threads_.size()) + 1; }

private:
    void Run(unsigned index);

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable start_;
    std::condition_variable done_;
    const std::function<void(int, int)>* work_ = nullptr;
    int begin_ = 0;
    int end_ = 0;
    unsigned job_ = 0;
    unsigned pending_ = 0;
    // The next rows taken (a few at a time, by whichever thread is free)
    std::atomic<int> next_{0};
    int chunk_ = 1;
    void TakeRows(const std::function<void(int, int)>& work, int end);
    bool stopping_ = false;
};

Workers& GetWorkers();
}
