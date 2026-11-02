#include "ManagedVocalAssetStore.h"

#include "ManagedVocalAssetFileOps.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QtEndian>
#include <QUuid>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <new>

namespace lmms {
namespace {

constexpr quint64 DefaultMaximumManifestBytes = 16ULL * 1024 * 1024;
constexpr quint64 MaximumAppendSamples = 16384;

void append16(QByteArray& bytes, quint16 value)
{
	const auto start = bytes.size();
	bytes.resize(start + 2);
	qToLittleEndian(value, reinterpret_cast<uchar*>(bytes.data() + start));
}

void append32(QByteArray& bytes, quint32 value)
{
	const auto start = bytes.size();
	bytes.resize(start + 4);
	qToLittleEndian(value, reinterpret_cast<uchar*>(bytes.data() + start));
}

QByteArray floatWaveHeader(quint64 frames, int sampleRate, int channels)
{
	const quint64 dataBytes = frames * quint64(channels) * sizeof(float);
	QByteArray header;
	header.reserve(56);
	header.append("RIFF", 4);
	append32(header, quint32(48 + dataBytes));
	header.append("WAVEfmt ", 8);
	append32(header, 16);
	append16(header, 3); // IEEE float
	append16(header, quint16(channels));
	append32(header, quint32(sampleRate));
	append32(header, quint32(sampleRate * channels * int(sizeof(float))));
	append16(header, quint16(channels * int(sizeof(float))));
	append16(header, 32);
	header.append("fact", 4);
	append32(header, 4);
	append32(header, quint32(frames));
	header.append("data", 4);
	append32(header, quint32(dataBytes));
	return header;
}

QString assetStem(const QString& assetId)
{
	return QStringLiteral("vocal-asset-") + assetId;
}

bool validReference(const ManagedVocalVariantReference& reference)
{
	const auto id = QUuid(reference.assetId);
	return !id.isNull() && id.toString(QUuid::WithoutBraces) == reference.assetId
		&& reference.assetRootId.size() == 64 && reference.mediaSha256.size() == 64
		&& reference.manifestSha256.size() == 64
		&& std::all_of(reference.assetRootId.begin(), reference.assetRootId.end(), [](QChar ch) {
			return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
		})
		&& std::all_of(reference.mediaSha256.begin(), reference.mediaSha256.end(), [](QChar ch) {
			return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
		})
		&& std::all_of(reference.manifestSha256.begin(), reference.manifestSha256.end(), [](QChar ch) {
			return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
		})
		&& reference.frames > 0 && (reference.sampleRate == 44100 || reference.sampleRate == 48000)
		&& (reference.channels == 1 || reference.channels == 2)
		&& reference.frames <= quint64(reference.sampleRate) * 120;
}

class HandleDevice final : public QIODevice
{
public:
	explicit HandleDevice(std::shared_ptr<managed_vocal_detail::FileState> file)
		: m_file(std::move(file))
	{
		open(QIODevice::ReadOnly | QIODevice::Unbuffered);
	}

	qint64 size() const override
	{
		return managed_vocal_detail::size(m_file);
	}

	bool seek(qint64 position) override
	{
		return managed_vocal_detail::seek(m_file, position) && QIODevice::seek(position);
	}

protected:
	qint64 readData(char* data, qint64 length) override
	{
		return managed_vocal_detail::read(m_file, data, length);
	}

	qint64 writeData(const char*, qint64) override
	{
		return -1;
	}

private:
	std::shared_ptr<managed_vocal_detail::FileState> m_file;
};

QString readBoundedFile(
	const std::shared_ptr<managed_vocal_detail::FileState>& file,
	quint64 maximumBytes,
	QByteArray& bytes)
{
	const auto length = managed_vocal_detail::size(file);
	if (length < 0 || quint64(length) > maximumBytes || length > std::numeric_limits<int>::max())
	{
		return QStringLiteral("resource_limit");
	}
	HandleDevice source(std::move(file));
	if (!source.seek(0))
	{
		return QStringLiteral("asset_integrity_failed");
	}
	try
	{
		bytes.resize(int(length));
	}
	catch (const std::bad_alloc&)
	{
		return QStringLiteral("resource_limit");
	}
	qint64 total = 0;
	while (total < length)
	{
		const auto count = source.read(bytes.data() + total, std::min<qint64>(length - total, 65536));
		if (count <= 0)
		{
			return QStringLiteral("asset_integrity_failed");
		}
		total += count;
	}
	return QStringLiteral("ok");
}

ManagedVocalAssetOpenResult failedOpen(const QString& code)
{
	return {code, {}, {}, {}};
}

} // namespace

struct ManagedVocalAssetWriter::State
{
	std::shared_ptr<ManagedVocalAssetRoot> root;
	std::shared_ptr<managed_vocal_detail::FileState> file;
	std::unique_ptr<QCryptographicHash> pcmHash;
	QString id;
	quint64 expected = 0;
	quint64 written = 0;
	quint64 maximumBytes = 0;
	int rate = 0;
	int channels = 0;
	double peak = 0.0;
	bool overrange = false;
	bool failed = false;
	bool published = false;
};

ManagedVocalAssetWriter::ManagedVocalAssetWriter(std::unique_ptr<State> state)
	: m_state(std::move(state))
{
}

ManagedVocalAssetWriter::~ManagedVocalAssetWriter()
{
	if (m_state && !m_state->published && m_state->file)
	{
		managed_vocal_detail::removeOwnedFile(m_state->file);
	}
}

QString ManagedVocalAssetWriter::assetId() const
{
	return m_state ? m_state->id : QString{};
}

QString ManagedVocalAssetWriter::appendFrames(std::span<const float> samples)
{
	if (!m_state || m_state->failed || !m_state->root || !m_state->root->isAuthorized())
	{
		return QStringLiteral("asset_root_revoked");
	}
	if (samples.empty() || samples.size() % std::size_t(m_state->channels) != 0)
	{
		return QStringLiteral("invalid_parameter");
	}
	if (samples.size() > MaximumAppendSamples)
	{
		return QStringLiteral("resource_limit");
	}
	const auto frames = quint64(samples.size() / std::size_t(m_state->channels));
	if (frames > m_state->expected - std::min(m_state->expected, m_state->written))
	{
		return QStringLiteral("resource_limit");
	}
	for (const auto sample : samples)
	{
		if (!std::isfinite(sample))
		{
			return QStringLiteral("invalid_audio_samples");
		}
	}

	QByteArray encoded(int(samples.size() * sizeof(float)), Qt::Uninitialized);
	for (std::size_t i = 0; i < samples.size(); ++i)
	{
		qToLittleEndian(std::bit_cast<quint32>(samples[i]),
			reinterpret_cast<uchar*>(encoded.data() + i * sizeof(float)));
	}
	if (!managed_vocal_detail::write(m_state->file, encoded.constData(), encoded.size()))
	{
		m_state->failed = true;
		return QStringLiteral("asset_write_failed");
	}
	m_state->pcmHash->addData(encoded);
	for (const auto sample : samples)
	{
		const auto magnitude = std::abs(double(sample));
		m_state->peak = std::max(m_state->peak, magnitude);
		m_state->overrange = m_state->overrange || magnitude > 1.0;
	}
	m_state->written += frames;
	return QStringLiteral("ok");
}

quint64 ManagedVocalAssetWriter::framesWritten() const
{
	return m_state ? m_state->written : 0;
}

quint64 ManagedVocalAssetWriter::expectedFrames() const
{
	return m_state ? m_state->expected : 0;
}

ManagedVocalAssetStore::ManagedVocalAssetStore(
	std::shared_ptr<ManagedVocalAssetRoot> root,
	quint64 maximumStagedBytes)
	: m_root(std::move(root)), m_maximumStagedBytes(maximumStagedBytes)
{
}

ManagedVocalAssetCreateResult ManagedVocalAssetStore::beginCreate(
	quint64 frames,
	int sampleRate,
	int channels)
{
	if (!m_root)
	{
		return {QStringLiteral("asset_root_not_configured"), {}, {}};
	}
	if (!m_root->isAuthorized())
	{
		return {QStringLiteral("asset_root_revoked"), {}, {}};
	}
	if (!frames || (sampleRate != 44100 && sampleRate != 48000)
		|| (channels != 1 && channels != 2))
	{
		return {QStringLiteral("invalid_parameter"), {}, {}};
	}
	if (frames > quint64(sampleRate) * 120
		|| frames > (std::numeric_limits<quint64>::max() - 56) / quint64(channels * int(sizeof(float))))
	{
		return {QStringLiteral("resource_limit"), {}, {}};
	}
	const auto dataBytes = frames * quint64(channels * int(sizeof(float)));
	const auto totalBytes = dataBytes + 56;
	if (totalBytes > m_maximumStagedBytes || dataBytes > std::numeric_limits<quint32>::max() - 48)
	{
		return {QStringLiteral("resource_limit"), {}, {}};
	}

	const auto assetId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const auto stagingName = assetStem(assetId) + QStringLiteral(".wav.partial");
	std::shared_ptr<managed_vocal_detail::FileState> file;
	const auto created = managed_vocal_detail::createNewFile(m_root->m_state, stagingName, totalBytes, file);
	if (created != QStringLiteral("ok"))
	{
		return {created, {}, {}};
	}
	const auto header = floatWaveHeader(frames, sampleRate, channels);
	if (header.size() != 56 || !managed_vocal_detail::write(file, header.constData(), header.size())
		|| !managed_vocal_detail::flush(file))
	{
		managed_vocal_detail::removeOwnedFile(file);
		return {QStringLiteral("asset_write_failed"), {}, {}};
	}

	auto state = std::make_unique<ManagedVocalAssetWriter::State>();
	state->root = m_root;
	state->file = std::move(file);
	state->pcmHash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
	state->id = assetId;
	state->expected = frames;
	state->maximumBytes = totalBytes;
	state->rate = sampleRate;
	state->channels = channels;
	return {QStringLiteral("ok"), assetId,
		std::unique_ptr<ManagedVocalAssetWriter>(new ManagedVocalAssetWriter(std::move(state)))};
}

ManagedVocalAssetPublishResult ManagedVocalAssetStore::publish(
	std::unique_ptr<ManagedVocalAssetWriter> writer,
	ManagedVocalManifest manifest)
{
	if (!writer || !writer->m_state || !m_root || !m_root->isAuthorized())
	{
		return {QStringLiteral("asset_root_revoked"), {}};
	}
	auto& state = *writer->m_state;
	if (state.root.get() != m_root.get() || state.failed || state.written != state.expected)
	{
		return {QStringLiteral("asset_incomplete"), {}};
	}
	if (!managed_vocal_detail::flush(state.file))
	{
		return {QStringLiteral("asset_write_failed"), {}};
	}

	HandleDevice source(state.file);
	const ManagedVocalWaveLimits waveLimits{m_maximumStagedBytes, m_maximumStagedBytes,
		quint64(state.rate) * 120};
	const auto verified = ManagedVocalWaveReader::read(source, waveLimits, false);
	const auto pcmSha256 = QString::fromLatin1(state.pcmHash->result().toHex());
	if (verified.code != QStringLiteral("ok") || verified.info.frames != state.expected
		|| verified.info.sampleRate != state.rate || verified.info.channels != state.channels
		|| verified.info.pcmSha256 != pcmSha256 || verified.info.peak != state.peak
		|| verified.info.overrange != state.overrange
		|| (manifest.media.rmsKnown && std::abs(verified.info.rms - manifest.media.rms) > 1e-12))
	{
		return {QStringLiteral("asset_integrity_failed"), {}};
	}

	manifest.assetId = state.id;
	manifest.assetRootId = m_root->rootId();
	manifest.media.frames = state.expected;
	manifest.media.sampleRate = state.rate;
	manifest.media.channels = state.channels;
	manifest.media.pcmSha256 = pcmSha256;
	manifest.media.peak = state.peak;
	manifest.media.overrange = state.overrange;
	const auto encodedManifest = encodeManagedVocalManifest(manifest);
	if (encodedManifest.isEmpty())
	{
		return {QStringLiteral("invalid_manifest"), {}};
	}
	auto reference = makeManagedVocalVariantReference(manifest, encodedManifest);
	if (reference.code != QStringLiteral("ok") || !reference.value)
	{
		return {reference.code, {}};
	}

	const auto manifestStageName = assetStem(state.id) + QStringLiteral(".json.partial");
	std::shared_ptr<managed_vocal_detail::FileState> manifestFile;
	const auto created = managed_vocal_detail::createNewFile(m_root->m_state, manifestStageName,
		quint64(encodedManifest.size()), manifestFile);
	if (created != QStringLiteral("ok"))
	{
		return {created, {}};
	}
	bool retainManifest = false;
	const auto cleanupManifest = [&] {
		if (!retainManifest && manifestFile)
		{
			managed_vocal_detail::removeOwnedFile(manifestFile);
		}
	};
	if (!managed_vocal_detail::write(manifestFile, encodedManifest.constData(), encodedManifest.size())
		|| !managed_vocal_detail::flush(manifestFile))
	{
		cleanupManifest();
		return {QStringLiteral("asset_write_failed"), {}};
	}
	QByteArray readback;
	const auto readCode = readBoundedFile(manifestFile, DefaultMaximumManifestBytes, readback);
	if (readCode != QStringLiteral("ok") || readback != encodedManifest
		|| decodeManagedVocalManifest(readback).code != QStringLiteral("ok"))
	{
		cleanupManifest();
		return {QStringLiteral("asset_integrity_failed"), {}};
	}

	const auto audioFinalName = assetStem(state.id) + QStringLiteral(".wav");
	const auto manifestFinalName = assetStem(state.id) + QStringLiteral(".json");
	const auto audioPublished = managed_vocal_detail::renameNoReplace(state.file, audioFinalName);
	if (audioPublished != QStringLiteral("ok"))
	{
		cleanupManifest();
		return {audioPublished == QStringLiteral("asset_exists") ? audioPublished
			: QStringLiteral("asset_audio_publish_failed"), {}};
	}
	const auto manifestPublished = managed_vocal_detail::renameNoReplace(manifestFile, manifestFinalName);
	if (manifestPublished != QStringLiteral("ok"))
	{
		managed_vocal_detail::removeOwnedFile(state.file);
		cleanupManifest();
		return {manifestPublished == QStringLiteral("asset_exists") ? manifestPublished
			: QStringLiteral("asset_manifest_publish_failed"), {}};
	}
	retainManifest = true;
	state.published = true;
	return {QStringLiteral("ok"), std::move(reference.value)};
}

ManagedVocalAssetOpenResult ManagedVocalAssetStore::readVariant(
	const ManagedVocalVariantReference& reference,
	bool retainSamples) const
{
	if (!m_root)
	{
		return failedOpen(QStringLiteral("asset_root_not_configured"));
	}
	if (!m_root->isAuthorized())
	{
		return failedOpen(QStringLiteral("asset_root_revoked"));
	}
	if (!validReference(reference))
	{
		return failedOpen(QStringLiteral("invalid_parameter"));
	}
	QByteArray manifestBytes;
	const auto manifestName = assetStem(reference.assetId) + QStringLiteral(".json");
	std::shared_ptr<managed_vocal_detail::FileState> manifestFile;
	const auto manifestOpenCode = managed_vocal_detail::openReadFile(m_root->m_state, manifestName, manifestFile);
	if (manifestOpenCode != QStringLiteral("ok"))
	{
		return failedOpen(manifestOpenCode);
	}
	const auto readCode = readBoundedFile(manifestFile, DefaultMaximumManifestBytes, manifestBytes);
	if (readCode != QStringLiteral("ok"))
	{
		return failedOpen(readCode);
	}
	const auto manifestHash = QString::fromLatin1(
		QCryptographicHash::hash(manifestBytes, QCryptographicHash::Sha256).toHex());
	if (manifestHash != reference.manifestSha256)
	{
		return failedOpen(QStringLiteral("asset_integrity_failed"));
	}
	const auto parsed = decodeManagedVocalManifest(manifestBytes);
	if (parsed.code != QStringLiteral("ok") || !parsed.value)
	{
		return failedOpen(QStringLiteral("asset_integrity_failed"));
	}
	const auto& manifest = *parsed.value;
	if (manifest.assetId != reference.assetId || manifest.media.pcmSha256 != reference.mediaSha256
		|| manifest.assetRootId != reference.assetRootId || reference.assetRootId != m_root->rootId()
		|| manifest.media.frames != reference.frames || manifest.media.sampleRate != reference.sampleRate
		|| manifest.media.channels != reference.channels)
	{
		return failedOpen(QStringLiteral("asset_integrity_failed"));
	}

	const auto audioName = assetStem(reference.assetId) + QStringLiteral(".wav");
	std::shared_ptr<managed_vocal_detail::FileState> audioFile;
	const auto openCode = managed_vocal_detail::openReadFile(m_root->m_state, audioName, audioFile);
	if (openCode != QStringLiteral("ok"))
	{
		return failedOpen(QStringLiteral("asset_integrity_failed"));
	}
	HandleDevice audio(std::move(audioFile));
	const ManagedVocalWaveLimits limits{m_maximumStagedBytes, m_maximumStagedBytes,
		quint64(reference.sampleRate) * 120};
	const auto readResult = ManagedVocalWaveReader::read(audio, limits, retainSamples);
	if (readResult.code != QStringLiteral("ok") || readResult.info.frames != reference.frames
		|| readResult.info.sampleRate != reference.sampleRate || readResult.info.channels != reference.channels
		|| readResult.info.pcmSha256 != reference.mediaSha256
		|| (manifest.media.rmsKnown && std::abs(readResult.info.rms - manifest.media.rms) > 1e-12))
	{
		return failedOpen(readResult.code == QStringLiteral("resource_limit")
			? QStringLiteral("resource_limit") : QStringLiteral("asset_integrity_failed"));
	}
	return {QStringLiteral("ok"), manifest, readResult.info, readResult.samples};
}

ManagedVocalAssetInspectResult ManagedVocalAssetStore::inspect(
	const ManagedVocalVariantReference& reference) const
{
	const auto result = readVariant(reference, false);
	if (result.code != QStringLiteral("ok") || !result.manifest)
	{
		return {result.code};
	}
	ManagedVocalAssetInspectResult summary;
	summary.code = QStringLiteral("ok");
	summary.projectId = result.manifest->projectId;
	summary.assetId = reference.assetId;
	summary.mediaSha256 = reference.mediaSha256;
	summary.manifestSha256 = reference.manifestSha256;
	summary.frames = reference.frames;
	summary.sampleRate = reference.sampleRate;
	summary.channels = reference.channels;
	summary.media = result.manifest->media;
	summary.recipeHash = result.manifest->recipeHash;
	summary.rendererId = result.manifest->rendererId;
	summary.rendererVersion = result.manifest->rendererVersion;
	summary.sourceCount = result.manifest->sources.size();
	summary.reference = reference;
	summary.sources = result.manifest->sources;
	return summary;
}

ManagedVocalAssetInspectResult ManagedVocalAssetStore::inspectById(
	const QString& assetId,
	const QString& expectedProjectId) const
{
	if (!m_root) return {QStringLiteral("asset_root_not_configured")};
	if (!m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked")};
	const auto parsedId = QUuid(assetId);
	if (parsedId.isNull() || parsedId.toString(QUuid::WithoutBraces) != assetId)
		return {QStringLiteral("invalid_parameter")};
	QByteArray manifestBytes;
	std::shared_ptr<managed_vocal_detail::FileState> manifestFile;
	const auto openCode = managed_vocal_detail::openReadFile(m_root->m_state,
		assetStem(assetId) + QStringLiteral(".json"), manifestFile);
	if (openCode != QStringLiteral("ok"))
		return {openCode == QStringLiteral("asset_missing") ? openCode : QStringLiteral("asset_integrity_failed")};
	if (readBoundedFile(manifestFile, DefaultMaximumManifestBytes, manifestBytes) != QStringLiteral("ok"))
		return {QStringLiteral("asset_integrity_failed")};
	const auto parsedManifest = decodeManagedVocalManifest(manifestBytes);
	if (parsedManifest.code != QStringLiteral("ok") || !parsedManifest.value
		|| parsedManifest.value->assetId != assetId
		|| parsedManifest.value->assetRootId != m_root->rootId())
		return {QStringLiteral("asset_integrity_failed")};
	if (!expectedProjectId.isEmpty() && parsedManifest.value->projectId != expectedProjectId)
		return {QStringLiteral("asset_project_mismatch")};
	const auto reference = makeManagedVocalVariantReference(*parsedManifest.value, manifestBytes);
	if (!reference.value) return {QStringLiteral("asset_integrity_failed")};
	return inspect(*reference.value);
}

ManagedVocalAssetOpenResult ManagedVocalAssetStore::openVerified(
	const ManagedVocalVariantReference& reference) const
{
	return readVariant(reference, true);
}

ManagedVocalAssetRecoveryScanResult ManagedVocalAssetStore::scanRecovery(size_t limit) const
{
	if (!m_root)
	{
		return {QStringLiteral("asset_root_not_configured"), {}, 0, false};
	}
	if (!m_root->isAuthorized())
	{
		return {QStringLiteral("asset_root_revoked"), {}, 0, false};
	}
	if (limit == 0 || limit > 128)
	{
		return {QStringLiteral("resource_limit"), {}, 0, false};
	}
	std::vector<managed_vocal_detail::RootEntry> entries;
	const auto listed = managed_vocal_detail::listRootEntries(m_root->m_state, entries, 100000);
	if (listed != QStringLiteral("ok"))
	{
		return {listed, {}, 0, false};
	}

	const auto prefix = QStringLiteral("vocal-asset-");
	std::map<QString, ManagedVocalAssetRecoveryItem> candidates;
	for (const auto& entry : entries)
	{
		if (!entry.name.startsWith(prefix))
		{
			continue;
		}
		struct NameKind { QString suffix; int kind; };
		const std::array<NameKind, 4> nameKinds{{
			{QStringLiteral(".wav.partial"), 0},
			{QStringLiteral(".json.partial"), 1},
			{QStringLiteral(".wav"), 2},
			{QStringLiteral(".json"), 3}
		}};
		const NameKind* matched = nullptr;
		for (const auto& candidate : nameKinds)
		{
			if (entry.name.endsWith(candidate.suffix))
			{
				matched = &candidate;
				break;
			}
		}
		if (!matched)
		{
			continue;
		}
		const auto assetId = entry.name.mid(prefix.size(),
			entry.name.size() - prefix.size() - matched->suffix.size());
		const auto parsedId = QUuid(assetId);
		if (parsedId.isNull() || parsedId.toString(QUuid::WithoutBraces) != assetId)
		{
			continue;
		}
		if (entry.directory || entry.reparse)
		{
			return {QStringLiteral("asset_integrity_failed"), {}, 0, false};
		}
		auto& item = candidates[assetId];
		item.assetId = assetId;
		switch (matched->kind)
		{
		case 0:
			item.stagedAudioPresent = true;
			item.stagedAudioBytes = entry.bytes;
			break;
		case 1:
			item.stagedManifestPresent = true;
			item.stagedManifestBytes = entry.bytes;
			break;
		case 2:
			item.finalAudioPresent = true;
			item.finalAudioBytes = entry.bytes;
			break;
		case 3:
			item.finalManifestPresent = true;
			item.finalManifestBytes = entry.bytes;
			break;
		}
	}

	ManagedVocalAssetRecoveryScanResult result;
	result.code = QStringLiteral("ok");
	for (auto& [assetId, item] : candidates)
	{
		Q_UNUSED(assetId);
		if (!item.stagedAudioPresent && !item.stagedManifestPresent
			&& item.finalAudioPresent && item.finalManifestPresent)
		{
			continue;
		}
		item.state = item.stagedAudioPresent || item.stagedManifestPresent
			? QStringLiteral("staging_recovery_required")
			: QStringLiteral("publication_recovery_required");
		++result.totalItems;
		if (result.items.size() < limit)
		{
			result.items.push_back(std::move(item));
		}
		else
		{
			result.truncated = true;
		}
	}
	return result;
}

} // namespace lmms
