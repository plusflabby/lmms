#ifndef LMMS_MANAGED_VOCAL_WAVE_READER_H
#define LMMS_MANAGED_VOCAL_WAVE_READER_H

#include <QIODevice>
#include <QString>
#include <vector>
#include "lmms_export.h"

namespace lmms {

struct ManagedVocalWaveLimits
{
	quint64 maximumFileBytes = 0;
	quint64 maximumDecodedBytes = 0;
	quint64 maximumFrames = 0;
};

struct ManagedVocalWaveInfo
{
	quint64 frames = 0;
	quint64 dataBytes = 0;
	int sampleRate = 0;
	int channels = 0;
	QString pcmSha256;
	double peak = 0.0;
	bool overrange = false;
};

struct ManagedVocalWaveReadResult
{
	QString code;
	ManagedVocalWaveInfo info;
	std::vector<float> samples;
};

class LMMS_EXPORT ManagedVocalWaveReader
{
public:
	static ManagedVocalWaveReadResult read(
		QIODevice& source,
		const ManagedVocalWaveLimits& limits,
		bool retainSamples = true);
};

} // namespace lmms

#endif
