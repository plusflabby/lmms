#include "RecordingRing.h"
#include <algorithm>
#include <limits>
#include <new>
namespace lmms::recording {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"recording counters must be lock-free on the target host");
bool RecordingRing::checkedCounterAdvance(std::uint64_t current,std::uint64_t delta,std::uint64_t& next) noexcept
{
 if(current>std::numeric_limits<std::uint64_t>::max()-delta)return false;
 next=current+delta;return true;
}
RecordingRing::RecordingRing(std::uint32_t rate,std::uint32_t capacity,std::uint64_t limit)
 :m_sampleRate(rate),m_capacity(capacity),m_maxTakeFrames(limit?limit:(rate==44100||rate==48000?std::uint64_t(rate)*600:0))
{
 const auto minimum=rate==44100?2ULL*44100+2ULL*MaximumCallback:rate==48000?2ULL*48000+2ULL*MaximumCallback:std::uint64_t(-1);
 if(!minimum || m_maxTakeFrames==0 || m_maxTakeFrames>std::uint64_t(rate)*600 || capacity<minimum || capacity>Capacity || (capacity&(capacity-1)))return;
 m_frames.reset(new(std::nothrow) float[capacity]);
}
RecordingRing::~RecordingRing()=default;
PushResult RecordingRing::push(const float* input,std::size_t frames,std::uint32_t channels,std::uint32_t channel) noexcept
{
 PushResult result;
 if(!m_frames || !input || frames==0 || frames>MaximumCallback || (channels!=1 && channels!=2) || channel>=channels)return result;
 result.valid=true;
 const auto accepted=m_totalAccepted.load(std::memory_order_relaxed);
 std::uint64_t requestedEnd=0;
 if(!checkedCounterAdvance(accepted,frames,requestedEnd)){result.valid=false;result.counterOverflow=true;m_counterOverflow.store(true,std::memory_order_release);return result;}
 const auto head=m_write.load(std::memory_order_relaxed),tail=m_read.load(std::memory_order_acquire);
 const auto occupied=head-tail;
 if(occupied>m_capacity){result.valid=false;result.counterOverflow=true;m_counterOverflow.store(true,std::memory_order_release);return result;}
 const auto belowLimit=accepted<m_maxTakeFrames?m_maxTakeFrames-accepted:0;
 const auto count=std::uint32_t(std::min<std::uint64_t>({frames,m_capacity-occupied,belowLimit}));
 std::uint64_t nextWrite=0,nextAccepted=0;
 if(!checkedCounterAdvance(head,count,nextWrite) || !checkedCounterAdvance(accepted,count,nextAccepted))
 {result.valid=false;result.counterOverflow=true;m_counterOverflow.store(true,std::memory_order_release);return result;}
 const auto channelCount=std::uint64_t(channels),channelIndex=std::uint64_t(channel);
 for(std::uint32_t i=0;i<count;++i)m_frames[(head+i)&(m_capacity-1)]=input[std::size_t(std::uint64_t(i)*channelCount+channelIndex)];
 if(count){m_write.store(nextWrite,std::memory_order_release);m_totalAccepted.store(nextAccepted,std::memory_order_release);}
 result.acceptedFrames=count;
 const auto remaining=std::size_t(frames)-count;
 // Samples excluded solely by the configured take boundary are an intentional stop,
 // not a buffer loss. Actual queue exhaustion is counted exactly and never padded.
 result.limitReached=(nextAccepted>=m_maxTakeFrames);
 if(remaining && !result.limitReached){
  const auto old=m_dropped.load(std::memory_order_relaxed);
  std::uint64_t nextDropped=0;
  if(!checkedCounterAdvance(old,remaining,nextDropped)){m_dropped.store(std::numeric_limits<std::uint64_t>::max(),std::memory_order_release);m_counterOverflow.store(true,std::memory_order_release);result.counterOverflow=true;}
  else m_dropped.store(nextDropped,std::memory_order_release);
  result.droppedFrames=std::uint32_t(remaining);
 }
 return result;
}
std::size_t RecordingRing::pop(float* destination,std::size_t capacity) noexcept
{
 if(!m_frames || !destination || !capacity)return 0;
 const auto tail=m_read.load(std::memory_order_relaxed),head=m_write.load(std::memory_order_acquire);
 const auto available=head-tail;const auto count=std::min<std::size_t>({capacity,available,MaximumCallback});
 for(std::size_t i=0;i<count;++i)destination[i]=m_frames[(tail+i)&(m_capacity-1)];
 std::uint64_t next=0;if(count && checkedCounterAdvance(tail,count,next))m_read.store(next,std::memory_order_release);
 else if(count)m_counterOverflow.store(true,std::memory_order_release);
 return count;
}
bool RecordingRing::resetAfterQuiescence() noexcept
{
 if(!m_frames || m_read.load(std::memory_order_acquire)!=m_write.load(std::memory_order_acquire))return false;
 m_read.store(0,std::memory_order_relaxed);m_write.store(0,std::memory_order_relaxed);
 m_totalAccepted.store(0,std::memory_order_relaxed);m_dropped.store(0,std::memory_order_relaxed);
 m_counterOverflow.store(false,std::memory_order_relaxed);return true;
}
}
