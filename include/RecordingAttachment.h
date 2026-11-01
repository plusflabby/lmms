#ifndef LMMS_RECORDING_ATTACHMENT_H
#define LMMS_RECORDING_ATTACHMENT_H
#include "RecordingPlacement.h"
#include "RecordingServiceHost.h"
#include <memory>
namespace lmms::recording {
struct AttachmentResult
{
	QString code, trackId, clipId, cause;
	bool attached = false;
};
class RecordingAttachment
{
public:
	RecordingAttachment(RecordingServiceHost&, RecordingTransportControl&);
	~RecordingAttachment();
	RecordingAttachment(const RecordingAttachment&) = delete;
	RecordingAttachment& operator=(const RecordingAttachment&) = delete;
	AttachmentResult attach(const SealedTakeToken&, const PlacementPlan&, quint64 expectedProjectGeneration);
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
}
#endif
