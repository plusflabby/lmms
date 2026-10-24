#ifndef LMMS_AI_BRIDGE_VOCAL_TYPES_H
#define LMMS_AI_BRIDGE_VOCAL_TYPES_H

#include "AiBridgeProjectContext.h"
#include "AudioSourceWindow.h"
#include "SampleDecoder.h"
#include <array>
#include <optional>
#include <vector>
#include <QStringList>

namespace lmms {
namespace VocalLimits {
inline constexpr size_t selections=8, retainedJobs=8;
inline constexpr quint64 seconds=120, jobBytes=256ULL*1024*1024, sessionBytes=16ULL*1024*1024;
inline constexpr quint64 selectionRecords=4096, jobRecords=8192;
inline constexpr int page=256, tracks=128, idBytes=256, pathChars=32768, rootCount=32, rootChars=65536;
inline constexpr size_t projectTracks=4096, projectClips=4096, automationBindings=65536;
}
struct VocalFrameRange { qint64 inFrame=0, outFrame=0; };
struct VocalSelection { QString clipId, expectedAudioHash; VocalFrameRange frames; };
struct VocalAnalysisSettings { double lowLevelDbfs=-60; int minimumLowLevelMs=100; };
struct VocalAnalyzeRequest { std::vector<VocalSelection> selections; VocalAnalysisSettings settings; };

// PRIVATE native ticket. Only copied values, never Prepared, Root, device or model pointers.
struct VocalSourceTicket
{
	QString clipId, reference;
	QStringList roots;
	quint64 policyRevision=0;
	SampleDecoder::PcmWaveLimits limits;
	SampleDecoder::PcmWaveInfo expectedInfo;
	QByteArray expectedIdentity;
	QString sourceRevision, sourceStatus;
	std::optional<AudioSourceWindowV2> window;
	AudioWindowWorld world;
	AudioWindowGeometry geometry;
};
struct VocalInputSnapshot
{
	quint64 generation=0, policyRevision=0;
	VocalAnalyzeRequest request;
	std::vector<VocalSourceTicket> sources;
	QString inputHash;
};
struct VocalTrackSnapshot
{
	QString trackId, name, mixerId, membershipRevision, hash;
	quint64 generation=0;
	double volume=0, panning=0;
	bool muted=false, solo=false;
};
struct VocalChannelMetrics
{
	quint64 count=0, fullScaleCandidates=0, overrange=0;
	double peak=0, rms=0, dc=0;
	std::optional<double> peakDbfs, rmsDbfs;
	bool silence=true;
};
struct VocalSelectionMetadata
{
	QString clipId, sourceRevision;
	quint64 generation=0;
	VocalFrameRange frames;
	int sampleRate=0, channels=0;
	qint64 windowFrames=0;
	QString basis="decoded_source_pre_mixer", algorithm="levels_v1", authorization="available", sourceValidation="job_snapshot";
	VocalAnalysisSettings settings;
};
struct VocalAnalysisRecord
{
	enum class Kind { Window, LowLevelSpan };
	Kind kind=Kind::Window;
	int selectionIndex=0;
	VocalFrameRange frames;
	std::array<VocalChannelMetrics,2> channels;
};
struct VocalSelectionSummary
{
	VocalSelectionMetadata provenance;
	std::array<VocalChannelMetrics,2> channels;
};
struct VocalAnalysisResult
{
	QString code;
	std::vector<VocalSelectionSummary> summaries;
	std::vector<VocalAnalysisRecord> records;
	quint64 accountedBytes=0;
};
enum class VocalJobState { Queued, Running, CancelRequested, Cancelled, Succeeded, Failed, Invalidated };
enum class VocalJobPhase { Authorizing, Scanning, Analyzing, Publishing };
struct VocalJobSnapshot
{
	QString jobId, inputHash, analysisId, failureCode;
	quint64 generation=0;
	VocalJobState state=VocalJobState::Queued;
	VocalJobPhase phase=VocalJobPhase::Authorizing;
	std::optional<double> progress;
};
struct VocalTrackRequest
{
	QString trackId, expectedTrackHash;
	std::optional<double> volume, panning;
	bool dryRun=false;
};
struct VocalSnapshotResult { QString code; std::optional<VocalInputSnapshot> value; };
struct VocalScanResult { QString code; SampleDecoder::PcmWaveInfo info; };
struct VocalTrackResult { QString code; std::optional<VocalTrackSnapshot> value; };
struct VocalStartResult { QString code, jobId, inputHash; };
struct VocalJobResult { QString code; std::optional<VocalJobSnapshot> value; bool cancelApplied=false; };
struct VocalAnalysisPage
{
	QString code, analysisId, applicability;
	std::vector<VocalSelectionSummary> summaries;
	AiBridgePage page;
	std::vector<VocalAnalysisRecord> records;
};
struct VocalAffectedClip { QString clipId, audioHash; };
struct VocalTrackActionResult
{
	QString code, failureCode;
	std::optional<VocalTrackSnapshot> before, after;
	std::optional<double> proposedVolume, proposedPanning;
	std::vector<VocalAffectedClip> affectedClips;
	QStringList applied, failed, unapplied;
};
struct VocalBudget { quint64 jobBytes=0, sessionBytes=0, recordCapacity=0; QString code; };

std::optional<qint64> parseVocalFrame(const QString&);
QString validateVocalRequest(const VocalAnalyzeRequest&);
VocalBudget estimateVocalBudget(const VocalInputSnapshot&);
QString vocalInputHash(const VocalInputSnapshot&);
QString vocalTrackHash(const VocalTrackSnapshot&);
}
#endif
