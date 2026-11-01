#pragma once
//
// PrefetchPool - a few preallocated worker threads that decode the images
// around the displayed one ahead of time, and can abandon a decode the user
// no longer needs. Work is handed over by message passing: update() replaces
// the wanted list (priority order); queued jobs that fell out of it are
// dropped, running ones get their cancel flag raised (the plugin polls it,
// see wiv_plugin_load_page_cancellable), and new ones are queued. Finished
// decodes come back as a queued imageReady() signal on the owner's thread.
// A job is either a full decode or a quick low-resolution preview (see
// ImagePluginHost::loadPreview()); previews are what make holding the arrow
// key fast, because showing one costs the GUI thread almost nothing.
//
#include "ImagePluginHost.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class PrefetchPool : public QObject
{
    Q_OBJECT
public:
    explicit PrefetchPool(int threadCount, QObject* parent = nullptr);
    ~PrefetchPool() override; // cancels running jobs and joins the workers

    enum class Kind { Full, Preview };
    struct Want
    {
        std::string path;
        Kind kind;
        bool operator==(const Want& o) const { return path == o.path && kind == o.kind; }
    };

    // `wanted`: decodes to have, most urgent first. Anything queued or
    // running that is not in it is cancelled. With enqueueNew == false only
    // the cancelling happens (used while the user is still skipping through
    // files, so nothing starts decoding for an image that is about to be
    // skipped).
    void update(const std::vector<Want>& wanted, bool enqueueNew);

    // True if `path` is queued or has a live (not cancelled) job running.
    bool isPending(const Want& want) const;

signals:
    // `image` is null when a preview was requested but the file has none
    // (small image, or its plugin cannot decode at reduced size).
    void imageReady(const QString& path, bool preview, std::shared_ptr<giv::LoadedImage> image);

private:
    struct Job
    {
        Want want;
        std::shared_ptr<std::atomic<int>> cancel = std::make_shared<std::atomic<int>>(0);
    };

    void workerLoop();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::vector<Job> running_;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};
