#ifndef LMMS_AIBRIDGE_ASSET_ADAPTERS_H
#define LMMS_AIBRIDGE_ASSET_ADAPTERS_H

#include "AiBridgeAssetTypes.h"

namespace lmms
{
class Instrument;
class InstrumentTrack;

class AudioFileProcessorAssetAdapter
{
public:
	static bool isCompatible(const Instrument *instrument);
	static AiBridgeAssetValidationResult validate(const QString &sampleId);
	static AiBridgeAssetLoadValue load(InstrumentTrack *track, const QString &sampleId,
		AiBridgeAssetValidationResult *validation = nullptr);
};
}

#endif
