#include "async_decoder.h"
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <QtConcurrentRun>

namespace Licasa::GainmapLab {
AsyncDecoder::AsyncDecoder(RequestCoordinator::Budgets budgets, Decode decode, QObject* parent,
                           std::shared_ptr<ResourceBudget> shared)
    : QObject(parent), coordinator_(shared ? std::move(shared) : std::make_shared<ResourceBudget>(
          ResourceBudget::Bytes{budgets.decode, budgets.retained, budgets.upload})), decode_(std::move(decode))
{}
AsyncDecoder::~AsyncDecoder() { coordinator_.cancel(); }

quint64 AsyncDecoder::submit(QString path, ExternalFileIdentity identity, float headroom, Limits limits)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (uploading_) return 0;
    cancel();
    pending_ = Intent{serial_, std::move(path), identity, headroom, limits};
    scheduleStart();
    return serial_;
}

void AsyncDecoder::cancel()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (uploading_) return;
    ++serial_;
    pending_.reset();
    coordinator_.cancel();
    gainSize_ = {};
    jpegBytes_ = 0;
}

void AsyncDecoder::scheduleStart()
{
    if (startQueued_) return;
    startQueued_ = true;
    QTimer::singleShot(0, this, [this] {
        startQueued_ = false;
        startPending();
    });
}

void AsyncDecoder::startPending()
{
    if (watcher_ || !pending_) return;
    const Intent intent = std::move(*pending_);
    pending_.reset();
    const auto request = coordinator_.request(intent.path, intent.identity, intent.headroom, intent.limits,
                                              !decode_);
    if (!request) {
        emit settled(intent.serial, false, QStringLiteral("Gain-map request rejected by identity or budget."),
                     DecodeFailure::Unavailable);
        return;
    }
    watcher_ = new QFutureWatcher<std::shared_ptr<Outcome>>(this);
    auto* watcher = watcher_;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, request, intent] {
        auto outcome = watcher->result();
        gainSize_ = outcome->result ? outcome->result->gainSize : QSize();
        jpegBytes_ = outcome->result ? outcome->result->jpegBytes : 0;
        const bool accepted = coordinator_.complete(*request, std::move(outcome->result));
        outcome->result.reset();
        watcher_ = nullptr;
        watcher->deleteLater();
        if (!accepted) {
            gainSize_ = {};
            jpegBytes_ = 0;
            if (outcome->error.isEmpty()) outcome->error = QStringLiteral("Gain-map result became stale or invalid.");
        }
        // Schedule before signaling; a receiver may destroy this adapter.
        scheduleStart();
        if (intent.serial == serial_) emit settled(intent.serial, accepted, outcome->error, outcome->failure);
    });
    const Decode work = decode_;
    watcher->setFuture(QtConcurrent::run([work, request] {
        auto outcome = std::make_shared<Outcome>();
        outcome->reservation = request->reservation;
        try {
            if (work) {
                outcome->result = work(*request, outcome->error);
            } else {
                outcome->result = GainmapLab::decode(request->path, request->identity, request->headroom,
                                                     outcome->error, request->limits,
                                                     request->cancelled.get(), &outcome->failure,
                                                     [&request](quint64 pixels) {
                                                         return request->reserveRaster(pixels);
                                                     });
            }
        } catch (...) {
            outcome->error = QStringLiteral("Gain-map worker failed.");
        }
        return outcome;
    }));
}

bool AsyncDecoder::withUpload(const std::function<bool(QImage&)>& consume, const ResourceBudget::Adapter& adapter)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (uploading_ || !coordinator_.beginUpload(adapter)) return false;
    uploading_ = true;
    const auto release = qScopeGuard([this] { coordinator_.endUpload(); uploading_ = false; });
    return consume(*coordinator_.uploadImage());
}
} // namespace Licasa::GainmapLab
