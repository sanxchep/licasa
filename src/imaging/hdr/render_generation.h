#pragma once
#include <QtGlobal>
#include <atomic>
namespace Licasa {
// Tokens survive queued callbacks; addresses/UUIDs alone cannot distinguish a
// rebuilt device from its predecessor (native handles can be reused).
class HdrRenderGeneration {
public:
    quint64 current() const { return value_.load(); }
    quint64 advance() { return ++value_; }
    bool matches(quint64 token) const { return token == current(); }
private:
    std::atomic<quint64> value_{1};
};
}
