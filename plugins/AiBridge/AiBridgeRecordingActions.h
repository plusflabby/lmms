#ifndef LMMS_AIBRIDGE_RECORDING_ACTIONS_H
#define LMMS_AIBRIDGE_RECORDING_ACTIONS_H

#include "AiBridgeProtocol.h"
#include "AiBridgeRecordingTypes.h"
#include <QThread>

namespace lmms::recording { class RecordingSessionManager; }

namespace lmms
{
class AiBridgeRecordingActions
{
public:
	explicit AiBridgeRecordingActions(recording::RecordingSessionManager* manager);
	AiBridgeProtocolResponse execute(const AiBridgeProtocolRequest&) const;
	bool blocksProjectMutation() const;

private:
	recording::RecordingSessionManager* m_manager;
	QThread* m_owner;
};
}

#endif
