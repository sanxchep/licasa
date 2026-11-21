#pragma once

#include "io/external_file_identity.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <algorithm>
#include <atomic>
#include <optional>
#include <vector>

namespace Licasa {

// The opt-in HDR bridge derives a second rendition of a still that the SDR
// provider has already delivered. Both renditions must come from the same
// file, including after a same-path replacement that the window has not
// reloaded. The provider therefore records the identity it validated for each
// recent still response, keyed by the exact image request.
//
// Recording is off unless a lab entry point enables it, so ordinary builds do
// not retain request strings or take this lock while decoding.
class PresentedStillSources {
  public:
    struct Entry {
        QString path;
        ExternalFileIdentity identity;
    };

    static PresentedStillSources& instance()
    {
        static PresentedStillSources shared;
        return shared;
    }

    void setEnabled(bool enabled)
    {
        enabled_.store(enabled, std::memory_order_relaxed);
        if (!enabled) {
            QMutexLocker lock(&mutex_);
            entries_.clear();
        }
    }

    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    void record(const QString& requestId, const QString& path,
                const std::optional<ExternalFileIdentity>& identity)
    {
        if (!enabled() || requestId.isEmpty() || !identity) {
            return;
        }
        QMutexLocker lock(&mutex_);
        entries_.insert(requestId, Node{{path, *identity}, ++tick_});
        if (entries_.size() > capacity) {
            // Drop the least recently used quarter in one pass. A consumer
            // that outlives its entry keeps SDR until the still is reopened.
            std::vector<quint64> ticks;
            ticks.reserve(size_t(entries_.size()));
            for (const auto& node : std::as_const(entries_)) {
                ticks.push_back(node.tick);
            }
            const auto cut = ticks.begin() + ticks.size() / 4;
            std::nth_element(ticks.begin(), cut, ticks.end());
            const quint64 oldestKept = *cut;
            for (auto node = entries_.begin(); node != entries_.end();) {
                node = node->tick < oldestKept ? entries_.erase(node) : std::next(node);
            }
        }
    }

    std::optional<Entry> find(const QString& requestId)
    {
        QMutexLocker lock(&mutex_);
        const auto found = entries_.find(requestId);
        if (found == entries_.end()) {
            return std::nullopt;
        }
        found->tick = ++tick_;
        return found->entry;
    }

    static constexpr qsizetype capacity = 1024;

  private:
    struct Node {
        Entry entry;
        quint64 tick = 0;
    };

    std::atomic_bool enabled_{false};
    QMutex mutex_;
    QHash<QString, Node> entries_;
    quint64 tick_ = 0;
};

} // namespace Licasa
