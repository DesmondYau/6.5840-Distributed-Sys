// ThreadPool.hpp
#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

class ThreadPool {
public:
    explicit ThreadPool(size_t threads) 
        : stop(false) 
    {
        for(size_t i = 0; i < threads; ++i) 
        {
            workers.emplace_back([this] {
                while(true) 
                {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{ 
                            return this->stop || !this->tasks.empty(); 
                        });

                        if(this->stop && this->tasks.empty()) 
                            return;
                            
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    template<class F>
    void enqueue(F&& f) 
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (stop) return; // Already stopped
            stop = true;
        }

        condition.notify_all();
        
        for (std::thread &worker : workers) 
        {
            if (worker.joinable()) worker.join();
        }
    }

    ~ThreadPool() 
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for(std::thread &worker : workers) {
            if(worker.joinable()) worker.join();
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};