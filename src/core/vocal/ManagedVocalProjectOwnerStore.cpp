#include "ManagedVocalProjectOwnerStore.h"
#include "ManagedVocalAssetFileOps.h"
#include "AudioFileAuthorization.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <algorithm>
#include <array>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace lmms {
namespace {
constexpr qint64 MaxProjectBytes = 64LL * 1024 * 1024;
constexpr qint64 MaxRecordBytes = 2048;
struct Snapshot
{
	QString fileId;
	QString digest;
	std::unique_ptr<QIODevice> pin;
	std::shared_ptr<managed_vocal_detail::FileState> rootPin;
};

std::optional<Snapshot> snapshot(const QString& path, const std::shared_ptr<ManagedVocalAssetRootState>& root)
{
#ifdef Q_OS_WIN
	if (path.isEmpty() || !QDir::isAbsolutePath(path)) return {};
	const auto full = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
	std::shared_ptr<managed_vocal_detail::FileState> rootFile;
	QByteArray rootFileId;
	const auto rootCode = managed_vocal_detail::openReadProjectInRoot(root, full, rootFile, rootFileId);
	if (rootCode == QStringLiteral("ok"))
	{
		const auto length = managed_vocal_detail::size(rootFile);
		if (rootFileId.size() != 32 || length < 0 || length > MaxProjectBytes) return {};
		QCryptographicHash hash(QCryptographicHash::Sha256);
		std::array<char, 65536> block{};
		qint64 consumed = 0;
		while (consumed < length)
		{
			const auto count = managed_vocal_detail::read(rootFile, block.data(), std::min<qint64>(block.size(), length - consumed));
			if (count <= 0) return {};
			hash.addData(block.data(), int(count));
			consumed += count;
		}
		if (managed_vocal_detail::size(rootFile) != length) return {};
		return Snapshot{QString::fromLatin1(rootFileId.toHex()), QString::fromLatin1(hash.result().toHex()), {}, std::move(rootFile)};
	}
	if (rootCode != QStringLiteral("project_not_in_root")) return {};
	const auto parent = QDir::fromNativeSeparators(QFileInfo(full).absolutePath());
	auto source = AudioFileAuthorization::acquire(parent, full);
	if (!source.source || source.identity.size() != 32) return {};
	const auto size = source.source->size();
	if (size < 0 || size > MaxProjectBytes) return {};
	// AudioFileAuthorization pins every non-reparse ancestor and the file.
	// Verify link count and identity on a second read-only handle while pinned.
	const auto file = CreateFileW(reinterpret_cast<LPCWSTR>(full.utf16()), GENERIC_READ,
		FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE) return {};
	BY_HANDLE_FILE_INFORMATION info{};
	const bool valid = GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &info)
		&& !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
		&& info.nNumberOfLinks == 1;
	CloseHandle(file);
	if (!valid) return {};
	const auto material = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
		+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
	if (QCryptographicHash::hash(material, QCryptographicHash::Sha256) != source.identity) return {};
	QCryptographicHash hash(QCryptographicHash::Sha256);
	std::array<char, 65536> block{};
	qint64 consumed = 0;
	while (consumed < size)
	{
		const auto count = source.source->read(block.data(), std::min<qint64>(block.size(), size - consumed));
		if (count <= 0) return {};
		hash.addData(block.data(), int(count));
		consumed += count;
	}
	if (source.source->size() != size) return {};
	return Snapshot{QString::fromLatin1(source.identity.toHex()), QString::fromLatin1(hash.result().toHex()),
		std::move(source.source), {}};
#else
	Q_UNUSED(path); Q_UNUSED(root);
	return {};
#endif
}

QString recordName(const Snapshot& state)
{
	const auto key = QCryptographicHash::hash(QByteArrayLiteral("lmms-vocal-project-binding-v1:")
		+ state.fileId.toLatin1() + ':' + state.digest.toLatin1(), QCryptographicHash::Sha256).toHex();
	return QStringLiteral("vocal-project-") + QString::fromLatin1(key) + QStringLiteral(".json");
}

ManagedVocalProjectOwnerStore::Result readBinding(const std::shared_ptr<ManagedVocalAssetRootState>& root,
	const QString& rootId, const Snapshot& state)
{
	using namespace managed_vocal_detail;
	std::shared_ptr<FileState> file;
	const auto code = openReadFile(root, recordName(state), file);
	if (code == QStringLiteral("asset_missing")) return {QStringLiteral("project_identity_unverified"), {}};
	if (code != QStringLiteral("ok")) return {code, {}};
	const auto bytes = size(file);
	if (bytes <= 0 || bytes > MaxRecordBytes) return {QStringLiteral("asset_integrity_failed"), {}};
	QByteArray json(int(bytes), Qt::Uninitialized);
	if (read(file, json.data(), bytes) != bytes) return {QStringLiteral("asset_integrity_failed"), {}};
	QJsonParseError error{};
	const auto doc = QJsonDocument::fromJson(json, &error);
	const auto object = doc.object();
	const auto projectId = object.value(QStringLiteral("project_id")).toString();
	const auto uuid = QUuid(projectId);
	if (error.error != QJsonParseError::NoError || !doc.isObject() || object.size() != 5
		|| QJsonDocument(object).toJson(QJsonDocument::Compact) != json
		|| object.value(QStringLiteral("schema")) != QJsonValue(1)
		|| object.value(QStringLiteral("root_id")) != QJsonValue(rootId)
		|| object.value(QStringLiteral("file_id")) != QJsonValue(state.fileId)
		|| object.value(QStringLiteral("document_sha256")) != QJsonValue(state.digest)
		|| uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != projectId)
		return {QStringLiteral("asset_integrity_failed"), {}};
	return {QStringLiteral("ok"), ManagedVocalProjectOwner{projectId, rootId, state.fileId, state.digest}};
}
}

ManagedVocalProjectOwnerStore::ManagedVocalProjectOwnerStore(std::shared_ptr<ManagedVocalAssetRoot> root)
	: m_root(std::move(root)) {}

ManagedVocalProjectOwnerStore::Result ManagedVocalProjectOwnerStore::lookup(const QString& projectPath) const
{
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	const auto current = snapshot(projectPath, m_root->m_state);
	if (!current) return {QStringLiteral("project_identity_unverified"), {}};
	return readBinding(m_root->m_state, m_root->rootId(), *current);
}

ManagedVocalProjectOwnerStore::Result ManagedVocalProjectOwnerStore::ensure(const QString& projectPath) const
{
	using namespace managed_vocal_detail;
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	const auto current = snapshot(projectPath, m_root->m_state);
	if (!current) return {QStringLiteral("project_identity_unverified"), {}};
	const auto rootId = m_root->rootId();
	const auto previous = readBinding(m_root->m_state, rootId, *current);
	if (previous.code != QStringLiteral("project_identity_unverified")) return previous;
	const auto projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const auto bytes = QJsonDocument(QJsonObject{{"schema",1},{"project_id",projectId},{"root_id",rootId},
		{"file_id",current->fileId},{"document_sha256",current->digest}}).toJson(QJsonDocument::Compact);
	std::shared_ptr<FileState> temporary;
	const auto stage = QStringLiteral("vocal-owner-stage-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
	const auto created = createNewFile(m_root->m_state, stage, MaxRecordBytes, temporary);
	if (created != QStringLiteral("ok")) return {created, {}};
	if (!write(temporary, bytes.constData(), bytes.size()) || !flush(temporary))
	{
		removeOwnedFile(temporary);
		return {QStringLiteral("asset_write_failed"), {}};
	}
	const auto published = renameNoReplace(temporary, recordName(*current));
	if (published != QStringLiteral("ok"))
	{
		removeOwnedFile(temporary);
		if (published != QStringLiteral("asset_exists")) return {published, {}};
	}
	temporary.reset();
	return readBinding(m_root->m_state, rootId, *current);
}

QString ManagedVocalProjectOwnerStore::verify(const QString& projectPath, const ManagedVocalProjectOwner& owner) const
{
	const auto found = lookup(projectPath);
	if (!found.owner) return found.code;
	return found.owner->projectId == owner.projectId && found.owner->rootId == owner.rootId
		&& found.owner->fileIdentity == owner.fileIdentity && found.owner->documentSha256 == owner.documentSha256
		? QStringLiteral("ok") : QStringLiteral("project_identity_unverified");
}
}
