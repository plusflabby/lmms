#ifndef LMMS_RECORDING_SESSION_MANAGER_H
#define LMMS_RECORDING_SESSION_MANAGER_H
#include "RecordingTypes.h"
#include "RecordingServiceHost.h"
#include <memory>
namespace lmms {class Song;}
namespace lmms::recording {
struct RecordingTransportSnapshot
{
 bool outputCaptureSafe=true;
 bool available=false,playing=false,paused=false,exporting=false,looping=false,songMode=false,metronomeActive=false,constantTempo=true,stableMeter=true;
 QString projectId;
 quint64 projectGeneration=0,contextRevision=0,seekRevision=0,stateRevision=0;
 qint64 songTick=0;
 int tempo=120,numerator=4,denominator=4,ticksPerBar=192;
 unsigned sampleRate=48000;
};
struct RecordingPlaybackOwnership {quint64 serial=0,stateRevision=0;};
struct RecordingTransportResult {bool ok=false;QString code;RecordingPlaybackOwnership ownership;};
class RecordingTransportControl
{
public:
 virtual ~RecordingTransportControl()=default;
 virtual RecordingTransportSnapshot snapshot() const=0;
 virtual RecordingTransportResult seekSong(qint64)=0;
 virtual RecordingTransportResult playSong()=0;
 virtual RecordingTransportResult stopOwned(RecordingPlaybackOwnership)=0;
};
std::unique_ptr<RecordingTransportControl> createNativeRecordingTransport(Song&);
// Control-owner only. Host/transport outlive this coordinator; shutdown must reach
// quiescence before their owners (or the project engine) may be destroyed.
class RecordingSessionManager
{
public:
 RecordingSessionManager(RecordingServiceHost&,RecordingTransportControl&);
 ~RecordingSessionManager();
 RecordingSessionManager(const RecordingSessionManager&)=delete;
 RecordingSessionManager& operator=(const RecordingSessionManager&)=delete;
 RecordingCapabilities capabilities() const noexcept;
 RecordingReply prepare(const PrepareTakeRequest&);
 RecordingReply arm(LocalInputTicket&&);
 RecordingReply start(const StartTakeRequest&);
 RecordingReply stop(const QString& takeId);
 RecordingReply disarm();
 RecordingReply requestShutdown();
 RecordingReply confirmPlacement(const ManualPlacementApproval&);
 RecordingStatus status() const;
 TakePage takes(PageRequest={}) const;
 RecordingStatus poll(quint64 nowMilliseconds);
 bool quiescent() const;
private:
 struct Impl;
 std::unique_ptr<Impl> m_impl;
};
}
#endif
