#pragma once
#include <QList>
#include <QMap>
#include <QMutex>
#include <QSet>
#include <QUrl>
#include <functional>
namespace keefi {
// Shared only by additional chart-study HTTP sources. Never delays the main
// visible candle source or substitutes a local pacing delay with an HTTP error.
class ChartStudyRequestPacer final {
 public:
 using Client=quint64;using Clock=std::function<qint64()>;
 explicit ChartStudyRequestPacer(Clock={});
 Client createClient();
 void cancel(Client); // Drops a waiting owner, preserves the origin deadline.
 void release(Client);
 bool tryAcquire(Client,const QUrl&);
 private:
 struct Gate {qint64 next=0;QList<Client> waiting;};
 Clock clock_;QMutex mutex_;Client nextClient_=1;QSet<Client> clients_;QMap<QString,Gate> gates_;
};
}
