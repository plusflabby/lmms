#ifndef LMMS_AI_BRIDGE_AUDIO_TYPES_H
#define LMMS_AI_BRIDGE_AUDIO_TYPES_H
#include <QString>
#include <QByteArray>
#include <optional>
#include <vector>
#include "AudioSourceWindow.h"
namespace lmms {
// Copied native state only. No native pointer or raw source path crosses this boundary.
struct AiBridgeAudioClipState
{
	QString id, trackId, sourceStatus, color;
	QString nativeName;
	QByteArray sourceReferenceHash, sourceIdentity, sourceDigest;
	qint64 position = 0, length = 0, offset = 0;
	bool muted = false, reversed = false, autoResize = false;
	bool solo = false;
	bool trackMuted = false;
	quint64 sourceFrames = 0;
	int sampleRate = 0, fxChannel = 0;
	std::optional<int> sourceChannels;
	double trackVolume = 0, trackPanning = 0;
	double playbackFrequency = 0, amplification = 0;
	std::optional<AudioSourceWindowV1> rangeWindow;
	std::optional<AudioSourceWindowV2> rangeWindowV2;
	QString rangeStatus;
	QByteArray invalidRangeMetadataDigest;
};
QString audioStateHash(std::vector<AiBridgeAudioClipState> clips);
}
#endif
