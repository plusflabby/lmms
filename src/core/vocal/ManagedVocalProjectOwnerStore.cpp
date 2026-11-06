#include "ManagedVocalProjectOwnerStore.h"
#include "ManagedVocalAssetFileOps.h"
#include "AudioFileAuthorization.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QUuid>
#include <algorithm>
#include <array>
#include <mutex>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#include <wincred.h>
#endif

namespace lmms {
namespace {
constexpr qint64 MaxProjectBytes = 64LL * 1024 * 1024;
constexpr qint64 MaxRecordBytes = 2048;
std::mutex ownerStoreMutex;
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

QString witnessName(const Snapshot& state)
{
	return QStringLiteral("vocal-owner-witness-") + recordName(state).mid(QStringLiteral("vocal-project-").size());
}

QString credentialName(const QString& rootId, const Snapshot& state)
{
	return QStringLiteral("LMMS/ManagedVocalOwner/v1/") + rootId + QLatin1Char('/') + recordName(state);
}

struct CredentialRead { QString code; QByteArray bytes; };
CredentialRead readCredential(const QString& name)
{
#ifdef Q_OS_WIN
	PCREDENTIALW credential = nullptr;
	if (!CredReadW(reinterpret_cast<LPCWSTR>(name.utf16()), CRED_TYPE_GENERIC, 0, &credential))
		return {GetLastError() == ERROR_NOT_FOUND ? QStringLiteral("asset_missing")
			: QStringLiteral("asset_integrity_failed"), {}};
	const auto release = qScopeGuard([&] { if (credential) CredFree(credential); });
	if (!credential || credential->Type != CRED_TYPE_GENERIC || !credential->CredentialBlob
		|| !credential->CredentialBlobSize || credential->CredentialBlobSize > DWORD(MaxRecordBytes))
		return {QStringLiteral("asset_integrity_failed"), {}};
	return {QStringLiteral("ok"), QByteArray(reinterpret_cast<const char*>(credential->CredentialBlob),
		int(credential->CredentialBlobSize))};
#else
	Q_UNUSED(name);
	return {QStringLiteral("unsupported_backend"), {}};
#endif
}

QString createCredential(const QString& name, const QByteArray& bytes)
{
#ifdef Q_OS_WIN
	const auto current = readCredential(name);
	if (current.code == QStringLiteral("ok")) return QStringLiteral("asset_exists");
	if (current.code != QStringLiteral("asset_missing") || bytes.isEmpty() || bytes.size() > MaxRecordBytes)
		return QStringLiteral("asset_write_failed");
	CREDENTIALW credential{};
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(name.utf16()));
	credential.CredentialBlobSize = DWORD(bytes.size());
	credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(bytes.constData()));
	credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	return CredWriteW(&credential, 0) ? QStringLiteral("ok") : QStringLiteral("asset_write_failed");
#else
	Q_UNUSED(name); Q_UNUSED(bytes);
	return QStringLiteral("unsupported_backend");
#endif
}

QByteArray protectionEntropy(const QString& rootId, const Snapshot& state, const QByteArray& purpose)
{
	return QCryptographicHash::hash(QByteArrayLiteral("lmms-vocal-project-protection-v1:")
		+ purpose + ':' + rootId.toLatin1() + ':' + state.fileId.toLatin1() + ':' + state.digest.toLatin1(),
		QCryptographicHash::Sha256);
}

std::optional<QByteArray> protectRecord(const QByteArray& data, const QByteArray& entropy)
{
#ifdef Q_OS_WIN
	DATA_BLOB input{DWORD(data.size()), reinterpret_cast<BYTE*>(const_cast<char*>(data.constData()))};
	DATA_BLOB extra{DWORD(entropy.size()), reinterpret_cast<BYTE*>(const_cast<char*>(entropy.constData()))};
	DATA_BLOB output{};
	if (!CryptProtectData(&input, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
	const auto release = qScopeGuard([&] { if (output.pbData) LocalFree(output.pbData); });
	if (!output.pbData || !output.cbData || output.cbData > DWORD(MaxRecordBytes)) return {};
	return QByteArray(reinterpret_cast<const char*>(output.pbData), int(output.cbData));
#else
	Q_UNUSED(data); Q_UNUSED(entropy);
	return {};
#endif
}

std::optional<QByteArray> unprotectRecord(const QByteArray& data, const QByteArray& entropy)
{
#ifdef Q_OS_WIN
	DATA_BLOB input{DWORD(data.size()), reinterpret_cast<BYTE*>(const_cast<char*>(data.constData()))};
	DATA_BLOB extra{DWORD(entropy.size()), reinterpret_cast<BYTE*>(const_cast<char*>(entropy.constData()))};
	DATA_BLOB output{};
	if (!CryptUnprotectData(&input, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
	const auto release = qScopeGuard([&] {
		if (output.pbData) { SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); }
	});
	if (!output.pbData || !output.cbData || output.cbData > DWORD(MaxRecordBytes)) return {};
	return QByteArray(reinterpret_cast<const char*>(output.pbData), int(output.cbData));
#else
	Q_UNUSED(data); Q_UNUSED(entropy);
	return {};
#endif
}

ManagedVocalProjectOwnerStore::Result readProtected(const std::shared_ptr<ManagedVocalAssetRootState>& root,
	const QString& rootId, const Snapshot& state, const QString& name, const QByteArray& purpose,
	QByteArray* encoded = nullptr)
{
	using namespace managed_vocal_detail;
	std::shared_ptr<FileState> file;
	const auto code = openReadFile(root, name, file);
	if (code != QStringLiteral("ok")) return {code, {}};
	const auto bytes = size(file);
	if (bytes <= 0 || bytes > MaxRecordBytes) return {QStringLiteral("asset_integrity_failed"), {}};
	QByteArray json(int(bytes), Qt::Uninitialized);
	if (read(file, json.data(), bytes) != bytes) return {QStringLiteral("asset_integrity_failed"), {}};
	if (encoded) *encoded = json;
	QJsonParseError error{};
	const auto envelope = QJsonDocument::fromJson(json, &error);
	const auto outer = envelope.object();
	const auto protectedText = outer.value(QStringLiteral("protected")).toString().toLatin1();
	const auto protectedBytes = QByteArray::fromBase64(protectedText);
	if (error.error != QJsonParseError::NoError || !envelope.isObject() || outer.size() != 2
		|| outer.value(QStringLiteral("schema")) != QJsonValue(2)
		|| !outer.value(QStringLiteral("protected")).isString() || protectedBytes.isEmpty()
		|| protectedBytes.toBase64() != protectedText
		|| QJsonDocument(outer).toJson(QJsonDocument::Compact) != json)
		return {QStringLiteral("asset_integrity_failed"), {}};
	const auto plaintext = unprotectRecord(protectedBytes, protectionEntropy(rootId, state, purpose));
	if (!plaintext) return {QStringLiteral("asset_integrity_failed"), {}};
	const auto doc = QJsonDocument::fromJson(*plaintext, &error);
	const auto object = doc.object();
	const auto projectId = object.value(QStringLiteral("project_id")).toString();
	const auto uuid = QUuid(projectId);
	if (error.error != QJsonParseError::NoError || !doc.isObject() || object.size() != 5
		|| QJsonDocument(object).toJson(QJsonDocument::Compact) != *plaintext
		|| object.value(QStringLiteral("schema")) != QJsonValue(1)
		|| object.value(QStringLiteral("root_id")) != QJsonValue(rootId)
		|| object.value(QStringLiteral("file_id")) != QJsonValue(state.fileId)
		|| object.value(QStringLiteral("document_sha256")) != QJsonValue(state.digest)
		|| uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != projectId)
		return {QStringLiteral("asset_integrity_failed"), {}};
	return {QStringLiteral("ok"), ManagedVocalProjectOwner{projectId, rootId, state.fileId, state.digest}};
}

ManagedVocalProjectOwnerStore::Result readBinding(const std::shared_ptr<ManagedVocalAssetRootState>& root,
	const QString& rootId, const Snapshot& state)
{
	const auto binding = readProtected(root, rootId, state, recordName(state), QByteArrayLiteral("binding"));
	QByteArray witnessBytes;
	const auto witness = readProtected(root, rootId, state, witnessName(state), QByteArrayLiteral("witness"), &witnessBytes);
	const auto credential = readCredential(credentialName(rootId, state));
	if (credential.code != QStringLiteral("ok") && credential.code != QStringLiteral("asset_missing"))
		return {credential.code, {}};
	if (binding.code == QStringLiteral("asset_missing") && witness.code == QStringLiteral("asset_missing")
		&& credential.code == QStringLiteral("asset_missing"))
		return {QStringLiteral("project_identity_unverified"), {}};
	if (binding.code == QStringLiteral("asset_missing") || witness.code == QStringLiteral("asset_missing")
		|| credential.code == QStringLiteral("asset_missing"))
		return {QStringLiteral("asset_incomplete"), {}};
	if (!binding.owner) return binding;
	if (!witness.owner) return witness;
	if (credential.bytes != witnessBytes || binding.owner->projectId != witness.owner->projectId
		|| binding.owner->rootId != witness.owner->rootId
		|| binding.owner->fileIdentity != witness.owner->fileIdentity
		|| binding.owner->documentSha256 != witness.owner->documentSha256)
		return {QStringLiteral("asset_integrity_failed"), {}};
	return binding;
}

QString publishRecord(const std::shared_ptr<ManagedVocalAssetRootState>& root, const QString& name, const QByteArray& bytes)
{
	using namespace managed_vocal_detail;
	if (bytes.size() > MaxRecordBytes) return QStringLiteral("asset_write_failed");
	std::shared_ptr<FileState> temporary;
	const auto stage = QStringLiteral("vocal-owner-stage-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
	const auto created = createNewFile(root, stage, MaxRecordBytes, temporary);
	if (created != QStringLiteral("ok")) return created;
	if (!write(temporary, bytes.constData(), bytes.size()) || !flush(temporary))
	{
		removeOwnedFile(temporary);
		return QStringLiteral("asset_write_failed");
	}
	const auto published = renameNoReplace(temporary, name);
	if (published != QStringLiteral("ok")) removeOwnedFile(temporary);
	return published;
}
}

ManagedVocalProjectOwnerStore::ManagedVocalProjectOwnerStore(std::shared_ptr<ManagedVocalAssetRoot> root)
	: m_root(std::move(root)) {}

ManagedVocalProjectOwnerStore::Result ManagedVocalProjectOwnerStore::lookup(const QString& projectPath) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	const auto current = snapshot(projectPath, m_root->m_state);
	if (!current) return {QStringLiteral("project_identity_unverified"), {}};
	return readBinding(m_root->m_state, m_root->rootId(), *current);
}

ManagedVocalProjectOwnerStore::Result ManagedVocalProjectOwnerStore::ensure(const QString& projectPath) const
{
	using namespace managed_vocal_detail;
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	const auto current = snapshot(projectPath, m_root->m_state);
	if (!current) return {QStringLiteral("project_identity_unverified"), {}};
	const auto rootId = m_root->rootId();
	const auto previous = readBinding(m_root->m_state, rootId, *current);
	if (previous.code != QStringLiteral("project_identity_unverified")) return previous;
	const auto projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const auto plaintext = QJsonDocument(QJsonObject{{"schema",1},{"project_id",projectId},{"root_id",rootId},
		{"file_id",current->fileId},{"document_sha256",current->digest}}).toJson(QJsonDocument::Compact);
	const auto binding = protectRecord(plaintext, protectionEntropy(rootId, *current, QByteArrayLiteral("binding")));
	const auto witness = protectRecord(plaintext, protectionEntropy(rootId, *current, QByteArrayLiteral("witness")));
	if (!binding || !witness) return {QStringLiteral("asset_write_failed"), {}};
	const auto envelope = [](const QByteArray& value) {
		return QJsonDocument(QJsonObject{{"schema",2},{"protected",QString::fromLatin1(value.toBase64())}})
			.toJson(QJsonDocument::Compact);
	};
	const auto witnessBytes = envelope(*witness);
	// The user-bound credential is created first. If either root publication is
	// interrupted, a later lookup fails closed instead of minting another owner.
	const auto credentialCode = createCredential(credentialName(rootId, *current), witnessBytes);
	if (credentialCode != QStringLiteral("ok")) return {credentialCode, {}};
	const auto witnessCode = publishRecord(m_root->m_state, witnessName(*current), witnessBytes);
	if (witnessCode != QStringLiteral("ok"))
		return witnessCode == QStringLiteral("asset_exists")
			? readBinding(m_root->m_state, rootId, *current) : Result{witnessCode, {}};
	const auto bindingCode = publishRecord(m_root->m_state, recordName(*current), envelope(*binding));
	if (bindingCode != QStringLiteral("ok")) return {bindingCode, {}};
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
