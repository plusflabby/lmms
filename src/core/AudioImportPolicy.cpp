#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "SampleBuffer.h"
#include <QCoreApplication>
#include <QThread>
#include <QJsonDocument>
#include <QJsonArray>
#include <new>
namespace lmms {
namespace {
bool applicationThread() { return QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread(); }
QStringList settings()
{
	auto c = ConfigManager::inst();
	return {c->value("aibridge", "audio_import_roots"), c->value("aibridge", "audio_import_max_file_bytes"),
		c->value("aibridge", "audio_import_max_decoded_bytes")};
}
}
AudioImportPolicy::AudioImportPolicy()
{
	static auto slot = std::make_shared<Slot>();
	m_slot = slot;
}
AudioImportPolicy::~AudioImportPolicy() { revoke(); }
AudioImportPolicy::Prepared::~Prepared() { if (m_slot) m_slot->busy = false; }
bool AudioImportPolicy::Prepared::isCurrent() const
{
	return applicationThread() && m_epoch && m_epoch->active && m_snapshot == settings()
		&& m_revision == ConfigManager::inst()->audioImportPolicyRevision();
}
QString AudioImportPolicy::refresh()
{
	if (!applicationThread()) return QStringLiteral("unsupported_audio_operation");
	const auto current = settings();
	const auto revision = ConfigManager::inst()->audioImportPolicyRevision();
	if (m_epoch && current == m_snapshot && revision == m_revision && m_status == "ok") return m_status;
	revoke(); m_snapshot = current; m_revision = revision;
	m_status = QStringLiteral("import_root_not_configured");
	if (current[0].isEmpty()) return m_status;
	m_status = QStringLiteral("import_path_denied");
	if (current[0].size() > 65536) return m_status;
	const auto document = QJsonDocument::fromJson(current[0].toUtf8());
	if (!document.isArray() || document.array().isEmpty() || document.array().size() > 32) return m_status;
	const auto limits = SampleDecoder::configuredImportLimits();
	if (!limits.maxFileBytes || !limits.maxDecodedBytes) return m_status = QStringLiteral("audio_resource_limits_not_configured");
	std::vector<std::shared_ptr<AudioFileAuthorization::Root>> roots;
	for (const auto& value : document.array())
	{
		if (!value.isString() || value.toString().isEmpty()) return m_status;
		auto approval = AudioFileAuthorization::approveRoot(value.toString());
		if (!approval.root) return m_status;
		roots.emplace_back(std::move(approval.root));
	}
	m_roots = std::move(roots); m_epoch = std::make_shared<Epoch>();
	return m_status = QStringLiteral("ok");
}
AudioImportPolicy::Result AudioImportPolicy::prepare(const QString& path, const QByteArray& expectedDigest, const QByteArray& expectedIdentity)
{
	const auto status = refresh();
	if (status != "ok") return {status, {}};
	if (m_slot->busy) return {QStringLiteral("audio_import_busy"), {}};
	AudioFileAuthorization::Result lease;
	QString error = QStringLiteral("import_path_denied");
	for (const auto& root : m_roots)
	{
		lease = AudioFileAuthorization::acquire(*root, path);
		if (lease.source) break;
		if (lease.code == "audio_source_missing") error = lease.code;
	}
	if (!lease.source) return {error, {}};
	if (!expectedIdentity.isEmpty() && lease.identity != expectedIdentity) return {QStringLiteral("audio_source_changed"), {}};
	auto decoded = SampleDecoder::decodePcmWave(*lease.source, SampleDecoder::configuredImportLimits());
	if (!decoded.sample) return {decoded.code, {}};
	if (!expectedDigest.isEmpty() && decoded.info.sourceSha256 != expectedDigest) return {QStringLiteral("audio_source_changed"), {}};
	try
	{
		auto audio = std::unique_ptr<Prepared>(new Prepared);
		audio->m_buffer = std::make_shared<const SampleBuffer>(std::move(decoded.sample->data), decoded.sample->sampleRate, path);
		audio->m_info = decoded.info; audio->m_path = path; audio->m_lease = std::move(lease);
		audio->m_epoch = m_epoch; audio->m_snapshot = m_snapshot; audio->m_slot = m_slot; audio->m_revision = m_revision;
		m_slot->busy = true;
		return {QStringLiteral("ok"), std::move(audio)};
	}
	catch (const std::bad_alloc&) { return {QStringLiteral("source_too_large"), {}}; }
}
AudioImportPolicy& AudioImportPolicy::configured() { static AudioImportPolicy policy; return policy; }
void AudioImportPolicy::revoke()
{
	if (m_epoch) m_epoch->active = false;
	for (auto& root : m_roots) root->revoke();
	m_roots.clear(); m_epoch.reset();
}
}
