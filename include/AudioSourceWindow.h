#ifndef LMMS_AUDIO_SOURCE_WINDOW_H
#define LMMS_AUDIO_SOURCE_WINDOW_H

#include <QDomDocument>
#include <QString>
#include <atomic>
#include <optional>
#include "SampleDecoder.h"
#include "lmms_export.h"

namespace lmms {
struct AudioSourceWindowV1
{
	qint64 inFrame=0, outFrame=0, phaseOriginFrame=0, sourceFrameCount=0;
	qint32 sourceRate=0, referenceTempoBpm=0, referenceOutputRate=0;
	qint32 ticksPerBeat=0, framesPerTick=0, lengthTicks=0, offsetTicks=0;
};
struct AudioSourceWindowV2
{
	AudioSourceWindowV1 base;
	qint64 referenceEndFrame=0;
};
enum class AudioWindowStorageKind { Legacy, V1, V2, InvalidMetadata };
struct AudioWindowStorage
{
	AudioWindowStorageKind kind=AudioWindowStorageKind::Legacy;
	std::optional<AudioSourceWindowV1> value;
	std::optional<AudioSourceWindowV2> valueV2;
	QDomDocument retainedInvalidXml;
	QString parseCode=QStringLiteral("ok");
};
struct AudioWindowWorld
{
	qint32 tempoBpm=0, outputRate=0, ticksPerBeat=0;
	bool tempoAutomationPresent=false;
};
struct AudioWindowGeometry
{
	qint64 positionTicks=0, lengthTicks=0, offsetTicks=0;
	bool autoResize=false, reversed=false, solo=false;
	QString nativeName, color;
	double frequency=0, amplification=0;
};
struct AudioWindowEligibility
{
	QString code;
	std::optional<AudioSourceWindowV1> window;
	std::optional<AudioSourceWindowV2> windowV2;
};
struct AudioWindowPlaybackGuard { std::atomic<quint64> epoch{1}; };

LMMS_EXPORT AudioWindowStorage parseAudioWindow(const QDomElement& sampleClip);
LMMS_EXPORT void writeAudioWindow(const AudioWindowStorage&, QDomDocument&, QDomElement& sampleClip);
LMMS_EXPORT QString validateAudioWindowRecord(const AudioSourceWindowV1&);
LMMS_EXPORT QString validateAudioWindowRecord(const AudioSourceWindowV2&);
// Shared fail-closed runtime predicate and first-conversion contract.
LMMS_EXPORT AudioWindowEligibility evaluateAudioWindow(const AudioWindowStorage&, const AudioWindowGeometry&,
	const AudioWindowWorld&, const QString& sourceCode, const std::optional<SampleDecoder::PcmWaveInfo>&);
LMMS_EXPORT AudioWindowEligibility convertLegacyAudioWindow(const AudioWindowGeometry&, const AudioWindowWorld&,
	const QString& sourceCode, const std::optional<SampleDecoder::PcmWaveInfo>&);
}
#endif
