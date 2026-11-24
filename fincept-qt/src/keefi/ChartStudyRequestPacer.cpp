#include "keefi/ChartStudyRequestPacer.h"
#include <QMutexLocker>
#include <chrono>
#include <limits>
namespace keefi {
ChartStudyRequestPacer::ChartStudyRequestPacer(Clock clock):clock_(std::move(clock)){
 if(!clock_)clock_=[] {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();};
}
ChartStudyRequestPacer::Client ChartStudyRequestPacer::createClient(){QMutexLocker lock(&mutex_);const auto client=nextClient_++;clients_.insert(client);return client;}
void ChartStudyRequestPacer::cancel(Client client){QMutexLocker lock(&mutex_);for(auto& gate:gates_)gate.waiting.removeAll(client);}
void ChartStudyRequestPacer::release(Client client){QMutexLocker lock(&mutex_);clients_.remove(client);for(auto& gate:gates_)gate.waiting.removeAll(client);}
bool ChartStudyRequestPacer::tryAcquire(Client client,const QUrl& url){
 QMutexLocker lock(&mutex_);
 if(!clients_.contains(client)||url.scheme()!="https"||url.port(443)!=443||!url.userInfo().isEmpty())return false;
 const auto host=url.host();QString origin;
 if(host=="api.binance.com"||host=="fapi.binance.com")origin="binance-live";
 else if(host=="api.upbit.com")origin="upbit";else return false;
 const auto now=clock_();
 // A source has one selected next request. Switching origin must release its
 // obsolete place in another origin's queue so another source cannot starve.
 for(auto it=gates_.begin();it!=gates_.end();){
  if(it.key()!=origin)it->waiting.removeAll(client);
  if(it->waiting.isEmpty()&&it->next<=now)it=gates_.erase(it);else ++it;
 }
 auto& gate=gates_[origin];if(!gate.waiting.contains(client))gate.waiting.append(client);
 if(now<gate.next||gate.waiting.first()!=client)return false;
 gate.waiting.removeFirst();gate.next=now>std::numeric_limits<qint64>::max()-1000?std::numeric_limits<qint64>::max():now+1000;
 return true;
}
}
