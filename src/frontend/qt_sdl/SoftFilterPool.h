/*
    A small pool of worker threads for the software screen filters (SoftLcdFilter, SoftCrtFilter).
    It runs one batch of jobs at a time; the calling thread takes jobs too.
*/

#ifndef SOFTFILTERPOOL_H
#define SOFTFILTERPOOL_H

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class SoftFilterPool
{
public:
    explicit SoftFilterPool(int threads);
    ~SoftFilterPool();
    int size() const { return (int)workers.size() + 1; }  // the calling thread works too
    void run(int jobs, const std::function<void(int)>& job);

private:
    void loop();
    std::vector<std::thread> workers;
    std::mutex mtx;
    std::condition_variable wake, done;
    const std::function<void(int)>* cur = nullptr;
    int jobsTotal = 0, nextJob = 0, jobsLeft = 0;
    unsigned generation = 0;
    bool quit = false;
};

#endif
