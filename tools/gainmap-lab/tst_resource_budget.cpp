#include "async_decoder.h"
#include "gpu_retirement.h"
#include <QSemaphore>
#include <QSignalSpy>
#include <QThreadPool>
#include <QtTest>
#include <limits>
#include <thread>
#include <vector>

using namespace Licasa;
using namespace Licasa::GainmapLab;
namespace {
ExternalFileIdentity identity()
{
    QFile file(GAINMAP_FIXTURE);
    if (!file.open(QIODevice::ReadOnly)) qFatal("Missing fixture");
    return externalFileIdentity(file).value();
}
Limits smallLimits() { return Limits{16384, 1088}; }
constexpr quint64 decodeAllowance = 1088 * 64 + 16384 * 2;
struct Gate {
    QSemaphore release;
    std::atomic_bool entered{false};
};
}
class BudgetTests : public QObject {
    Q_OBJECT
private slots:
    void atomicAdmissionAndSplit()
    {
        auto pool = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{100, 80, 60});
        auto work = pool->acquire({100, 80, 0});
        QVERIFY(work);
        QVERIFY(!pool->acquire({1, 0, 1}));
        QCOMPARE(pool->used().upload, quint64(0)); // No partial debit.
        QVERIFY(!work->split({0, 81, 0}));
        auto output = work->split({0, 64, 0});
        QVERIFY(output);
        work->clear();
        QCOMPARE(pool->used().decode, quint64(0));
        QCOMPARE(pool->used().retained, quint64(64));
        auto alias = output;
        output.reset();
        QCOMPARE(pool->used().retained, quint64(64));
        alias.reset();
        QCOMPARE(pool->used().retained, quint64(0));
        auto gpu = pool->acquire({0, 0, 60});
        QVERIFY(gpu);
        QVERIFY(!pool->acquire({0, 0, std::numeric_limits<quint64>::max()}));
        gpu.reset();
        QCOMPARE(pool->used().upload, quint64(0));
    }
    void targetAndUploadShareGpuCapacity()
    {
        auto pool = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{1, 1, 100, 100}, 100);
        auto target = pool->acquire({0, 0, 0, 70});
        QVERIFY(target);
        QVERIFY(!pool->acquire({1, 1, 31, 0}));
        QCOMPARE(pool->used().decode, quint64(0));
        QVERIFY(!pool->acquire({0, 0, 20, 11}));
        auto upload = pool->acquire({0, 0, 30, 0});
        QVERIFY(upload);
        auto part = target->split({0, 0, 0, 20});
        QVERIFY(part);
        target.reset();
        QCOMPARE(pool->used().targets, quint64(20));
        QVERIFY(!part->split({0, 0, 0, 21}));
        auto both = pool->acquire({0, 0, 20, 30});
        QVERIFY(both);
        QVERIFY(!pool->acquire({0, 0, 0, 1}));
        part.reset(); both.reset(); upload.reset();
        QCOMPARE(pool->used().targets, quint64(0));
        QCOMPARE(pool->used().upload, quint64(0));
        auto huge = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{
            0, 0, std::numeric_limits<quint64>::max(), std::numeric_limits<quint64>::max()});
        QVERIFY(!huge->acquire({0, 0, std::numeric_limits<quint64>::max(), 1}));
        auto full = huge->acquire({0, 0, 0, std::numeric_limits<quint64>::max()});
        QVERIFY(full);
        QVERIFY(!huge->acquire({0, 0, 1, 0}));
    }
    void adapterIsolationAndPolicyDebt()
    {
        auto pool = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{100,100,100,100}, 150, 200, true);
        QVERIFY(!pool->acquire({0,0,1}));
        QVERIFY(!pool->acquire({0,0,1}, "unknown"));
        pool->setAdapterLimit("A", 60);
        pool->setAdapterLimit("B", 100);
        auto a = pool->acquire({0,0,0,60}, "A");
        QVERIFY(a);
        QVERIFY(!pool->acquire({0,0,1}, "A"));
        auto b = pool->acquire({0,0,80}, "B");
        QVERIFY(b);
        QVERIFY(!pool->acquire({0,0,0,11}, "B")); // Combined GPU cap.
        auto cpu = pool->acquire({60,0,0});
        QVERIFY(cpu);
        pool->setLimits({100,100,100,100}, 50, 100);
        QCOMPARE(pool->adapterUsed("A"), quint64(60));
        QCOMPARE(pool->used().upload, quint64(80));
        QVERIFY(!pool->acquire({1,0,0}));
        auto split = a->split({0,0,0,20});
        QVERIFY(split);
        a.reset(); cpu.reset();
        QCOMPARE(pool->adapterUsed("A"), quint64(20));
        pool->setAdapterLimit("B", 10);
        QVERIFY(!pool->acquire({0,0,1}, "B"));
        b.reset(); split.reset();
        pool->setLimits({100,100,100,100}, 150, 200);
        QVERIFY(pool->acquire({0,0,10}, "B"));
        QVERIFY(!pool->acquire({0,0,11}, "B"));
        QCOMPARE(pool->adapterUsed("A"), quint64(0));
        QCOMPARE(pool->adapterUsed("B"), quint64(0));
        auto probe = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{0,0,20,20});
        auto pending = probe->acquire({0,0,20});
        QVERIFY(pending);
        probe->setAdapterLimit("tiny", 10);
        QVERIFY(!pending->bindAdapter("tiny"));
        probe->setAdapterLimit("ready", 20);
        QVERIFY(pending->bindAdapter("ready"));
        QVERIFY(pending->bindAdapter("ready"));
        QVERIFY(!pending->bindAdapter("tiny"));
        pending.reset();
        QCOMPARE(probe->adapterUsed("ready"), quint64(0));
    }
    void concurrentAdmission()
    {
        auto pool = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{1, 1, 1, 1}, 1, 3, true);
        pool->setAdapterLimit("A", 1);
        pool->setAdapterLimit("B", 1);
        std::atomic_int inside{0}, violations{0}, admitted{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) threads.emplace_back([&] {
            for (int j = 0; j < 1000; ++j) {
                auto lease = pool->acquire(j % 2 ? ResourceBudget::Bytes{1, 1, 1, 0} : ResourceBudget::Bytes{1, 1, 0, 1}, j % 2 ? "A" : "B");
                if (!lease) continue;
                if (++inside != 1) ++violations;
                ++admitted;
                std::this_thread::yield();
                --inside;
            }
        });
        std::thread policy([&] {
            for (int i = 0; i < 1000; ++i) {
                pool->setLimits({1,1,1,1}, i % 2, 2 + i % 2);
                pool->setAdapterLimit("A", i % 2);
            }
        });
        for (auto& thread : threads) thread.join();
        policy.join();
        QVERIFY(admitted > 0);
        QCOMPARE(violations.load(), 0);
        QCOMPARE(pool->used().decode, quint64(0));
        QCOMPARE(pool->used().retained, quint64(0));
        QCOMPARE(pool->used().upload, quint64(0));
    }
    void closeAndRecreateWhileWorkerDrains()
    {
        auto pool = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{decodeAllowance, 8704, 8192});
        auto gate = std::make_shared<Gate>();
        auto* first = new AsyncDecoder({}, [gate](const RequestCoordinator::Request& request, QString& error) {
            auto result = decode(request.path, request.identity, request.headroom, error,
                                 request.limits, request.cancelled.get());
            gate->entered = true;
            if (!gate->release.tryAcquire(1, 5000)) return std::optional<Result>();
            return result;
        }, nullptr, pool);
        first->submit(GAINMAP_FIXTURE, identity(), 4, smallLimits());
        QTRY_VERIFY(gate->entered.load());
        delete first;
        QCOMPARE(pool->used().decode, decodeAllowance);
        QCOMPARE(pool->used().retained, quint64(8704));
        AsyncDecoder next({}, {}, nullptr, pool);
        QSignalSpy settled(&next, &AsyncDecoder::settled);
        next.submit(GAINMAP_FIXTURE, identity(), 2, smallLimits());
        QTRY_COMPARE(settled.count(), 1);
        QVERIFY(!settled[0][1].toBool());
        gate->release.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QTRY_COMPARE(pool->used().decode, quint64(0));
        QCOMPARE(pool->used().retained, quint64(0));
        next.submit(GAINMAP_FIXTURE, identity(), 2, smallLimits());
        QTRY_COMPARE(settled.count(), 2);
        QVERIFY(settled[1][1].toBool());
        QCOMPARE(pool->used().retained, quint64(8192));
        next.cancel();
        QCOMPARE(pool->used().retained, quint64(0));
    }
    void persistentResourceAndTransfer()
    {
        auto pool = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{decodeAllowance, 17408, 8192});
        std::shared_ptr<RequestCoordinator::Upload> pending;
        {
            AsyncDecoder first({}, {}, nullptr, pool);
            QSignalSpy settled(&first, &AsyncDecoder::settled);
            first.submit(GAINMAP_FIXTURE, identity(), 4, smallLimits());
            QTRY_COMPARE(settled.count(), 1);
            QVERIFY(settled[0][1].toBool());
            QVERIFY(first.withUpload([&](QImage&) { pending = first.uploadLease(); return true; }));
            first.cancel();
            QCOMPARE(pool->used().upload, quint64(8192));
            QCOMPARE(pool->used().retained, quint64(8192));
        }
        struct Texture {
            std::shared_ptr<ResourceBudget> pool;
            bool* releasedWhileCharged;
            ~Texture() { *releasedWhileCharged = pool->used().upload == 8192; }
        };
        bool correctOrder = false;
        LeasedResource<Texture> texture(pending, std::unique_ptr<Texture>(new Texture{pool, &correctOrder}));
        pending.reset();
        AsyncDecoder second({}, {}, nullptr, pool);
        QSignalSpy settled(&second, &AsyncDecoder::settled);
        second.submit(GAINMAP_FIXTURE, identity(), 1, smallLimits());
        QTRY_COMPARE(settled.count(), 1);
        QVERIFY(settled[0][1].toBool());
        QVERIFY(!second.withUpload([](QImage&) { return true; }));
        texture.reset();
        QVERIFY(correctOrder);
        QCOMPARE(pool->used().upload, quint64(0));
        QCOMPARE(pool->used().retained, quint64(8192));
        QVERIFY(second.withUpload([](QImage&) { return true; }));
        second.cancel();
        QCOMPARE(pool->used().retained, quint64(0));
        QCOMPARE(pool->used().upload, quint64(0));
    }
    void delayedAndLastUseRetirement()
    {
        struct Fence : GpuFence {
            Status status = Status::Pending;
            Status poll() noexcept override { return status; }
            bool wait() noexcept override { status = Status::Complete; return true; }
        };
        auto pool = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{0, 1, 1, 1}, 2, 3, true);
        pool->setAdapterLimit("A", 1); pool->setAdapterLimit("B", 1);
        auto upload = std::make_shared<Fence>();
        auto lastDraw = std::make_shared<Fence>();
        GpuRetirement queue;
        auto source = pool->acquire({0, 1, 0});
        auto texture = pool->acquire({0, 0, 1}, "A");
        auto target = pool->acquire({0, 0, 0, 1}, "B");
        QVERIFY(queue.retire(source, upload));
        QVERIFY(queue.retire(texture, lastDraw));
        QVERIFY(queue.retire(target, lastDraw));
        pool->setAdapterLimit("A", 0); pool->setAdapterLimit("B", 0);
        target.reset();
        source.reset(); texture.reset(); // Owners closed before GPU completion.
        QVERIFY(queue.collect());
        QCOMPARE(queue.pending(), size_t(3));
        QCOMPARE(pool->used().retained, quint64(1));
        QCOMPARE(pool->used().upload, quint64(1));
        upload->status = GpuFence::Status::Complete;
        QVERIFY(queue.collect());
        QCOMPARE(pool->used().retained, quint64(0));
        QCOMPARE(pool->used().upload, quint64(1));
        QCOMPARE(pool->adapterUsed("A"), quint64(1));
        QCOMPARE(pool->adapterUsed("B"), quint64(1));
        QVERIFY(!pool->acquire({0, 0, 1}, "A"));
        lastDraw->status = GpuFence::Status::Complete;
        QVERIFY(queue.collect());
        QCOMPARE(queue.pending(), size_t(0));
        QCOMPARE(pool->used().upload, quint64(0));
        QCOMPARE(pool->adapterUsed("A"), quint64(0));
        QCOMPARE(pool->adapterUsed("B"), quint64(0));
        pool->setAdapterLimit("A", 1);
        QVERIFY(pool->acquire({0, 0, 1}, "A"));
    }
    void retirementBackpressureAndFailure()
    {
        struct Fence : GpuFence {
            Status status = Status::Failed;
            bool waited = false;
            Status poll() noexcept override { return status; }
            bool wait() noexcept override { waited = true; return status == Status::Complete; }
        };
        auto pool = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{0, 0, 1});
        auto lease = pool->acquire({0, 0, 1});
        auto fence = std::make_shared<Fence>();
        GpuRetirement queue(1);
        QVERIFY(queue.retire(lease, fence));
        QVERIFY(!queue.retire(lease, fence)); // Caller still owns rejected entry.
        lease.reset();
        QVERIFY(!queue.collect());
        QVERIFY(!queue.shutdown());
        QVERIFY(fence->waited);
        QCOMPARE(pool->used().upload, quint64(1));
        fence->status = GpuFence::Status::Complete;
        QVERIFY(queue.shutdown());
        QCOMPARE(pool->used().upload, quint64(0));
    }
    void outOfOrderRetirement()
    {
        struct Fence : GpuFence {
            Status status = Status::Pending;
            Status poll() noexcept override { return status; }
            bool wait() noexcept override { return true; }
        };
        auto pool = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{0, 1, 1});
        auto first = std::make_shared<Fence>();
        auto second = std::make_shared<Fence>();
        GpuRetirement queue;
        auto a = pool->acquire({0, 1, 0});
        auto b = pool->acquire({0, 0, 1});
        QVERIFY(queue.retire(a, first));
        QVERIFY(queue.retire(b, second));
        a.reset(); b.reset();
        second->status = GpuFence::Status::Complete;
        QVERIFY(queue.collect());
        QCOMPARE(queue.pending(), size_t(1));
        QCOMPARE(pool->used().retained, quint64(1));
        QCOMPARE(pool->used().upload, quint64(0));
        first->status = GpuFence::Status::Complete;
        QVERIFY(queue.collect());
        QVERIFY(queue.collect()); // Duplicate polling cannot double-release.
        QCOMPARE(queue.pending(), size_t(0));
        QCOMPARE(pool->used().retained, quint64(0));
    }
    void retirementShutdownWaits()
    {
        struct Fence : GpuFence {
            bool* waited;
            explicit Fence(bool* value) : waited(value) {}
            Status poll() noexcept override { return Status::Pending; }
            bool wait() noexcept override { *waited = true; return true; }
        };
        bool waited = false;
        auto pool = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{0, 0, 1});
        {
            GpuRetirement queue;
            auto lease = pool->acquire({0, 0, 1});
            QVERIFY(queue.retire(lease, std::make_shared<Fence>(&waited)));
        }
        QVERIFY(waited);
        QCOMPARE(pool->used().upload, quint64(0));
    }
    void poolLivesWithLease()
    {
        auto pool = std::make_shared<ResourceBudget>(ResourceBudget::Bytes{1, 1, 1});
        std::weak_ptr<ResourceBudget> weak = pool;
        auto lease = pool->acquire({1, 0, 0});
        pool.reset();
        QVERIFY(!weak.expired());
        lease.reset();
        QVERIFY(weak.expired());
    }
};
QTEST_GUILESS_MAIN(BudgetTests)
#include "tst_resource_budget.moc"
