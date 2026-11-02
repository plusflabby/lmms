#include "ManagedVocalAssetTypes.h"
#include "ManagedVocalPlayback.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>
#include <QSet>

namespace lmms {
namespace {

constexpr qsizetype MaximumManifestBytes = 16 * 1024 * 1024;

bool digest(const QString& value)
{
	if (value.size() != 64)
	{
		return false;
	}
	for (const auto ch : value)
	{
		if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
		{
			return false;
		}
	}
	return true;
}

bool uuid(const QString& value)
{
	const auto parsed = QUuid(value);
	return !parsed.isNull() && parsed.toString(QUuid::WithoutBraces) == value;
}

bool identifier(const QString& value, int maximum)
{
	if (value.isEmpty() || value.size() > maximum)
	{
		return false;
	}
	for (const auto ch : value)
	{
		const bool alphaNumeric = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
			|| (ch >= '0' && ch <= '9');
		if (!alphaNumeric && ch != '-' && ch != '_' && ch != '.' && ch != ':')
		{
			return false;
		}
	}
	return true;
}

bool parseUnsigned(const QString& text, quint64& value)
{
	if (text.isEmpty() || text.size() > 20 || (text.size() > 1 && text[0] == '0'))
	{
		return false;
	}
	value = 0;
	for (const auto ch : text)
	{
		if (ch < '0' || ch > '9')
		{
			return false;
		}
		const auto digit = quint64(ch.unicode() - '0');
		if (value > (std::numeric_limits<quint64>::max() - digit) / 10)
		{
			return false;
		}
		value = value * 10 + digit;
	}
	return true;
}

bool parseSignedFrame(const QString& text, qint64& value)
{
	quint64 parsed = 0;
	if (!parseUnsigned(text, parsed) || parsed > quint64(std::numeric_limits<qint64>::max()))
	{
		return false;
	}
	value = qint64(parsed);
	return true;
}

bool parseCanonicalSigned(const QString& text, qint64& value)
{
	bool ok = false;
	const auto parsed = text.toLongLong(&ok, 10);
	if (!ok || QString::number(parsed) != text)
	{
		return false;
	}
	value = parsed;
	return true;
}

bool integer(const QJsonValue& value, int& output)
{
	if (!value.isDouble())
	{
		return false;
	}
	const auto number = value.toDouble();
	if (!std::isfinite(number) || std::floor(number) != number
		|| number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
	{
		return false;
	}
	output = int(number);
	return true;
}

bool exactKeys(const QJsonObject& object, const QStringList& expected)
{
	if (object.size() != expected.size())
	{
		return false;
	}
	return std::all_of(expected.begin(), expected.end(), [&object](const QString& key) {
		return object.contains(key);
	});
}

QJsonObject sourceToJson(const ManagedVocalSourceProvenance& source)
{
	const auto basis = source.basis == ManagedVocalSourceBasis::AuthorizedPcm
		? QStringLiteral("authorized_pcm") : QStringLiteral("playback_reference");
	return {
		{"clip_id", source.clipId},
		{"source_sha256", source.sourceSha256},
		{"source_identity", source.sourceIdentity},
		{"private_source_reference", source.privateSourceReference},
		{"source_frames", QJsonObject{{"in", QString::number(source.sourceFrames.inFrame)},
			{"out", QString::number(source.sourceFrames.outFrame)}}},
		{"source_frame_count", QString::number(source.sourceFrameCount)},
		{"sample_rate", source.sampleRate},
		{"channels", source.channels},
		{"source_basis", basis},
		{"recorded_take_project_id", source.recordedTakeProjectId},
		{"recorded_take_id", source.recordedTakeId}
	};
}

QJsonObject manifestToJson(const ManagedVocalManifest& manifest)
{
	QJsonArray sources;
	for (const auto& source : manifest.sources)
	{
		sources.push_back(sourceToJson(source));
	}
	QJsonObject media{
		{"frames", QString::number(manifest.media.frames)},
		{"sample_rate", manifest.media.sampleRate},
		{"channels", manifest.media.channels},
		{"pcm_sha256", manifest.media.pcmSha256},
		{"peak", manifest.media.peak},
		{"overrange", manifest.media.overrange}
	};
	if (manifest.schemaVersion >= 2)
	{
		media.insert(QStringLiteral("rms"), manifest.media.rmsKnown
			? QJsonValue(manifest.media.rms) : QJsonValue(QJsonValue::Null));
	}
	QJsonObject result{
		{"schema_version", int(manifest.schemaVersion)},
		{"asset_id", manifest.assetId},
		{"asset_root_id", manifest.assetRootId},
		{"media", media},
		{"recipe_hash", manifest.recipeHash},
		{"renderer_id", manifest.rendererId},
		{"renderer_version", manifest.rendererVersion},
		{"renderer_options", manifest.rendererOptions},
		{"recipe", manifest.recipe},
		{"realized_frame_maps", manifest.realizedFrameMaps},
		{"sources", sources}
	};
	if (manifest.schemaVersion >= 2)
	{
		result.insert(QStringLiteral("project_id"), manifest.projectId);
	}
	return result;
}

bool readSource(const QJsonObject& object, ManagedVocalSourceProvenance& source)
{
	if (!exactKeys(object, {"clip_id", "source_sha256", "source_identity", "private_source_reference",
			"source_frames", "source_frame_count", "sample_rate", "channels", "source_basis",
			"recorded_take_project_id", "recorded_take_id"}))
	{
		return false;
	}
	const auto frames = object["source_frames"].toObject();
	if (!exactKeys(frames, {"in", "out"}) || !object["clip_id"].isString()
		|| !object["source_sha256"].isString() || !object["source_identity"].isString()
		|| !object["private_source_reference"].isString() || !object["source_frame_count"].isString()
		|| !object["source_basis"].isString() || !object["recorded_take_project_id"].isString()
		|| !object["recorded_take_id"].isString())
	{
		return false;
	}
	const auto sourceIn = frames["in"].toString();
	const auto sourceOut = frames["out"].toString();
	const auto frameCount = object["source_frame_count"].toString();
	if (!parseSignedFrame(sourceIn, source.sourceFrames.inFrame)
		|| !parseSignedFrame(sourceOut, source.sourceFrames.outFrame)
		|| !parseSignedFrame(frameCount, source.sourceFrameCount)
		|| !integer(object["sample_rate"], source.sampleRate)
		|| !integer(object["channels"], source.channels))
	{
		return false;
	}
	source.clipId = object["clip_id"].toString();
	source.sourceSha256 = object["source_sha256"].toString();
	source.sourceIdentity = object["source_identity"].toString();
	source.privateSourceReference = object["private_source_reference"].toString();
	source.recordedTakeProjectId = object["recorded_take_project_id"].toString();
	source.recordedTakeId = object["recorded_take_id"].toString();
	const auto basis = object["source_basis"].toString();
	if (basis == QStringLiteral("authorized_pcm"))
	{
		source.basis = ManagedVocalSourceBasis::AuthorizedPcm;
	}
	else if (basis == QStringLiteral("playback_reference"))
	{
		source.basis = ManagedVocalSourceBasis::PlaybackReference;
	}
	else
	{
		return false;
	}
	return true;
}

bool validVariantReference(const ManagedVocalVariantReference& reference)
{
	return uuid(reference.assetId) && digest(reference.assetRootId) && digest(reference.mediaSha256)
		&& digest(reference.manifestSha256) && reference.frames > 0
		&& (reference.sampleRate == 44100 || reference.sampleRate == 48000)
		&& (reference.channels == 1 || reference.channels == 2)
		&& reference.frames <= quint64(reference.sampleRate) * 120;
}

bool validPlaybackContext(
	const ManagedVocalPlaybackContext& playback,
	const ManagedVocalVariantReference& reference)
{
	if (playback.tempoBpm < 10 || playback.tempoBpm > 999
		|| playback.outputRate != reference.sampleRate || playback.ticksPerQuarterNote != 48
		|| playback.timeSignatureNumerator < 1 || playback.timeSignatureNumerator > 32
		|| playback.timeSignatureDenominator < 1 || playback.timeSignatureDenominator > 32
		|| playback.visualLengthTicks <= 0
		|| playback.visualLengthTicks > std::numeric_limits<qint32>::max()
		|| playback.endpointResidualNumerator < 0 || playback.endpointResidualDenominator <= 0
		|| !digest(playback.worldHash))
	{
		return false;
	}
	const auto extent = managedVocalVisualExtent(reference.frames, reference.sampleRate, playback.tempoBpm);
	return extent.code == QStringLiteral("ok")
		&& playback.visualLengthTicks == extent.visualLengthTicks
		&& playback.nativeVisualCoverageFrames == extent.nativeCoverageFrames
		&& playback.endpointResidualNumerator == extent.exactResidualNumerator
		&& playback.endpointResidualDenominator == extent.exactResidualDenominator;
}

void writeReferenceAttributes(QDomElement& element, const ManagedVocalVariantReference& reference)
{
	element.setAttribute(QStringLiteral("asset_id"), reference.assetId);
	element.setAttribute(QStringLiteral("asset_root_id"), reference.assetRootId);
	element.setAttribute(QStringLiteral("media_sha256"), reference.mediaSha256);
	element.setAttribute(QStringLiteral("manifest_sha256"), reference.manifestSha256);
	element.setAttribute(QStringLiteral("frames"), QString::number(reference.frames));
	element.setAttribute(QStringLiteral("sample_rate"), QString::number(reference.sampleRate));
	element.setAttribute(QStringLiteral("channels"), QString::number(reference.channels));
}

bool readReferenceAttributes(const QDomElement& element, ManagedVocalVariantReference& reference,
	bool allowAdditionalAttributes = false)
{
	if ((!allowAdditionalAttributes && element.attributes().count() != 7)
		|| (allowAdditionalAttributes && element.attributes().count() < 7))
	{
		return false;
	}
	quint64 frames = 0;
	quint64 rate = 0;
	quint64 channels = 0;
	if (!parseUnsigned(element.attribute(QStringLiteral("frames")), frames)
		|| !parseUnsigned(element.attribute(QStringLiteral("sample_rate")), rate)
		|| !parseUnsigned(element.attribute(QStringLiteral("channels")), channels)
		|| rate > quint64(std::numeric_limits<int>::max())
		|| channels > quint64(std::numeric_limits<int>::max()))
	{
		return false;
	}
	reference.assetId = element.attribute(QStringLiteral("asset_id"));
	reference.assetRootId = element.attribute(QStringLiteral("asset_root_id"));
	reference.mediaSha256 = element.attribute(QStringLiteral("media_sha256"));
	reference.manifestSha256 = element.attribute(QStringLiteral("manifest_sha256"));
	reference.frames = frames;
	reference.sampleRate = int(rate);
	reference.channels = int(channels);
	return validVariantReference(reference);
}

void writePlaybackAttributes(QDomElement& element, const ManagedVocalPlaybackContext& playback)
{
	element.setAttribute(QStringLiteral("tempo_bpm"), QString::number(playback.tempoBpm));
	element.setAttribute(QStringLiteral("output_rate"), QString::number(playback.outputRate));
	element.setAttribute(QStringLiteral("ticks_per_quarter_note"),
		QString::number(playback.ticksPerQuarterNote));
	element.setAttribute(QStringLiteral("time_signature_numerator"),
		QString::number(playback.timeSignatureNumerator));
	element.setAttribute(QStringLiteral("time_signature_denominator"),
		QString::number(playback.timeSignatureDenominator));
	element.setAttribute(QStringLiteral("visual_length_ticks"), QString::number(playback.visualLengthTicks));
	element.setAttribute(QStringLiteral("native_visual_coverage_frames"),
		QString::number(playback.nativeVisualCoverageFrames));
	element.setAttribute(QStringLiteral("endpoint_error_numerator"),
		QString::number(playback.endpointResidualNumerator));
	element.setAttribute(QStringLiteral("endpoint_error_denominator"),
		QString::number(playback.endpointResidualDenominator));
	element.setAttribute(QStringLiteral("world_hash"), playback.worldHash);
}

bool readPlaybackAttributes(const QDomElement& element, ManagedVocalPlaybackContext& playback,
	bool allowAdditionalAttributes = false)
{
	const QStringList names{
		QStringLiteral("tempo_bpm"), QStringLiteral("output_rate"), QStringLiteral("ticks_per_quarter_note"),
		QStringLiteral("time_signature_numerator"), QStringLiteral("time_signature_denominator"),
		QStringLiteral("visual_length_ticks"), QStringLiteral("native_visual_coverage_frames"),
		QStringLiteral("endpoint_error_numerator"),
		QStringLiteral("endpoint_error_denominator"), QStringLiteral("world_hash")
	};
	if ((!allowAdditionalAttributes && element.attributes().count() != names.size())
		|| (allowAdditionalAttributes && element.attributes().count() < names.size()))
	{
		return false;
	}
	for (const auto& name : names)
	{
		if (!element.hasAttribute(name)) return false;
	}
	quint64 tempo = 0;
	quint64 rate = 0;
	quint64 ticksPerQuarterNote = 0;
	quint64 timeSignatureNumerator = 0;
	quint64 timeSignatureDenominator = 0;
	quint64 nativeVisualCoverageFrames = 0;
	if (!parseUnsigned(element.attribute(QStringLiteral("tempo_bpm")), tempo)
		|| !parseUnsigned(element.attribute(QStringLiteral("output_rate")), rate)
		|| !parseUnsigned(element.attribute(QStringLiteral("ticks_per_quarter_note")), ticksPerQuarterNote)
		|| !parseUnsigned(element.attribute(QStringLiteral("time_signature_numerator")), timeSignatureNumerator)
		|| !parseUnsigned(element.attribute(QStringLiteral("time_signature_denominator")), timeSignatureDenominator)
		|| !parseUnsigned(element.attribute(QStringLiteral("native_visual_coverage_frames")),
			nativeVisualCoverageFrames)
		|| tempo > quint64(std::numeric_limits<int>::max())
		|| rate > quint64(std::numeric_limits<int>::max())
		|| ticksPerQuarterNote > quint64(std::numeric_limits<int>::max())
		|| timeSignatureNumerator > quint64(std::numeric_limits<int>::max())
		|| timeSignatureDenominator > quint64(std::numeric_limits<int>::max())
		|| !parseSignedFrame(element.attribute(QStringLiteral("visual_length_ticks")),
			playback.visualLengthTicks)
		|| !parseSignedFrame(element.attribute(QStringLiteral("endpoint_error_numerator")),
			playback.endpointResidualNumerator)
		|| !parseSignedFrame(element.attribute(QStringLiteral("endpoint_error_denominator")),
			playback.endpointResidualDenominator))
	{
		return false;
	}
	playback.tempoBpm = int(tempo);
	playback.outputRate = int(rate);
	playback.ticksPerQuarterNote = int(ticksPerQuarterNote);
	playback.timeSignatureNumerator = int(timeSignatureNumerator);
	playback.timeSignatureDenominator = int(timeSignatureDenominator);
	playback.nativeVisualCoverageFrames = nativeVisualCoverageFrames;
	playback.worldHash = element.attribute(QStringLiteral("world_hash"));
	return true;
}

} // namespace

QString validateManagedVocalManifest(const ManagedVocalManifest& manifest)
{
	if ((manifest.schemaVersion != 1 && manifest.schemaVersion != 2)
		|| !uuid(manifest.assetId)
		|| (manifest.schemaVersion == 1 && !manifest.projectId.isEmpty())
		|| (manifest.schemaVersion == 2 && !uuid(manifest.projectId))
		|| (manifest.schemaVersion == 1 && manifest.media.rmsKnown)
		|| (manifest.schemaVersion == 2 && (!manifest.media.rmsKnown
			|| !std::isfinite(manifest.media.rms) || manifest.media.rms < 0.0))
		|| !digest(manifest.assetRootId)
		|| !digest(manifest.media.pcmSha256)
		|| !digest(manifest.recipeHash) || !identifier(manifest.rendererId, 128)
		|| !identifier(manifest.rendererVersion, 64) || !std::isfinite(manifest.media.peak)
		|| manifest.media.peak < 0.0 || manifest.media.overrange != (manifest.media.peak > 1.0)
		|| (manifest.media.sampleRate != 44100 && manifest.media.sampleRate != 48000)
		|| (manifest.media.channels != 1 && manifest.media.channels != 2)
		|| !manifest.media.frames
		|| manifest.media.frames > quint64(manifest.media.sampleRate) * 120
		|| manifest.sources.empty() || manifest.sources.size() > 8
		|| manifest.recipe.isEmpty())
	{
		return QStringLiteral("invalid_manifest");
	}
	QSet<QString> clipIds;
	for (const auto& source : manifest.sources)
	{
		if (!identifier(source.clipId, 256) || clipIds.contains(source.clipId)
			|| !digest(source.sourceSha256) || !digest(source.sourceIdentity)
			|| source.sourceFrameCount <= 0 || source.sourceFrames.inFrame < 0
			|| source.sourceFrames.outFrame <= source.sourceFrames.inFrame
			|| source.sourceFrames.outFrame > source.sourceFrameCount
			|| (source.sampleRate != 44100 && source.sampleRate != 48000)
			|| source.sampleRate != manifest.media.sampleRate
			|| (source.channels != 1 && source.channels != 2)
			|| source.channels != manifest.media.channels
			|| (source.basis != ManagedVocalSourceBasis::AuthorizedPcm
				&& source.basis != ManagedVocalSourceBasis::PlaybackReference)
			|| source.privateSourceReference.size() > 1024
			|| source.privateSourceReference.contains(QChar(0)))
		{
			return QStringLiteral("invalid_manifest");
		}
		const bool hasTakeProject = !source.recordedTakeProjectId.isEmpty();
		const bool hasTakeId = !source.recordedTakeId.isEmpty();
		if (hasTakeProject != hasTakeId
			|| (hasTakeProject && (!uuid(source.recordedTakeProjectId) || !uuid(source.recordedTakeId)))
			|| (!hasTakeProject && source.privateSourceReference.isEmpty()))
		{
			return QStringLiteral("invalid_manifest");
		}
		clipIds.insert(source.clipId);
	}
	const auto json = QJsonDocument(manifestToJson(manifest)).toJson(QJsonDocument::Compact);
	return json.size() <= MaximumManifestBytes ? QStringLiteral("ok") : QStringLiteral("resource_limit");
}

QByteArray encodeManagedVocalManifest(const ManagedVocalManifest& manifest)
{
	if (validateManagedVocalManifest(manifest) != QStringLiteral("ok"))
	{
		return {};
	}
	return QJsonDocument(manifestToJson(manifest)).toJson(QJsonDocument::Compact);
}

ManagedVocalManifestResult decodeManagedVocalManifest(const QByteArray& bytes)
{
	if (bytes.isEmpty() || bytes.size() > MaximumManifestBytes)
	{
		return {bytes.size() > MaximumManifestBytes ? QStringLiteral("resource_limit")
			: QStringLiteral("invalid_manifest"), {}};
	}
	QJsonParseError parse{};
	const auto document = QJsonDocument::fromJson(bytes, &parse);
	if (parse.error != QJsonParseError::NoError || !document.isObject())
	{
		return {QStringLiteral("invalid_manifest"), {}};
	}
	const auto object = document.object();
	int schemaVersion = 0;
	if (!integer(object.value(QStringLiteral("schema_version")), schemaVersion)
		|| (schemaVersion != 1 && schemaVersion != 2))
	{
		return {QStringLiteral("invalid_manifest"), {}};
	}
	const QStringList expectedKeys{
		QStringLiteral("schema_version"), QStringLiteral("asset_id"), QStringLiteral("asset_root_id"),
		QStringLiteral("media"), QStringLiteral("recipe_hash"), QStringLiteral("renderer_id"),
		QStringLiteral("renderer_version"), QStringLiteral("renderer_options"), QStringLiteral("recipe"),
		QStringLiteral("realized_frame_maps"), QStringLiteral("sources")};
	auto keys = expectedKeys;
	if (schemaVersion == 2) keys.push_back(QStringLiteral("project_id"));
	if (!exactKeys(object, keys))
	{
		return {QStringLiteral("invalid_manifest"), {}};
	}
	ManagedVocalManifest manifest;
	if (!object["asset_id"].isString() || !object["asset_root_id"].isString()
		|| (schemaVersion == 2 && !object["project_id"].isString())
		|| !object["media"].isObject()
		|| !object["recipe_hash"].isString() || !object["renderer_id"].isString()
		|| !object["renderer_version"].isString() || !object["renderer_options"].isObject()
		|| !object["recipe"].isObject() || !object["realized_frame_maps"].isArray()
		|| !object["sources"].isArray())
	{
		return {QStringLiteral("invalid_manifest"), {}};
	}
	manifest.schemaVersion = quint32(schemaVersion);
	manifest.assetId = object["asset_id"].toString();
	manifest.projectId = schemaVersion == 2 ? object["project_id"].toString() : QString{};
	manifest.assetRootId = object["asset_root_id"].toString();
	manifest.recipeHash = object["recipe_hash"].toString();
	manifest.rendererId = object["renderer_id"].toString();
	manifest.rendererVersion = object["renderer_version"].toString();
	manifest.rendererOptions = object["renderer_options"].toObject();
	manifest.recipe = object["recipe"].toObject();
	manifest.realizedFrameMaps = object["realized_frame_maps"].toArray();

	const auto media = object["media"].toObject();
	QStringList expectedMediaKeys{QStringLiteral("frames"), QStringLiteral("sample_rate"),
		QStringLiteral("channels"), QStringLiteral("pcm_sha256"), QStringLiteral("peak"),
		QStringLiteral("overrange")};
	if (schemaVersion == 2) expectedMediaKeys.push_back(QStringLiteral("rms"));
	if (!exactKeys(media, expectedMediaKeys)
		|| !media["frames"].isString() || !media["pcm_sha256"].isString()
		|| !media["peak"].isDouble() || !media["overrange"].isBool()
		|| (schemaVersion == 2 && !media["rms"].isDouble())
		|| !parseUnsigned(media["frames"].toString(), manifest.media.frames)
		|| !integer(media["sample_rate"], manifest.media.sampleRate)
		|| !integer(media["channels"], manifest.media.channels))
	{
		return {QStringLiteral("invalid_manifest"), {}};
	}
	manifest.media.pcmSha256 = media["pcm_sha256"].toString();
	manifest.media.peak = media["peak"].toDouble();
	manifest.media.overrange = media["overrange"].toBool();
	if (schemaVersion == 2)
	{
		manifest.media.rms = media["rms"].toDouble();
		manifest.media.rmsKnown = true;
	}

	const auto sourceValues = object["sources"].toArray();
	if (sourceValues.size() > 8)
	{
		return {QStringLiteral("resource_limit"), {}};
	}
	for (const auto& value : sourceValues)
	{
		if (!value.isObject())
		{
			return {QStringLiteral("invalid_manifest"), {}};
		}
		ManagedVocalSourceProvenance source;
		if (!readSource(value.toObject(), source))
		{
			return {QStringLiteral("invalid_manifest"), {}};
		}
		manifest.sources.push_back(std::move(source));
	}
	const auto code = validateManagedVocalManifest(manifest);
	return code == QStringLiteral("ok")
		? ManagedVocalManifestResult{code, std::move(manifest)}
		: ManagedVocalManifestResult{code, {}};
}

ManagedVocalVariantReferenceResult makeManagedVocalVariantReference(
	const ManagedVocalManifest& manifest,
	const QByteArray& encodedManifest)
{
	const auto canonicalManifest = encodeManagedVocalManifest(manifest);
	if (canonicalManifest.isEmpty() || canonicalManifest != encodedManifest)
	{
		return {QStringLiteral("invalid_manifest"), {}};
	}
	ManagedVocalVariantReference reference;
	reference.assetId = manifest.assetId;
	reference.assetRootId = manifest.assetRootId;
	reference.mediaSha256 = manifest.media.pcmSha256;
	reference.manifestSha256 = QString::fromLatin1(
		QCryptographicHash::hash(encodedManifest, QCryptographicHash::Sha256).toHex());
	reference.frames = manifest.media.frames;
	reference.sampleRate = manifest.media.sampleRate;
	reference.channels = manifest.media.channels;
	return {QStringLiteral("ok"), std::move(reference)};
}

QString validateManagedVocalVariantAssociation(const ManagedVocalVariantAssociation& association)
{
	if (association.schemaVersion != 1 || !validVariantReference(association.active)
		|| (association.previousVariant && !validVariantReference(*association.previousVariant))
		|| !validPlaybackContext(association.playback, association.active)
		|| bool(association.previousVariant) != bool(association.previousPlayback)
		|| association.originalLengthTicks <= 0
		|| association.originalLengthTicks > std::numeric_limits<qint32>::max()
		|| association.originalOffsetTicks < std::numeric_limits<qint32>::min()
		|| association.originalOffsetTicks > std::numeric_limits<qint32>::max())
	{
		return QStringLiteral("invalid_managed_variant");
	}
	const auto& playback = association.playback;
	if (playback.tempoBpm < 10 || playback.tempoBpm > 999
		|| playback.outputRate != association.active.sampleRate
		|| playback.ticksPerQuarterNote != 48
		|| playback.timeSignatureNumerator < 1 || playback.timeSignatureNumerator > 32
		|| playback.timeSignatureDenominator < 1 || playback.timeSignatureDenominator > 32
		|| playback.visualLengthTicks <= 0
		|| playback.visualLengthTicks > std::numeric_limits<qint32>::max()
		|| playback.endpointResidualNumerator < 0 || playback.endpointResidualDenominator <= 0
		|| !digest(playback.worldHash))
	{
		return QStringLiteral("invalid_managed_variant");
	}
	if (association.previousVariant)
	{
		if (!validPlaybackContext(*association.previousPlayback, *association.previousVariant))
		{
			return QStringLiteral("invalid_managed_variant");
		}
	}
	return QStringLiteral("ok");
}

QDomElement writeManagedVocalVariantAssociation(
	QDomDocument& document,
	const ManagedVocalVariantAssociation& association)
{
	if (validateManagedVocalVariantAssociation(association) != QStringLiteral("ok"))
	{
		return {};
	}
	auto element = document.createElement(QStringLiteral("managed_vocal_variant"));
	element.setAttribute(QStringLiteral("schema"), QString::number(association.schemaVersion));
	writeReferenceAttributes(element, association.active);
	writePlaybackAttributes(element, association.playback);
	element.setAttribute(QStringLiteral("original_auto_resize"), association.originalAutoResize ? "1" : "0");
	element.setAttribute(QStringLiteral("original_length_ticks"), QString::number(association.originalLengthTicks));
	element.setAttribute(QStringLiteral("original_offset_ticks"), QString::number(association.originalOffsetTicks));
	if (association.previousVariant)
	{
		auto previous = document.createElement(QStringLiteral("previous_variant"));
		writeReferenceAttributes(previous, *association.previousVariant);
		writePlaybackAttributes(previous, *association.previousPlayback);
		element.appendChild(previous);
	}
	return element;
}

ManagedVocalVariantAssociationResult readManagedVocalVariantAssociation(const QDomElement& element)
{
	const QStringList expectedAttributes{
		QStringLiteral("schema"), QStringLiteral("asset_id"), QStringLiteral("asset_root_id"),
		QStringLiteral("media_sha256"), QStringLiteral("manifest_sha256"), QStringLiteral("frames"),
		QStringLiteral("sample_rate"), QStringLiteral("channels"), QStringLiteral("tempo_bpm"),
		QStringLiteral("output_rate"), QStringLiteral("ticks_per_quarter_note"),
		QStringLiteral("time_signature_numerator"), QStringLiteral("time_signature_denominator"),
		QStringLiteral("visual_length_ticks"),
		QStringLiteral("native_visual_coverage_frames"),
		QStringLiteral("endpoint_error_numerator"), QStringLiteral("endpoint_error_denominator"),
		QStringLiteral("world_hash"), QStringLiteral("original_auto_resize"),
		QStringLiteral("original_length_ticks"), QStringLiteral("original_offset_ticks")
	};
	if (element.isNull() || element.tagName() != QStringLiteral("managed_vocal_variant")
		|| element.attributes().count() != expectedAttributes.size())
	{
		return {QStringLiteral("invalid_managed_variant"), {}};
	}
	for (const auto& name : expectedAttributes)
	{
		if (!element.hasAttribute(name))
		{
			return {QStringLiteral("invalid_managed_variant"), {}};
		}
	}
	ManagedVocalVariantAssociation association;
	quint64 schema = 0;
	if (!parseUnsigned(element.attribute(QStringLiteral("schema")), schema)
		|| schema > std::numeric_limits<quint32>::max()
		|| !readReferenceAttributes(element, association.active, true)
		|| !readPlaybackAttributes(element, association.playback, true)
		|| (element.attribute(QStringLiteral("original_auto_resize")) != QStringLiteral("0")
			&& element.attribute(QStringLiteral("original_auto_resize")) != QStringLiteral("1"))
		|| !parseSignedFrame(element.attribute(QStringLiteral("original_length_ticks")),
			association.originalLengthTicks)
		|| !parseCanonicalSigned(element.attribute(QStringLiteral("original_offset_ticks")),
			association.originalOffsetTicks))
	{
		return {QStringLiteral("invalid_managed_variant"), {}};
	}
	association.schemaVersion = quint32(schema);
	association.originalAutoResize = element.attribute(QStringLiteral("original_auto_resize")) == QStringLiteral("1");
	for (auto child = element.firstChild(); !child.isNull(); child = child.nextSibling())
	{
		if (child.isText() && child.nodeValue().trimmed().isEmpty())
		{
			continue;
		}
		if (!child.isElement() || child.toElement().tagName() != QStringLiteral("previous_variant")
			|| association.previousVariant)
		{
			return {QStringLiteral("invalid_managed_variant"), {}};
		}
		ManagedVocalVariantReference previous;
		const auto previousElement = child.toElement();
		ManagedVocalPlaybackContext previousPlayback;
		if (previousElement.attributes().count() != 17
			|| !readReferenceAttributes(previousElement, previous, true)
			|| !readPlaybackAttributes(previousElement, previousPlayback, true)
			|| !previousElement.firstChild().isNull())
		{
			return {QStringLiteral("invalid_managed_variant"), {}};
		}
		association.previousVariant = std::move(previous);
		association.previousPlayback = std::move(previousPlayback);
	}
	const auto code = validateManagedVocalVariantAssociation(association);
	return code == QStringLiteral("ok")
		? ManagedVocalVariantAssociationResult{code, std::move(association)}
		: ManagedVocalVariantAssociationResult{code, {}};
}

} // namespace lmms
