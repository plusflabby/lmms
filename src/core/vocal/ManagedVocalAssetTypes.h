#ifndef LMMS_MANAGED_VOCAL_ASSET_TYPES_H
#define LMMS_MANAGED_VOCAL_ASSET_TYPES_H

#include <QByteArray>
#include <QDomDocument>
#include <QDomElement>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <optional>
#include <vector>
#include "lmms_export.h"

namespace lmms {

struct ManagedVocalFrameRange
{
	qint64 inFrame = 0;
	qint64 outFrame = 0;
};

enum class ManagedVocalSourceBasis : quint8
{
	AuthorizedPcm = 0,
	PlaybackReference = 1
};

struct ManagedVocalSourceProvenance
{
	QString clipId;
	QString sourceSha256;
	QString sourceIdentity;
	QString privateSourceReference;
	ManagedVocalFrameRange sourceFrames;
	qint64 sourceFrameCount = 0;
	int sampleRate = 0;
	int channels = 0;
	ManagedVocalSourceBasis basis = ManagedVocalSourceBasis::AuthorizedPcm;
	QString recordedTakeProjectId;
	QString recordedTakeId;
};

struct ManagedVocalMedia
{
	quint64 frames = 0;
	int sampleRate = 0;
	int channels = 0;
	QString pcmSha256;
	double peak = 0.0;
	bool overrange = false;
};

struct ManagedVocalManifest
{
	quint32 schemaVersion = 1;
	QString assetId;
	QString assetRootId;
	ManagedVocalMedia media;
	QString recipeHash;
	QString rendererId;
	QString rendererVersion;
	QJsonObject rendererOptions;
	QJsonObject recipe;
	QJsonArray realizedFrameMaps;
	std::vector<ManagedVocalSourceProvenance> sources;
};

// Persist this path-free value in SampleClip. The deterministic filename is
// derived from assetId inside the authorized root, never received from MCP.
struct ManagedVocalVariantReference
{
	QString assetId;
	QString assetRootId;
	QString mediaSha256;
	QString manifestSha256;
	quint64 frames = 0;
	int sampleRate = 0;
	int channels = 0;
};

struct ManagedVocalPlaybackContext
{
	int tempoBpm = 0;
	int outputRate = 0;
	int ticksPerQuarterNote = 48;
	int timeSignatureNumerator = 4;
	int timeSignatureDenominator = 4;
	qint64 visualLengthTicks = 0;
	quint64 nativeVisualCoverageFrames = 0;
	qint64 endpointResidualNumerator = 0;
	qint64 endpointResidualDenominator = 1;
	QString worldHash;
};

struct ManagedVocalVariantAssociation
{
	quint32 schemaVersion = 1;
	ManagedVocalVariantReference active;
	std::optional<ManagedVocalVariantReference> previousVariant;
	ManagedVocalPlaybackContext playback;
	std::optional<ManagedVocalPlaybackContext> previousPlayback;
	bool originalAutoResize = false;
	qint64 originalLengthTicks = 0;
	qint64 originalOffsetTicks = 0;
};

struct ManagedVocalManifestResult
{
	QString code;
	std::optional<ManagedVocalManifest> value;
};

struct ManagedVocalVariantReferenceResult
{
	QString code;
	std::optional<ManagedVocalVariantReference> value;
};

struct ManagedVocalVariantAssociationResult
{
	QString code;
	std::optional<ManagedVocalVariantAssociation> value;
};

LMMS_EXPORT QString validateManagedVocalManifest(const ManagedVocalManifest& manifest);
LMMS_EXPORT QByteArray encodeManagedVocalManifest(const ManagedVocalManifest& manifest);
LMMS_EXPORT ManagedVocalManifestResult decodeManagedVocalManifest(const QByteArray& bytes);
LMMS_EXPORT ManagedVocalVariantReferenceResult makeManagedVocalVariantReference(
	const ManagedVocalManifest& manifest,
	const QByteArray& encodedManifest);
LMMS_EXPORT QString validateManagedVocalVariantAssociation(
	const ManagedVocalVariantAssociation& association);
LMMS_EXPORT QDomElement writeManagedVocalVariantAssociation(
	QDomDocument& document,
	const ManagedVocalVariantAssociation& association);
LMMS_EXPORT ManagedVocalVariantAssociationResult readManagedVocalVariantAssociation(
	const QDomElement& element);

} // namespace lmms

#endif
