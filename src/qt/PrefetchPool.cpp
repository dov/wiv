#include "PrefetchPool.h"

#include <QMetaObject>

#include <algorithm>
#include <unordered_set>

static_assert(sizeof(std::atomic<int>) == sizeof(int) && std::atomic<int>::is_always_lock_free,
              "cancel flags are handed to plugins as plain int pointers");

PrefetchPool::PrefetchPool(int threadCount, QObject* parent) : QObject(parent)
{
    for (int i = 0; i < std::max(1, threadCount); ++i)
        threads_.emplace_back([this]() { workerLoop(); });
}

PrefetchPool::~PrefetchPool()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        queue_.clear();
        for (Job& job : running_) job.cancel->store(1);
    }
    wake_.notify_all();
    for (auto& t : threads_) t.join();
}

void PrefetchPool::update(const std::vector<Want>& wanted, bool enqueueNew)
{
    const auto isWanted = [&](const Want& w) { return std::find(wanted.begin(), wanted.end(), w) != wanted.end(); };
    std::lock_guard<std::mutex> lock(mutex_);

    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Job& j) { return !isWanted(j.want); }),
                 queue_.end());
    for (Job& job : running_)
        if (!isWanted(job.want)) job.cancel->store(1);

    if (!enqueueNew) return;

    // Rebuild the queue in priority order, keeping jobs already queued and
    // skipping those with a live running job.
    std::deque<Job> ordered;
    for (const Want& want : wanted)
    {
        const bool running = std::any_of(running_.begin(), running_.end(),
                                         [&](const Job& j) { return j.want == want && !j.cancel->load(); });
        if (running) continue;
        auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Job& j) { return j.want == want; });
        if (it != queue_.end()) ordered.push_back(std::move(*it));
        else ordered.push_back(Job{want});
    }
    queue_ = std::move(ordered);
    wake_.notify_all();
}

bool PrefetchPool::isPending(const Want& want) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::any_of(queue_.begin(), queue_.end(), [&](const Job& j) { return j.want == want; })) return true;
    return std::any_of(running_.begin(), running_.end(),
                       [&](const Job& j) { return j.want == want && !j.cancel->load(); });
}

void PrefetchPool::workerLoop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
        wake_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
        if (stop_) return;

        Job job = std::move(queue_.front());
        queue_.pop_front();
        running_.push_back(job);
        lock.unlock();

        const bool preview = job.want.kind == Kind::Preview;
        // A preview decode is short and not interruptible; it is simply discarded if cancelled.
        auto image = preview ? giv::ImagePluginHost::loadPreview(job.want.path)
                             : giv::ImagePluginHost::load(job.want.path, 0, reinterpret_cast<const volatile int*>(job.cancel.get()));

        lock.lock();
        running_.erase(std::find_if(running_.begin(), running_.end(),
                                    [&](const Job& j) { return j.cancel == job.cancel; }));
        if (!job.cancel->load() && (image || preview))
        {
            std::shared_ptr<giv::LoadedImage> shared;
            if (image) shared = std::make_shared<giv::LoadedImage>(std::move(*image));
            QString path = QString::fromStdString(job.want.path);
            // Posted to the owner's thread; dropped if the pool is destroyed first.
            QMetaObject::invokeMethod(this, [this, path, preview, shared]() { emit imageReady(path, preview, shared); },
                                      Qt::QueuedConnection);
        }
    }
}
