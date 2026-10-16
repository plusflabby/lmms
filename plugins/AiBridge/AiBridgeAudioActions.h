#ifndef LMMS_AIBRIDGE_AUDIO_ACTIONS_H
#define LMMS_AIBRIDGE_AUDIO_ACTIONS_H
#include "AiBridgeAudioContext.h"
#include "AiBridgeAudioPlanner.h"
#include <QStringList>
namespace lmms {
struct AiBridgeAudioTrackRequest
{
	QString name;
	double volume = 100, panning = 0;
	bool muted = false, dryRun = false;
};
struct AiBridgeAudioImportRequest
{
	QString trackId, path;
	qint64 position = 0;
	bool dryRun = false;
};
struct AiBridgeAudioActionResult
{
	QString code, trackId, clipId;
	QJsonObject preview, observed;
	QStringList applied, failed, unapplied;
	QString failureCode;
};
struct AiBridgeAudioEditResult
{
	QString code, failureCode;
	QJsonArray preview, expected, observed;
	bool previewHasMore = false;
	std::vector<int> applied, failed, unapplied, noEffect;
	QStringList affectedClipIds, createdClipIds;
	QString expectedWorldHash, observedWorldHash;
	QJsonObject toJson() const;
};
class AiBridgeAudioActions
{
public:
	AiBridgeAudioActionResult createTrack(const AiBridgeAudioTrackRequest& request) const;
	AiBridgeAudioActionResult importFile(const AiBridgeAudioImportRequest& request) const;
	AiBridgeAudioEditResult editClip(const AiBridgeAudioEdit& edit,bool dryRun) const;
	AiBridgeAudioEditResult batchEdit(const std::vector<AiBridgeAudioEdit>& edits,bool dryRun) const;
};
}
#endif
