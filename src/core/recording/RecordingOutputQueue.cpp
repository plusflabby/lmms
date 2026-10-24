#include "RecordingOutputQueue.h"
#include <algorithm>
#include <utility>
#include <limits>
namespace lmms::recording {
ProducerReservation::~ProducerReservation(){if(m_owner)m_owner->cancel(*this);}
ProducerReservation::ProducerReservation(ProducerReservation&& other) noexcept { *this=std::move(other); }
ProducerReservation& ProducerReservation::operator=(ProducerReservation&& other) noexcept
{
 if(this!=&other){if(m_owner)m_owner->cancel(*this);m_owner=std::exchange(other.m_owner,nullptr);m_first=other.m_first;m_descriptor=other.m_descriptor;m_frames=other.m_frames;}
 return *this;
}
void RecordingOutputQueue::cancel(ProducerReservation& r) noexcept {r.m_owner=nullptr;m_reserved=false;}
ProducerReservation RecordingOutputQueue::reservePeriod(RenderEpoch epoch,std::uint32_t frames) noexcept
{
 if(epoch!=m_epoch || !epoch.generation || !epoch.revision || m_reserved || !frames || frames>NativePeriodLimit)return {};
 const auto read=m_frameRead.load(std::memory_order_acquire);
 const auto descriptor=m_descriptorWrite.load(std::memory_order_relaxed);
 const auto consumed=m_descriptorRead.load(std::memory_order_acquire);
 if(m_frameWrite>std::numeric_limits<std::uint64_t>::max()-frames || descriptor==std::numeric_limits<std::uint64_t>::max()
  || m_frameWrite-read>OutputCapacity-frames || descriptor-consumed>=OutputSpans)return {};
 ProducerReservation r;r.m_owner=this;r.m_first=m_frameWrite;r.m_descriptor=descriptor;r.m_frames=frames;m_reserved=true;
 return r;
}
bool RecordingOutputQueue::publish(ProducerReservation r,std::span<const SampleFrame> pcm,RenderStamp stamp) noexcept
{
 if(r.m_owner!=this || !m_reserved || r.m_first!=m_frameWrite || pcm.size()!=r.m_frames || stamp.epoch!=m_epoch
  || stamp.frames!=r.m_frames || stamp.renderFirstFrame<0
  || stamp.renderFirstFrame>std::numeric_limits<std::int64_t>::max()-stamp.frames
  || (stamp.hasTarget && (!stamp.musicalValid || stamp.targetOffset>=stamp.frames || m_targetPublished)))return false;
 for(std::uint32_t i=0;i<r.m_frames;++i)m_pcm[(m_frameWrite+i)%OutputCapacity]=pcm[i];
 m_stamps[r.m_descriptor%OutputSpans]=stamp;
 m_frameWrite+=r.m_frames;
 m_targetPublished=m_targetPublished || stamp.hasTarget;
 m_descriptorWrite.store(r.m_descriptor+1,std::memory_order_release);
 cancel(r);
 return true;
}
void RecordingOutputQueue::consume(float* stereo,std::uint32_t frames,OutputTransfer& result) noexcept
{
 result={};result.missingFrames=frames;
 if(!stereo || !frames || frames>CallbackMaximum)return;
 std::fill_n(stereo,frames*2,0.f);result.epoch=m_epoch;
 auto descriptor=m_descriptorRead.load(std::memory_order_relaxed);
 auto frame=m_frameRead.load(std::memory_order_relaxed);
 while(result.copiedFrames<frames && descriptor<m_descriptorWrite.load(std::memory_order_acquire))
 {
  const auto& stamp=m_stamps[descriptor%OutputSpans];
  const auto count=std::min(stamp.frames-m_partial,frames-result.copiedFrames);
  if(stamp.hasTarget && stamp.targetOffset>=m_partial && stamp.targetOffset-m_partial<count)
  {result.hasTarget=true;result.targetOffset=result.copiedFrames+stamp.targetOffset-m_partial;}
  for(std::uint32_t i=0;i<count;++i)
  {
   const auto& sample=m_pcm[(frame+i)%OutputCapacity];
   stereo[2*(result.copiedFrames+i)]=sample.left();stereo[2*(result.copiedFrames+i)+1]=sample.right();
  }
  frame+=count;m_partial+=count;result.copiedFrames+=count;
  m_frameRead.store(frame,std::memory_order_release);
  if(m_partial==stamp.frames){m_partial=0;++descriptor;m_descriptorRead.store(descriptor,std::memory_order_release);}
 }
 result.missingFrames=frames-result.copiedFrames;
}
}
