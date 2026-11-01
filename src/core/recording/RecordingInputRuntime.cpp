#include "RecordingInputRuntime.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
namespace lmms::recording {
RecordingInputRuntime::RecordingInputRuntime(const InputProfile& profile,RecordingRing& ring) noexcept
 :m_profile(profile),m_ring(ring),
  m_profileReady(ring.isReady() && (profile.sampleRate==44100 || profile.sampleRate==48000)
   && profile.inputChannels>0 && profile.selectedChannel<profile.inputChannels
   && profile.maxCallback>0 && profile.maxCallback<=RecordingRing::MaximumCallback
   && profile.maxTakeFrames>0 && profile.maxTakeFrames<=std::uint64_t(profile.sampleRate)*120
   && ring.acceptedFrames()==0 && ring.droppedFrames()==0 && !ring.counterOverflowed()){}
bool RecordingInputRuntime::add(std::atomic<std::uint64_t>& counter,std::uint64_t delta) noexcept
{
 const auto value=counter.load(std::memory_order_relaxed);
 std::uint64_t next=0;
 if(!RecordingRing::checkedCounterAdvance(value,delta,next)){counter.store(std::numeric_limits<std::uint64_t>::max(),std::memory_order_release);return false;}
 counter.store(next,std::memory_order_release);return true;
}
void RecordingInputRuntime::latchFault(InputFault fault) noexcept
{
 auto expected=InputFault::None;
 m_fault.compare_exchange_strong(expected,fault,std::memory_order_acq_rel);
 auto reason=InputStopReason::None;
 m_stopReason.compare_exchange_strong(reason,InputStopReason::Fault,std::memory_order_acq_rel);
 m_stopRequested.store(true,std::memory_order_release);
}
void RecordingInputRuntime::reportFault(InputFault fault) noexcept
{
 if(fault!=InputFault::None)latchFault(fault);
}
void RecordingInputRuntime::outside(std::uint64_t frames) noexcept
{
 if(!add(m_intentionallyUnrecordedFrames,frames)){latchFault(InputFault::CounterOverflow);}
}
bool RecordingInputRuntime::enableCapture(InputEpoch epoch) noexcept
{
 if(!m_profileReady || !epoch)return false;
 bool expected=false;
 if(!m_started.compare_exchange_strong(expected,true,std::memory_order_acq_rel))return false;
 m_epochGeneration.store(epoch.projectGeneration,std::memory_order_relaxed);
 m_epochRevision.store(epoch.sessionRevision,std::memory_order_relaxed);
 m_enabled.store(true,std::memory_order_release);return true;
}
InputDecision RecordingInputRuntime::process(const InputBlock& block) noexcept
{
 if(!m_profileReady){latchFault(InputFault::InvalidProfile);return InputDecision::Stop;}
 if(!m_enabled.load(std::memory_order_acquire))return InputDecision::Continue;
 const auto firstFrame=m_receivedFrames.load(std::memory_order_relaxed);
 if(!add(m_receivedFrames,block.frames)){latchFault(InputFault::CounterOverflow);return InputDecision::Stop;}
 if(m_stopRequested.load(std::memory_order_acquire))
 {outside(block.frames);auto expected=InputStopReason::None;m_stopReason.compare_exchange_strong(expected,InputStopReason::Requested,std::memory_order_acq_rel);return InputDecision::Stop;}
 if(block.frames==0 || block.frames>m_profile.maxCallback || block.frames>RecordingRing::MaximumCallback
  || !block.input || block.channels!=m_profile.inputChannels || block.channels==0
  || m_profile.selectedChannel>=block.channels
  || std::size_t(block.channels)>std::numeric_limits<std::size_t>::max()/std::size_t(block.frames))
 {
  const auto available=firstFrame<m_profile.maxTakeFrames?m_profile.maxTakeFrames-firstFrame:0;
  const auto lost=std::min<std::uint64_t>(block.frames,available);
  if(!add(m_applicationDroppedFrames,lost))latchFault(InputFault::CounterOverflow);
  outside(std::uint64_t(block.frames)-lost);latchFault(InputFault::InvalidBuffer);return InputDecision::Stop;
 }
 const auto remaining=firstFrame<m_profile.maxTakeFrames?m_profile.maxTakeFrames-firstFrame:0;
 const auto framesToCapture=std::uint32_t(std::min<std::uint64_t>(block.frames,remaining));
 std::uint64_t invalid=0;
 for(std::uint32_t i=0;i<framesToCapture;++i)
 {
  const auto value=block.input[std::size_t(i)*block.channels+m_profile.selectedChannel];
  if(!std::isfinite(value)){++invalid;continue;}
  m_monoScratch[i]=value;
 }
 if(invalid)
 {
  if(!add(m_invalidSamples,invalid) || !add(m_applicationDroppedFrames,framesToCapture))latchFault(InputFault::CounterOverflow);
  outside(std::uint64_t(block.frames)-framesToCapture);latchFault(InputFault::InvalidSample);return InputDecision::Stop;
 }
 const auto pushed=m_ring.push(m_monoScratch.data(),framesToCapture,1,0);
 if(!pushed.valid)
 {
  if(pushed.counterOverflow)latchFault(InputFault::CounterOverflow);
  else latchFault(InputFault::RingContractMismatch);
  return InputDecision::Stop;
 }
 if(!add(m_acceptedFrames,pushed.acceptedFrames)){latchFault(InputFault::CounterOverflow);return InputDecision::Stop;}
 if(pushed.acceptedFrames)
 {
  float peak=0;
  for(std::uint32_t i=0;i<pushed.acceptedFrames;++i)peak=std::max(peak,std::abs(m_monoScratch[i]));
  m_inputPeakBits.store(std::bit_cast<std::uint32_t>(peak),std::memory_order_release);
 }
 const auto lost=std::uint64_t(framesToCapture)-pushed.acceptedFrames;
 if(lost)
 {
  if(!add(m_applicationDroppedFrames,lost)){latchFault(InputFault::CounterOverflow);return InputDecision::Stop;}
  if(pushed.limitReached)latchFault(InputFault::RingContractMismatch);
  else latchFault(InputFault::CaptureOverrun);
 }
 outside(std::uint64_t(block.frames)-framesToCapture);
 if(block.hostFlags&(InputHostOverflow|InputHostUnderflow))
 {m_hardwareLossCountKnown.store(false,std::memory_order_release);latchFault(InputFault::HardwareDiscontinuity);}
 if(block.hostFlags&~(InputHostOverflow|InputHostUnderflow))latchFault(InputFault::HardwareDiscontinuity);
 if(!std::isfinite(block.adcFirstSeconds) || !std::isfinite(block.currentFirstSeconds))
 {
  m_sawUnavailableClock=true;
  if(m_timestampQuality.load(std::memory_order_relaxed)!=InputTimestampQuality::Discontinuous)
   m_timestampQuality.store(InputTimestampQuality::Unavailable,std::memory_order_release);
 }
 else
 {
  if(m_haveClock && (block.adcFirstSeconds<=m_lastAdcTime || block.currentFirstSeconds<m_lastCurrentTime))
   m_timestampQuality.store(InputTimestampQuality::Discontinuous,std::memory_order_release);
  else if(!m_sawUnavailableClock && m_timestampQuality.load(std::memory_order_relaxed)==InputTimestampQuality::Unavailable)
   m_timestampQuality.store(InputTimestampQuality::Observed,std::memory_order_release);
  m_lastAdcTime=block.adcFirstSeconds;m_lastCurrentTime=block.currentFirstSeconds;m_haveClock=true;
 }
 if(framesToCapture)
 {
  const auto write=m_observationWrite.load(std::memory_order_relaxed),read=m_observationRead.load(std::memory_order_acquire);
  if(write<read || write==std::numeric_limits<std::uint64_t>::max() || write-read>=m_observations.size())
  {
   if(!add(m_observationsDropped,1))latchFault(InputFault::CounterOverflow);
   else latchFault(InputFault::ObservationOverflow);
  }
  else
  {
   m_observations[write%m_observations.size()]={firstFrame,block.frames,block.hostFlags,block.adcFirstSeconds,block.currentFirstSeconds,
    m_timestampQuality.load(std::memory_order_relaxed)};
   m_observationWrite.store(write+1,std::memory_order_release);
  }
 }
 if(m_fault.load(std::memory_order_acquire)!=InputFault::None)return InputDecision::Stop;
 if(pushed.limitReached || firstFrame+block.frames>=m_profile.maxTakeFrames)
 {
  auto expected=InputStopReason::None;m_stopReason.compare_exchange_strong(expected,InputStopReason::DurationLimit,std::memory_order_acq_rel);
  m_stopRequested.store(true,std::memory_order_release);return InputDecision::Stop;
 }
 return InputDecision::Continue;
}
void RecordingInputRuntime::requestStop(InputEpoch epoch) noexcept
{
 if(m_enabled.load(std::memory_order_acquire)
  && epoch.projectGeneration==m_epochGeneration.load(std::memory_order_relaxed)
  && epoch.sessionRevision==m_epochRevision.load(std::memory_order_relaxed))
  m_stopRequested.store(true,std::memory_order_release);
}
InputSnapshot RecordingInputRuntime::snapshot() const noexcept
{
 InputSnapshot result;
 result.inputPeak=std::bit_cast<float>(m_inputPeakBits.load(std::memory_order_acquire));
 if(m_enabled.load(std::memory_order_acquire))
  result.epoch={m_epochGeneration.load(std::memory_order_relaxed),m_epochRevision.load(std::memory_order_relaxed)};
 result.receivedFrames=m_receivedFrames.load(std::memory_order_acquire);
 result.acceptedFrames=m_acceptedFrames.load(std::memory_order_acquire);result.applicationDroppedFrames=m_applicationDroppedFrames.load(std::memory_order_acquire);
 result.intentionallyUnrecordedFrames=m_intentionallyUnrecordedFrames.load(std::memory_order_acquire);result.invalidSamples=m_invalidSamples.load(std::memory_order_acquire);
 result.observationsDropped=m_observationsDropped.load(std::memory_order_acquire);result.hardwareLostFrames=m_hardwareLostFrames.load(std::memory_order_acquire);
 result.hardwareLossCountKnown=m_hardwareLossCountKnown.load(std::memory_order_acquire);result.fault=m_fault.load(std::memory_order_acquire);
 result.stopReason=m_stopReason.load(std::memory_order_acquire);result.timestampQuality=m_timestampQuality.load(std::memory_order_acquire);return result;
}
bool RecordingInputRuntime::popObservation(InputObservation& result) noexcept
{
 const auto read=m_observationRead.load(std::memory_order_relaxed),write=m_observationWrite.load(std::memory_order_acquire);
 if(read>=write)return false;
 result=m_observations[read%m_observations.size()];m_observationRead.store(read+1,std::memory_order_release);return true;
}
}
