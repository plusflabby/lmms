#ifndef LMMS_AI_BRIDGE_VOCAL_VARIANT_ATTACHMENT_H
#define LMMS_AI_BRIDGE_VOCAL_VARIANT_ATTACHMENT_H

#include "AiBridgeVocalTypes.h"
#include <QJsonObject>

namespace lmms {

struct VocalVariantApplyRequest
{
	QString variantId, clipId, mode, destinationTrackId;
	QString expectedWorldHash, expectedAudioStateHash, expectedTrackStateHash;
	QString expectedDestinationTrackStateHash;
	quint64 expectedProjectGeneration = 0;
	bool dryRun = false;
};

struct VocalVariantApplyResult
{
	QString code;
	QJsonObject data;
};

class AiBridgeVocalVariantAttachment
{
public:
	static VocalVariantApplyResult apply(const VocalVariantApplyRequest& request);
};

} // namespace lmms

#endif
