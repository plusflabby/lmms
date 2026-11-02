#include "ManagedVocalAssetTypes.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
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
	return {
		{"schema_version", int(manifest.schemaVersion)},
		{"asset_id", manifest.assetId},
		{"asset_root_id", manifest.assetRootId},
		{"media", QJsonObject{
			{"frames", QString::number(manifest.media.frames)},
			{"sample_rate", manifest.media.sampleRate},
			{"channels", manifest.media.channels},
			{"pcm_sha256", manifest.media.pcmSha256},
			{"peak", manifest.media.peak},
			{"overrange", manifest.media.overrange}
		}},
		{"recipe_hash", manifest.recipeHash},
		{"renderer_id", manifest.rendererId},
		{"renderer_version", manifest.rendererVersion},
		{"renderer_options", manifest.rendererOptions},
		{"recipe", manifest.recipe},
		{"realized_frame_maps", manifest.realizedFrameMaps},
		{"sources", sources}
	};
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

} // namespace

QString validateManagedVocalManifest(const ManagedVocalManifest& manifest)
{
	if (manifest.schemaVersion != 1 || !uuid(manifest.assetId) || !digest(manifest.assetRootId)
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
	if (!exactKeys(object, {"schema_version", "asset_id", "asset_root_id", "media", "recipe_hash", "renderer_id",
			"renderer_version", "renderer_options", "recipe", "realized_frame_maps", "sources"}))
	{
		return {QStringLiteral("invalid_manifest"), {}};
	}
	ManagedVocalManifest manifest;
	int schemaVersion = 0;
	if (!integer(object["schema_version"], schemaVersion) || schemaVersion < 0
		|| !object["asset_id"].isString() || !object["asset_root_id"].isString()
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
	manifest.assetRootId = object["asset_root_id"].toString();
	manifest.recipeHash = object["recipe_hash"].toString();
	manifest.rendererId = object["renderer_id"].toString();
	manifest.rendererVersion = object["renderer_version"].toString();
	manifest.rendererOptions = object["renderer_options"].toObject();
	manifest.recipe = object["recipe"].toObject();
	manifest.realizedFrameMaps = object["realized_frame_maps"].toArray();

	const auto media = object["media"].toObject();
	if (!exactKeys(media, {"frames", "sample_rate", "channels", "pcm_sha256", "peak", "overrange"})
		|| !media["frames"].isString() || !media["pcm_sha256"].isString()
		|| !media["peak"].isDouble() || !media["overrange"].isBool()
		|| !parseUnsigned(media["frames"].toString(), manifest.media.frames)
		|| !integer(media["sample_rate"], manifest.media.sampleRate)
		|| !integer(media["channels"], manifest.media.channels))
	{
		return {QStringLiteral("invalid_manifest"), {}};
	}
	manifest.media.pcmSha256 = media["pcm_sha256"].toString();
	manifest.media.peak = media["peak"].toDouble();
	manifest.media.overrange = media["overrange"].toBool();

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

} // namespace lmms
