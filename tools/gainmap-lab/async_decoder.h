#pragma once

#include "request_coordinator.h"
#include <QFutureWatcher>
#include <QObject>
#include <functional>

namespace Licasa::GainmapLab {
// Offline owner-thread adapter. At most one worker and one latest intent.
// Destroying it cancels work; workers capture no owner pointers and can drain.
class AsyncDecoder : public QObject {
    Q_OBJECT
public:
    using Decode = std::function<std::optional<Result>(const RequestCoordinator::Request&, QString&)>;
    // A supplied pool is authoritative; otherwise budgets creates a private pool.
    explicit AsyncDecoder(RequestCoordinator::Budgets budgets = {}, Decode decode = {},
                          QObject* parent = nullptr, std::shared_ptr<ResourceBudget> shared = {});
    ~AsyncDecoder() override;
    quint64 submit(QString path, ExternalFileIdentity identity, float headroom, Limits limits = {});
    void cancel();
    bool busy() const { return watcher_ != nullptr; }
    const RequestCoordinator& coordinator() const { return coordinator_; }
    QSize gainSize() const { return gainSize_; }
    qsizetype jpegBytes() const { return jpegBytes_; }
    // Retain uploadLease() with queued transfers or persistent resources; without
    // that lease the callback must finish all work before returning. No reentrant
    // owner operations or image mutation after the callback returns.
    std::shared_ptr<RequestCoordinator::Upload> uploadLease() const { return coordinator_.uploadLease(); }
    bool withUpload(const std::function<bool(QImage&)>& consume, const ResourceBudget::Adapter& adapter = {});
signals:
    void settled(quint64 serial, bool accepted, QString error, DecodeFailure failure);
private:
    struct Intent {
        quint64 serial;
        QString path;
        ExternalFileIdentity identity;
        float headroom;
        Limits limits;
    };
    struct Outcome {
        ResourceBudget::Lease reservation;
        std::optional<Result> result;
        QString error;
        DecodeFailure failure = DecodeFailure::Unavailable;
    };
    void startPending();
    void scheduleStart();
    RequestCoordinator coordinator_;
    Decode decode_;
    QFutureWatcher<std::shared_ptr<Outcome>>* watcher_ = nullptr;
    std::optional<Intent> pending_;
    quint64 serial_ = 0;
    QSize gainSize_;
    qsizetype jpegBytes_ = 0;
    bool uploading_ = false;
    bool startQueued_ = false;
};
} // namespace Licasa::GainmapLab
