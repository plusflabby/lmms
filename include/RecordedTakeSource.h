#ifndef LMMS_RECORDED_TAKE_SOURCE_H
#define LMMS_RECORDED_TAKE_SOURCE_H
#include "RecordingTakeTypes.h"
#include "lmms_export.h"
#include <QDomDocument>
#include <QDomElement>
#include <optional>
namespace lmms::recording {
using SealedTakeSourceCapability = SealedTakeToken;
struct RecordedTakeReference
{
	QString projectId, takeId, trackId, clipId;
	std::uint32_t sampleRate = 0;
	std::uint64_t rawFrames = 0, placementRevision = 0;
	RecordingFileIdentity sourceIdentity;
	QByteArray sourceDigest;
};
class LMMS_EXPORT RecordedTakeSource
{
public:
	static std::optional<RecordedTakeReference> fromFacts(const RawTakeFacts&);
	static QString validate(const RecordedTakeReference&);
	static bool matches(const RecordedTakeReference&, const RawTakeFacts&);
	static QDomElement writeReference(QDomDocument&, const RecordedTakeReference&);
	static std::optional<RecordedTakeReference> readReference(const QDomElement&, QString& code);
};
}
#endif
