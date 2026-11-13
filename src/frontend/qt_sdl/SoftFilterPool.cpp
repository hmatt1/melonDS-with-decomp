/*
    Worker pool for the software screen filters: see SoftFilterPool.h.
*/

#include "SoftFilterPool.h"

SoftFilterPool::SoftFilterPool(int threads)
{
    for (int i = 1; i < threads; i++)
        workers.emplace_back([this] { loop(); });
}

SoftFilterPool::~SoftFilterPool()
{
    {
        std::lock_guard<std::mutex> l(mtx);
        quit = true;
    }
    wake.notify_all();
    for (auto& t : workers) t.join();
}

void SoftFilterPool::run(int jobs, const std::function<void(int)>& job)
{
    if (jobs <= 0) return;
    std::unique_lock<std::mutex> l(mtx);
    cur = &job;
    jobsTotal = jobs;
    nextJob = 0;
    jobsLeft = jobs;
    generation++;
    wake.notify_all();
    while (nextJob < jobsTotal)    // the caller takes jobs too
    {
        int j = nextJob++;
        l.unlock();
        job(j);
        l.lock();
        jobsLeft--;
    }
    done.wait(l, [this] { return jobsLeft == 0; });
    cur = nullptr;
}

void SoftFilterPool::loop()
{
    unsigned seen = 0;
    std::unique_lock<std::mutex> l(mtx);
    for (;;)
    {
        wake.wait(l, [&] { return quit || (generation != seen && cur); });
        if (quit) return;
        seen = generation;
        while (cur && nextJob < jobsTotal)
        {
            int j = nextJob++;
            auto* job = cur;
            l.unlock();
            (*job)(j);
            l.lock();
            if (--jobsLeft == 0) done.notify_all();
        }
    }
}
