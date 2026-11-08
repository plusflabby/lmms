#include "ManagedVocalProjectOwnerStore.h"
#include "ManagedVocalAssetFileOps.h"
#include "AudioFileAuthorization.h"
#include "ProjectSavePublication.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QUuid>
#include <algorithm>
#include <array>
#include <functional>
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
	std::shared_ptr<void> pin;
	std::shared_ptr<managed_vocal_detail::FileState> rootPin;
};
struct HeldSnapshot
{
	std::shared_ptr<ManagedVocalAssetRoot> root;
	Snapshot snapshot;
	std::optional<Snapshot> backup;
};

std::optional<Snapshot> snapshot(const QString& path, const std::shared_ptr<ManagedVocalAssetRootState>& root,
	bool* missing = nullptr)
{
	if (missing) *missing = false;
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
	if (rootCode != QStringLiteral("project_not_in_root"))
	{
		if (missing && rootCode == QStringLiteral("project_missing")) *missing = true;
		return {};
	}
	const auto parent = QDir::fromNativeSeparators(QFileInfo(full).absolutePath());
	auto source = AudioFileAuthorization::acquireWithRetainedPin(parent, full);
	if (!source.source || !source.pin || source.identity.size() != 32)
	{
		if (missing && source.code == QStringLiteral("audio_source_missing")) *missing = true;
		return {};
	}
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
	const auto fileId = QString::fromLatin1(source.identity.toHex());
	const auto fileDigest = QString::fromLatin1(hash.result().toHex());
	// QIODevice is destroyed on the acquiring thread. Only native handles are
	// retained, so the immutable snapshot can be released by a worker safely.
	source.source.reset();
	return Snapshot{fileId, fileDigest, std::move(source.pin), {}};
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

ManagedVocalProjectOwnerStore::Result decodeOwnerEnvelope(const QString& rootId, const Snapshot& state,
	const QByteArray& json, const QByteArray& purpose)
{
	if (json.isEmpty() || json.size() > MaxRecordBytes) return {QStringLiteral("asset_integrity_failed"), {}};
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
	return decodeOwnerEnvelope(rootId, state, json, purpose);
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

ManagedVocalProjectOwnerStore::Result bindSnapshot(const std::shared_ptr<ManagedVocalAssetRootState>& root,
	const QString& rootId, const Snapshot& next, const QString& projectId, const std::function<bool()>& valid)
{
	using Result = ManagedVocalProjectOwnerStore::Result;
	const auto incomplete = [] { return Result{QStringLiteral("asset_incomplete"), {}}; };
	if (!valid()) return incomplete();
	const auto existing = readBinding(root, rootId, next);
	if (existing.owner)
		return existing.owner->projectId == projectId && valid() ? existing : Result{QStringLiteral("asset_integrity_failed"), {}};
	const auto plaintext = QJsonDocument(QJsonObject{{"schema",1},{"project_id",projectId},{"root_id",rootId},
		{"file_id",next.fileId},{"document_sha256",next.digest}}).toJson(QJsonDocument::Compact);
	const auto envelope = [](const QByteArray& value) {
		return QJsonDocument(QJsonObject{{"schema",2},{"protected",QString::fromLatin1(value.toBase64())}})
			.toJson(QJsonDocument::Compact);
	};
	if (existing.code == QStringLiteral("asset_incomplete"))
	{
		const auto credential = readCredential(credentialName(rootId, next));
		if (credential.code != QStringLiteral("ok")) return {credential.code, {}};
		const auto credentialOwner = decodeOwnerEnvelope(rootId, next, credential.bytes, QByteArrayLiteral("witness"));
		if (!credentialOwner.owner || credentialOwner.owner->projectId != projectId) return {QStringLiteral("asset_integrity_failed"), {}};
		QByteArray witnessBytes;
		const auto witness = readProtected(root, rootId, next, witnessName(next), QByteArrayLiteral("witness"), &witnessBytes);
		if (witness.code != QStringLiteral("asset_missing")
			&& (!witness.owner || witness.owner->projectId != projectId || witnessBytes != credential.bytes))
			return {QStringLiteral("asset_integrity_failed"), {}};
		const auto binding = readProtected(root, rootId, next, recordName(next), QByteArrayLiteral("binding"));
		if (binding.code != QStringLiteral("asset_missing") && (!binding.owner || binding.owner->projectId != projectId))
			return {QStringLiteral("asset_integrity_failed"), {}};
		if (witness.code == QStringLiteral("asset_missing"))
		{
			if (!valid()) return incomplete();
			const auto code = publishRecord(root, witnessName(next), credential.bytes); if (code != QStringLiteral("ok")) return {code, {}};
		}
		if (binding.code == QStringLiteral("asset_missing"))
		{
			const auto encrypted = protectRecord(plaintext, protectionEntropy(rootId, next, QByteArrayLiteral("binding")));
			if (!encrypted) return {QStringLiteral("asset_write_failed"), {}};
			if (!valid()) return incomplete();
			const auto code = publishRecord(root, recordName(next), envelope(*encrypted)); if (code != QStringLiteral("ok")) return {code, {}};
		}
	}
	else if (existing.code == QStringLiteral("project_identity_unverified"))
	{
		const auto binding = protectRecord(plaintext, protectionEntropy(rootId, next, QByteArrayLiteral("binding")));
		const auto witness = protectRecord(plaintext, protectionEntropy(rootId, next, QByteArrayLiteral("witness")));
		if (!binding || !witness) return {QStringLiteral("asset_write_failed"), {}};
		if (!valid()) return incomplete();
		const auto witnessBytes = envelope(*witness);
		const auto credentialCode = createCredential(credentialName(rootId, next), witnessBytes);
		if (credentialCode != QStringLiteral("ok")) return {credentialCode, {}};
		if (!valid()) return incomplete();
		const auto witnessCode = publishRecord(root, witnessName(next), witnessBytes); if (witnessCode != QStringLiteral("ok")) return {witnessCode, {}};
		if (!valid()) return incomplete();
		const auto bindingCode = publishRecord(root, recordName(next), envelope(*binding)); if (bindingCode != QStringLiteral("ok")) return {bindingCode, {}};
	}
	else return existing;
	if (!valid()) return incomplete();
	return readBinding(root, rootId, next);
}

bool canonicalUuid(const QString& text)
{
	const auto value = QUuid(text);
	return !value.isNull() && value.toString(QUuid::WithoutBraces) == text;
}

bool saveDigest(const QString& text)
{
	if (text.size() != 64) return false;
	for (const auto c : text)
		if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
			|| (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
	return true;
}

QString saveTargetHash(const QString& path)
{
	if (path.isEmpty() || path.size() > 1024 || path.contains(QChar(0)) || !QDir::isAbsolutePath(path)) return {};
	// Keep the exact spelling: Windows can host case-sensitive directories.
	const auto normalized = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
	return QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("lmms-managed-save-target-v1:")
		+ normalized.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QByteArray saveJournalEntropy(const QString& rootId, const QString& transactionId)
{
	return QCryptographicHash::hash(QByteArrayLiteral("lmms-managed-save-journal-v1:")
		+ rootId.toLatin1() + ':' + transactionId.toLatin1(), QCryptographicHash::Sha256);
}

QString saveJournalName(const QString& id) { return QStringLiteral("vocal-save-") + id + QStringLiteral(".json"); }
QString saveJournalCredential(const QString& rootId, const QString& id)
{
	return QStringLiteral("LMMS/ManagedVocalOwner/v1/") + rootId + QStringLiteral("/save-") + id;
}
QString saveOwnerName(const QString& id) { return QStringLiteral("vocal-pending-") + id + QStringLiteral(".json"); }
QString saveOwnerCredential(const QString& rootId, const QString& id)
{
	return QStringLiteral("LMMS/ManagedVocalOwner/v1/") + rootId + QStringLiteral("/pending-") + id;
}
QString saveTargetCredential(const QString& rootId, const QString& targetHash)
{
	return QStringLiteral("LMMS/ManagedVocalOwner/v1/") + rootId + QStringLiteral("/target-") + targetHash;
}
QString completedSaveCredential(const QString& rootId, const QString& targetHash)
{
	return QStringLiteral("LMMS/ManagedVocalOwner/v1/") + rootId + QStringLiteral("/done-") + targetHash;
}
QString abortedSaveCredential(const QString& rootId, const QString& targetHash)
{
	return QStringLiteral("LMMS/ManagedVocalOwner/v1/") + rootId + QStringLiteral("/abort-") + targetHash;
}

QString removeMatchingCredential(const QString& name, const QByteArray& expected)
{
#ifdef Q_OS_WIN
	const auto credential = readCredential(name);
	if (credential.code == QStringLiteral("asset_missing")) return QStringLiteral("asset_missing");
	if (credential.code != QStringLiteral("ok")) return credential.code;
	if (credential.bytes != expected) return QStringLiteral("asset_integrity_failed");
	return CredDeleteW(reinterpret_cast<LPCWSTR>(name.utf16()), CRED_TYPE_GENERIC, 0)
		? QStringLiteral("ok") : QStringLiteral("asset_write_failed");
#else
	Q_UNUSED(name); Q_UNUSED(expected); return QStringLiteral("unsupported_backend");
#endif
}

QString pendingOwnerStatus(const std::shared_ptr<ManagedVocalAssetRootState>& root,
	const QString& rootId, const QString& projectId)
{
	std::shared_ptr<managed_vocal_detail::FileState> pending;
	const auto fileCode = managed_vocal_detail::openReadFile(root, saveOwnerName(projectId), pending);
	const auto credential = readCredential(saveOwnerCredential(rootId, projectId));
	if (fileCode != QStringLiteral("ok") && fileCode != QStringLiteral("asset_missing")) return fileCode;
	if (credential.code != QStringLiteral("ok") && credential.code != QStringLiteral("asset_missing"))
		return credential.code;
	return fileCode == QStringLiteral("asset_missing") && credential.code == QStringLiteral("asset_missing")
		? QStringLiteral("ok") : QStringLiteral("asset_incomplete");
}

QString pendingTargetStatus(const QString& rootId, const QString& projectPath)
{
	const auto targetHash = saveTargetHash(projectPath);
	if (targetHash.isEmpty()) return QStringLiteral("project_identity_unverified");
	const auto completed = readCredential(completedSaveCredential(rootId, targetHash));
	if (completed.code == QStringLiteral("ok")) return QStringLiteral("asset_incomplete");
	if (completed.code != QStringLiteral("asset_missing")) return completed.code;
	const auto aborted = readCredential(abortedSaveCredential(rootId, targetHash));
	if (aborted.code == QStringLiteral("ok")) return QStringLiteral("asset_incomplete");
	if (aborted.code != QStringLiteral("asset_missing")) return aborted.code;
	const auto credential = readCredential(saveTargetCredential(rootId, targetHash));
	if (credential.code == QStringLiteral("asset_missing")) return QStringLiteral("ok");
	return credential.code == QStringLiteral("ok") ? QStringLiteral("asset_incomplete") : credential.code;
}

ManagedVocalProjectOwnerStore::SaveJournalResult readSaveJournal(
	const std::shared_ptr<ManagedVocalAssetRootState>& root, const QString& rootId,
	const QString& projectPath, const QString& transactionId)
{
	using namespace managed_vocal_detail;
	const auto targetHash = saveTargetHash(projectPath);
	if (targetHash.isEmpty() || !canonicalUuid(transactionId))
		return {QStringLiteral("project_identity_unverified"), {}};
	std::shared_ptr<FileState> file;
	const auto code = openReadFile(root, saveJournalName(transactionId), file);
	const auto credential = readCredential(saveJournalCredential(rootId, transactionId));
	if (credential.code != QStringLiteral("ok") && credential.code != QStringLiteral("asset_missing"))
		return {credential.code, {}};
	if (code == QStringLiteral("asset_missing") && credential.code == QStringLiteral("asset_missing"))
		return {QStringLiteral("asset_incomplete"), {}};
	if (code == QStringLiteral("asset_missing") || credential.code == QStringLiteral("asset_missing"))
		return {QStringLiteral("asset_incomplete"), {}};
	if (code != QStringLiteral("ok")) return {code, {}};
	const auto length = size(file);
	if (length <= 0 || length > MaxRecordBytes) return {QStringLiteral("asset_integrity_failed"), {}};
	QByteArray bytes(int(length), Qt::Uninitialized);
	if (read(file, bytes.data(), length) != length || bytes != credential.bytes)
		return {QStringLiteral("asset_integrity_failed"), {}};
	QJsonParseError error{};
	const auto envelope = QJsonDocument::fromJson(bytes, &error);
	const auto outer = envelope.object();
	const auto text = outer.value(QStringLiteral("protected")).toString().toLatin1();
	const auto encrypted = QByteArray::fromBase64(text);
	if (error.error != QJsonParseError::NoError || !envelope.isObject() || outer.size() != 3
		|| outer.value(QStringLiteral("schema")) != QJsonValue(1) || !outer.value(QStringLiteral("protected")).isString()
		|| outer.value(QStringLiteral("transaction_id")) != QJsonValue(transactionId)
		|| encrypted.isEmpty() || encrypted.toBase64() != text || QJsonDocument(outer).toJson(QJsonDocument::Compact) != bytes)
		return {QStringLiteral("asset_integrity_failed"), {}};
	const auto plain = unprotectRecord(encrypted, saveJournalEntropy(rootId, transactionId));
	if (!plain) return {QStringLiteral("asset_integrity_failed"), {}};
	const auto document = QJsonDocument::fromJson(*plain, &error);
	const auto value = document.object();
	if (error.error != QJsonParseError::NoError || !document.isObject() || value.size() != 9
		|| QJsonDocument(value).toJson(QJsonDocument::Compact) != *plain
		|| value.value(QStringLiteral("schema")) != QJsonValue(1)
		|| value.value(QStringLiteral("transaction_id")) != QJsonValue(transactionId)
		|| value.value(QStringLiteral("root_id")) != QJsonValue(rootId)
		|| value.value(QStringLiteral("target_path_hash")) != QJsonValue(targetHash))
		return {QStringLiteral("asset_integrity_failed"), {}};
	ManagedVocalPendingSaveJournal result;
	result.transactionId = transactionId;
	result.original = {value.value(QStringLiteral("project_id")).toString(), rootId,
		value.value(QStringLiteral("original_file_id")).toString(), value.value(QStringLiteral("original_sha256")).toString()};
	result.targetPathHash = targetHash;
	result.stagedFileIdentity = value.value(QStringLiteral("staged_file_id")).toString();
	result.stagedSha256 = value.value(QStringLiteral("staged_sha256")).toString();
	if (!canonicalUuid(result.original.projectId) || !saveDigest(result.original.fileIdentity)
		|| !saveDigest(result.original.documentSha256) || !saveDigest(result.stagedFileIdentity)
		|| !saveDigest(result.stagedSha256) || result.stagedFileIdentity == result.original.fileIdentity)
		return {QStringLiteral("asset_integrity_failed"), {}};
	std::shared_ptr<FileState> pending;
	const auto pendingCode = openReadFile(root, saveOwnerName(result.original.projectId), pending);
	const auto pendingCredential = readCredential(saveOwnerCredential(rootId, result.original.projectId));
	if (pendingCode == QStringLiteral("asset_missing") || pendingCredential.code == QStringLiteral("asset_missing"))
		return {QStringLiteral("asset_incomplete"), {}};
	if (pendingCode != QStringLiteral("ok")) return {pendingCode, {}};
	if (pendingCredential.code != QStringLiteral("ok")) return {pendingCredential.code, {}};
	const auto pendingLength = size(pending);
	if (pendingLength != length) return {QStringLiteral("asset_integrity_failed"), {}};
	QByteArray pendingBytes(int(pendingLength), Qt::Uninitialized);
	if (read(pending, pendingBytes.data(), pendingLength) != pendingLength
		|| pendingBytes != bytes || pendingCredential.bytes != bytes)
		return {QStringLiteral("asset_integrity_failed"), {}};
	const auto targetCredential = readCredential(saveTargetCredential(rootId, targetHash));
	if (targetCredential.code == QStringLiteral("asset_missing")) return {QStringLiteral("asset_incomplete"), {}};
	if (targetCredential.code != QStringLiteral("ok")) return {targetCredential.code, {}};
	if (targetCredential.bytes != bytes) return {QStringLiteral("asset_integrity_failed"), {}};
	return {QStringLiteral("ok"), std::move(result)};
}

struct VerifiedSaveAsJournal
{
	QString code, sourcePath;
	QByteArray bytes;
	std::optional<ManagedVocalSaveAsJournal> journal;
};

VerifiedSaveAsJournal decodeSaveAsJournal(const QByteArray& bytes, const QString& rootId,
	const QString& targetPath, const QString& transactionId)
{
	const auto invalid = [] { return VerifiedSaveAsJournal{QStringLiteral("asset_integrity_failed"), {}, {}, {}}; };
	const auto targetHash = saveTargetHash(targetPath);
	if (targetHash.isEmpty() || !canonicalUuid(transactionId) || bytes.isEmpty() || bytes.size() > MaxRecordBytes) return invalid();
	QJsonParseError error{}; const auto envelope = QJsonDocument::fromJson(bytes, &error); const auto outer = envelope.object();
	const auto text = outer.value("protected").toString().toLatin1(); const auto encrypted = QByteArray::fromBase64(text);
	if (error.error != QJsonParseError::NoError || !envelope.isObject() || outer.size() != 3
		|| outer.value("schema") != QJsonValue(2) || outer.value("transaction_id") != QJsonValue(transactionId)
		|| !outer.value("protected").isString() || encrypted.isEmpty() || encrypted.toBase64() != text
		|| QJsonDocument(outer).toJson(QJsonDocument::Compact) != bytes) return invalid();
	const auto plain = unprotectRecord(encrypted, saveJournalEntropy(rootId, transactionId)); if (!plain) return invalid();
	const auto document = QJsonDocument::fromJson(*plain, &error); const auto value = document.object();
	if (error.error != QJsonParseError::NoError || !document.isObject() || value.size() != 14
		|| QJsonDocument(value).toJson(QJsonDocument::Compact) != *plain || value.value("schema") != QJsonValue(2)
		|| value.value("transaction_id") != QJsonValue(transactionId) || value.value("root_id") != QJsonValue(rootId)
		|| value.value("target_path_hash") != QJsonValue(targetHash) || !value.value("destination_missing").isBool()) return invalid();
	const auto sourcePath = value.value("source_path").toString(); const auto sourceHash = saveTargetHash(sourcePath);
	if (sourceHash.isEmpty() || value.value("source_path_hash") != QJsonValue(sourceHash) || sourceHash == targetHash) return invalid();
	ManagedVocalSaveAsJournal result;
	result.transactionId = transactionId; result.sourcePathHash = sourceHash; result.targetPathHash = targetHash;
	result.source = {value.value("project_id").toString(), rootId, value.value("source_file_id").toString(), value.value("source_sha256").toString()};
	result.destinationMissing = value.value("destination_missing").toBool();
	result.destinationFileIdentity = value.value("destination_file_id").toString(); result.destinationSha256 = value.value("destination_sha256").toString();
	result.stagedFileIdentity = value.value("staged_file_id").toString(); result.stagedSha256 = value.value("staged_sha256").toString();
	if (!canonicalUuid(result.source.projectId) || !saveDigest(result.source.fileIdentity) || !saveDigest(result.source.documentSha256)
		|| !saveDigest(result.stagedFileIdentity) || !saveDigest(result.stagedSha256) || result.stagedFileIdentity == result.source.fileIdentity
		|| (result.destinationMissing ? (!result.destinationFileIdentity.isEmpty() || !result.destinationSha256.isEmpty())
			: (!saveDigest(result.destinationFileIdentity) || !saveDigest(result.destinationSha256)
				|| result.destinationFileIdentity == result.source.fileIdentity || result.destinationFileIdentity == result.stagedFileIdentity))) return invalid();
	return {QStringLiteral("ok"), sourcePath, bytes, std::move(result)};
}

VerifiedSaveAsJournal readSaveAsJournal(const std::shared_ptr<ManagedVocalAssetRootState>& root,
	const QString& rootId, const QString& targetPath, const QString& transactionId)
{
	using namespace managed_vocal_detail;
	const auto invalid = [] { return VerifiedSaveAsJournal{QStringLiteral("asset_integrity_failed"), {}, {}, {}}; };
	std::shared_ptr<FileState> file;
	const auto code = openReadFile(root, saveJournalName(transactionId), file);
	const auto credential = readCredential(saveJournalCredential(rootId, transactionId));
	if (code == QStringLiteral("asset_missing") || credential.code == QStringLiteral("asset_missing"))
		return {QStringLiteral("asset_incomplete"), {}, {}, {}};
	if (code != QStringLiteral("ok")) return {code, {}, {}, {}};
	if (credential.code != QStringLiteral("ok")) return {credential.code, {}, {}, {}};
	const auto length = size(file); if (length <= 0 || length > MaxRecordBytes) return invalid();
	QByteArray bytes(int(length), Qt::Uninitialized);
	if (read(file, bytes.data(), length) != length || bytes != credential.bytes) return invalid();
	auto decoded = decodeSaveAsJournal(bytes, rootId, targetPath, transactionId); if (!decoded.journal) return decoded;
	const auto& result = *decoded.journal; const auto targetHash = result.targetPathHash;
	std::shared_ptr<FileState> pending; const auto pendingCode = openReadFile(root, saveOwnerName(result.source.projectId), pending);
	const auto ownerCredential = readCredential(saveOwnerCredential(rootId, result.source.projectId));
	const auto targetCredential = readCredential(saveTargetCredential(rootId, targetHash));
	if (pendingCode == QStringLiteral("asset_missing") || ownerCredential.code == QStringLiteral("asset_missing")
		|| targetCredential.code == QStringLiteral("asset_missing")) return {QStringLiteral("asset_incomplete"), {}, {}, {}};
	if (pendingCode != QStringLiteral("ok")) return {pendingCode, {}, {}, {}};
	if (ownerCredential.code != QStringLiteral("ok")) return {ownerCredential.code, {}, {}, {}};
	if (targetCredential.code != QStringLiteral("ok")) return {targetCredential.code, {}, {}, {}};
	if (size(pending) != length || ownerCredential.bytes != bytes || targetCredential.bytes != bytes) return invalid();
	QByteArray pendingBytes(int(length), Qt::Uninitialized);
	if (read(pending, pendingBytes.data(), length) != length || pendingBytes != bytes) return invalid();
	return decoded;
}
}

ManagedVocalProjectOwnerStore::ManagedVocalProjectOwnerStore(std::shared_ptr<ManagedVocalAssetRoot> root)
	: m_root(std::move(root)) {}

ManagedVocalSaveRecoveryDecision classifyManagedVocalSaveRecovery(const ManagedVocalSaveRecoveryEvidence& evidence)
{
	const auto hexDigest = [](const QString& value) {
		if (value.size() != 64) return false;
		for (const auto c : value)
			if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
				|| (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
		return true;
	};
	const auto project = QUuid(evidence.original.projectId);
	if (project.isNull() || project.toString(QUuid::WithoutBraces) != evidence.original.projectId
		|| !hexDigest(evidence.original.rootId) || !hexDigest(evidence.original.fileIdentity)
		|| !hexDigest(evidence.original.documentSha256) || !hexDigest(evidence.stagedFileIdentity)
		|| !hexDigest(evidence.stagedSha256) || evidence.original.fileIdentity == evidence.stagedFileIdentity)
		return ManagedVocalSaveRecoveryDecision::RecoveryRequired;
	using File = ManagedVocalSaveFileEvidence;
	const auto valid = [&](const File& file) {
		if (file.state == File::State::Missing) return file.fileIdentity.isEmpty() && file.documentSha256.isEmpty();
		return file.state == File::State::Verified && hexDigest(file.fileIdentity) && hexDigest(file.documentSha256);
	};
	if (!valid(evidence.target) || !valid(evidence.backup) || !valid(evidence.stage))
		return ManagedVocalSaveRecoveryDecision::RecoveryRequired;
	const auto isMissing = [](const File& file) { return file.state == File::State::Missing; };
	const auto matches = [](const File& file, const QString& id, const QString& digest) {
		return file.state == File::State::Verified && file.fileIdentity == id && file.documentSha256 == digest;
	};
	const auto isOriginal = [&](const File& file) {
		return matches(file, evidence.original.fileIdentity, evidence.original.documentSha256);
	};
	const auto isStaged = [&](const File& file) {
		return matches(file, evidence.stagedFileIdentity, evidence.stagedSha256);
	};
	if (isOriginal(evidence.target) && isMissing(evidence.backup)
		&& (isStaged(evidence.stage) || isMissing(evidence.stage)))
		return ManagedVocalSaveRecoveryDecision::OriginalIntact;
	if (isMissing(evidence.target) && isOriginal(evidence.backup)
		&& (isStaged(evidence.stage) || isMissing(evidence.stage)))
		return ManagedVocalSaveRecoveryDecision::RestoreOriginal;
	if (isStaged(evidence.target) && isOriginal(evidence.backup) && isMissing(evidence.stage))
		return ManagedVocalSaveRecoveryDecision::FinalizePublished;
	return ManagedVocalSaveRecoveryDecision::RecoveryRequired;
}

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
	return ensureImpl(projectPath, {}, {}, false);
}

ManagedVocalProjectOwnerStore::Result ManagedVocalProjectOwnerStore::ensureExpected(
	const QString& projectPath, const QString& fileIdentity, const QString& documentSha256) const
{
	return ensureImpl(projectPath, fileIdentity, documentSha256, true);
}

ManagedVocalProjectOwnerStore::Result ManagedVocalProjectOwnerStore::ensureLoadedSnapshot(
	const QString& projectPath, const QString& sourceFileId, const QByteArray& sourceSha256) const
{
	const auto invalid = [] { return Result{QStringLiteral("project_identity_unverified"), {}}; };
	if (sourceFileId.size() != 26 || sourceFileId[8] != QLatin1Char(':') || sourceFileId[17] != QLatin1Char(':')
		|| sourceSha256.size() != 64) return invalid();
	for (int i = 0; i < sourceFileId.size(); ++i)
	{
		if (i == 8 || i == 17) continue;
		const auto c = sourceFileId[i];
		if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
			|| (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return invalid();
	}
	for (const auto c : sourceSha256)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return invalid();
	bool volumeOk = false, highOk = false, lowOk = false;
	const auto volume = sourceFileId.mid(0, 8).toUInt(&volumeOk, 16);
	const auto high = sourceFileId.mid(9, 8).toUInt(&highOk, 16);
	const auto low = sourceFileId.mid(18, 8).toUInt(&lowOk, 16);
	if (!volumeOk || !highOk || !lowOk) return invalid();
	const auto material = QByteArray::number(volume) + ':' + QByteArray::number(high) + ':' + QByteArray::number(low);
	const auto expectedId = QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
	return ensureExpected(projectPath, expectedId, QString::fromLatin1(sourceSha256));
}

ManagedVocalProjectOwnerStore::Result ManagedVocalProjectOwnerStore::ensureImpl(
	const QString& projectPath, const QString& fileIdentity, const QString& documentSha256, bool requireExpected) const
{
	using namespace managed_vocal_detail;
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	const auto current = snapshot(projectPath, m_root->m_state);
	if (!current) return {QStringLiteral("project_identity_unverified"), {}};
	if (requireExpected && (current->fileId != fileIdentity || current->digest != documentSha256))
		return {QStringLiteral("project_identity_unverified"), {}};
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

ManagedVocalProjectOwnerStore::HoldResult ManagedVocalProjectOwnerStore::holdVerified(
	const QString& projectPath, const ManagedVocalProjectOwner& owner) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	auto current = snapshot(projectPath, m_root->m_state);
	if (!current || owner.rootId != m_root->rootId() || current->fileId != owner.fileIdentity
		|| current->digest != owner.documentSha256)
		return {QStringLiteral("project_identity_unverified"), {}};
	const auto binding = readBinding(m_root->m_state, owner.rootId, *current);
	if (!binding.owner) return {binding.code, {}};
	if (binding.owner->projectId != owner.projectId || binding.owner->rootId != owner.rootId
		|| binding.owner->fileIdentity != owner.fileIdentity || binding.owner->documentSha256 != owner.documentSha256)
		return {QStringLiteral("project_identity_unverified"), {}};
	const auto pending = pendingOwnerStatus(m_root->m_state, owner.rootId, owner.projectId);
	if (pending != QStringLiteral("ok")) return {pending, {}};
	const auto pendingTarget = pendingTargetStatus(owner.rootId, projectPath);
	if (pendingTarget != QStringLiteral("ok")) return {pendingTarget, {}};
	// A legacy save removes the old .bak before attempting to rename a pinned
	// project. Pin an existing backup as well, and fail closed if its state is
	// unreadable or unsafe. A missing backup is not created or modified here.
	bool backupMissing = false;
	auto backup = snapshot(projectPath + QStringLiteral(".bak"), m_root->m_state, &backupMissing);
	if (!backup && !backupMissing) return {QStringLiteral("project_identity_unverified"), {}};
	return {QStringLiteral("ok"), std::make_shared<HeldSnapshot>(
		HeldSnapshot{m_root, std::move(*current), std::move(backup)})};
}

ManagedVocalProjectOwnerStore::SaveJournalResult ManagedVocalProjectOwnerStore::prepareSaveJournal(
	const QString& projectPath, const QString& transactionId, const ManagedVocalProjectOwner& owner) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	const auto targetHash = saveTargetHash(projectPath);
	if (targetHash.isEmpty() || !canonicalUuid(transactionId) || owner.rootId != m_root->rootId())
		return {QStringLiteral("project_identity_unverified"), {}};
	auto original = snapshot(projectPath, m_root->m_state);
	if (!original || original->fileId != owner.fileIdentity || original->digest != owner.documentSha256)
		return {QStringLiteral("project_identity_unverified"), {}};
	const auto binding = readBinding(m_root->m_state, owner.rootId, *original);
	if (!binding.owner) return {binding.code, {}};
	if (binding.owner->projectId != owner.projectId || binding.owner->rootId != owner.rootId
		|| binding.owner->fileIdentity != owner.fileIdentity || binding.owner->documentSha256 != owner.documentSha256)
		return {QStringLiteral("project_identity_unverified"), {}};
	const auto pending = pendingOwnerStatus(m_root->m_state, owner.rootId, owner.projectId);
	if (pending != QStringLiteral("ok")) return {pending, {}};
	const auto pendingTarget = pendingTargetStatus(owner.rootId, projectPath);
	if (pendingTarget != QStringLiteral("ok")) return {pendingTarget, {}};
	const auto stagePath = QFileInfo(projectPath).absoluteDir().filePath(QStringLiteral(".lmms-vocal-stage-") + transactionId);
	auto stage = snapshot(stagePath, m_root->m_state);
	if (!stage || stage->fileId == original->fileId) return {QStringLiteral("project_identity_unverified"), {}};
	const QJsonObject value{{"schema",1},{"transaction_id",transactionId},{"root_id",owner.rootId},
		{"project_id",owner.projectId},{"target_path_hash",targetHash},
		{"original_file_id",original->fileId},{"original_sha256",original->digest},
		{"staged_file_id",stage->fileId},{"staged_sha256",stage->digest}};
	const auto encrypted = protectRecord(QJsonDocument(value).toJson(QJsonDocument::Compact),
		saveJournalEntropy(owner.rootId, transactionId));
	if (!encrypted) return {QStringLiteral("asset_write_failed"), {}};
	const auto bytes = QJsonDocument(QJsonObject{{"schema",1},{"transaction_id",transactionId},
		{"protected",QString::fromLatin1(encrypted->toBase64())}})
		.toJson(QJsonDocument::Compact);
	if (bytes.size() > MaxRecordBytes) return {QStringLiteral("asset_write_failed"), {}};
	// The target index is published first: after interruption it must not be
	// mistaken for a complete journal or authorize publication.
	const auto targetCredential = createCredential(saveTargetCredential(owner.rootId, targetHash), bytes);
	if (targetCredential != QStringLiteral("ok")) return {targetCredential, {}};
	const auto ownerCredential = createCredential(saveOwnerCredential(owner.rootId, owner.projectId), bytes);
	if (ownerCredential != QStringLiteral("ok")) return {ownerCredential, {}};
	const auto credential = createCredential(saveJournalCredential(owner.rootId, transactionId), bytes);
	if (credential != QStringLiteral("ok")) return {credential, {}};
	const auto pendingPublished = publishRecord(m_root->m_state, saveOwnerName(owner.projectId), bytes);
	if (pendingPublished != QStringLiteral("ok")) return {pendingPublished, {}};
	const auto published = publishRecord(m_root->m_state, saveJournalName(transactionId), bytes);
	if (published != QStringLiteral("ok")) return {published, {}};
	return readSaveJournal(m_root->m_state, owner.rootId, projectPath, transactionId);
}

ManagedVocalProjectOwnerStore::SaveJournalResult ManagedVocalProjectOwnerStore::loadSaveJournal(
	const QString& projectPath, const QString& transactionId) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	return readSaveJournal(m_root->m_state, m_root->rootId(), projectPath, transactionId);
}

ManagedVocalProjectOwnerStore::SaveJournalResult ManagedVocalProjectOwnerStore::prepareSaveJournal(
	const ProjectSavePublication& publication, const ManagedVocalProjectOwner& owner) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	ProjectSavePublication::Evidence evidence;
	if (!publication.evidence(evidence) || owner.rootId != m_root->rootId()
		|| evidence.originalIdentity.toHex() != owner.fileIdentity.toLatin1()
		|| evidence.originalSha256.toHex() != owner.documentSha256.toLatin1()
		|| evidence.stageIdentity.size() != 32 || evidence.stageSha256.size() != 32
		|| evidence.stageIdentity == evidence.originalIdentity || !canonicalUuid(evidence.transactionId))
		return {QStringLiteral("project_identity_unverified"), {}};
	const auto targetHash = saveTargetHash(evidence.target);
	if (targetHash.isEmpty()) return {QStringLiteral("project_identity_unverified"), {}};
	Snapshot original; original.fileId = owner.fileIdentity; original.digest = owner.documentSha256;
	const auto binding = readBinding(m_root->m_state, owner.rootId, original);
	if (!binding.owner) return {binding.code, {}};
	if (binding.owner->projectId != owner.projectId || binding.owner->rootId != owner.rootId
		|| binding.owner->fileIdentity != owner.fileIdentity || binding.owner->documentSha256 != owner.documentSha256)
		return {QStringLiteral("project_identity_unverified"), {}};
	const auto pending = pendingOwnerStatus(m_root->m_state, owner.rootId, owner.projectId);
	if (pending != QStringLiteral("ok")) return {pending, {}};
	const auto targetStatus = pendingTargetStatus(owner.rootId, evidence.target);
	if (targetStatus != QStringLiteral("ok")) return {targetStatus, {}};
	const auto tx = evidence.transactionId;
	const QJsonObject value{{"schema",1},{"transaction_id",tx},{"root_id",owner.rootId},
		{"project_id",owner.projectId},{"target_path_hash",targetHash},
		{"original_file_id",owner.fileIdentity},{"original_sha256",owner.documentSha256},
		{"staged_file_id",QString::fromLatin1(evidence.stageIdentity.toHex())},
		{"staged_sha256",QString::fromLatin1(evidence.stageSha256.toHex())}};
	const auto encrypted = protectRecord(QJsonDocument(value).toJson(QJsonDocument::Compact), saveJournalEntropy(owner.rootId, tx));
	if (!encrypted) return {QStringLiteral("asset_write_failed"), {}};
	const auto bytes = QJsonDocument(QJsonObject{{"schema",1},{"transaction_id",tx},
		{"protected",QString::fromLatin1(encrypted->toBase64())}}).toJson(QJsonDocument::Compact);
	if (bytes.size() > MaxRecordBytes) return {QStringLiteral("asset_write_failed"), {}};
	const auto targetCredential = createCredential(saveTargetCredential(owner.rootId, targetHash), bytes);
	if (targetCredential != QStringLiteral("ok")) return {targetCredential, {}};
	const auto ownerCredential = createCredential(saveOwnerCredential(owner.rootId, owner.projectId), bytes);
	if (ownerCredential != QStringLiteral("ok")) return {ownerCredential, {}};
	const auto credential = createCredential(saveJournalCredential(owner.rootId, tx), bytes);
	if (credential != QStringLiteral("ok")) return {credential, {}};
	const auto pendingPublished = publishRecord(m_root->m_state, saveOwnerName(owner.projectId), bytes);
	if (pendingPublished != QStringLiteral("ok")) return {pendingPublished, {}};
	const auto published = publishRecord(m_root->m_state, saveJournalName(tx), bytes);
	if (published != QStringLiteral("ok")) return {published, {}};
	ProjectSavePublication::Evidence stillHeld;
	if (!m_root->isAuthorized() || !publication.evidence(stillHeld)
		|| stillHeld.target != evidence.target || stillHeld.transactionId != tx
		|| stillHeld.originalIdentity != evidence.originalIdentity || stillHeld.originalSha256 != evidence.originalSha256
		|| stillHeld.stageIdentity != evidence.stageIdentity || stillHeld.stageSha256 != evidence.stageSha256)
		return {QStringLiteral("asset_incomplete"), {}};
	return readSaveJournal(m_root->m_state, owner.rootId, evidence.target, tx);
}

ManagedVocalProjectOwnerStore::SaveJournalResult ManagedVocalProjectOwnerStore::findPendingSaveJournal(
	const QString& projectPath, const ManagedVocalProjectOwner& owner) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	if (owner.rootId != m_root->rootId() || !canonicalUuid(owner.projectId)
		|| !saveDigest(owner.fileIdentity) || !saveDigest(owner.documentSha256))
		return {QStringLiteral("project_identity_unverified"), {}};
	const auto credential = readCredential(saveOwnerCredential(owner.rootId, owner.projectId));
	if (credential.code == QStringLiteral("asset_missing"))
	{
		std::shared_ptr<managed_vocal_detail::FileState> pending;
		const auto fileCode = managed_vocal_detail::openReadFile(m_root->m_state, saveOwnerName(owner.projectId), pending);
		if (fileCode == QStringLiteral("asset_missing")) return {QStringLiteral("asset_missing"), {}};
		return {fileCode == QStringLiteral("ok") ? QStringLiteral("asset_incomplete") : fileCode, {}};
	}
	if (credential.code != QStringLiteral("ok")) return {credential.code, {}};
	if (credential.bytes.isEmpty() || credential.bytes.size() > MaxRecordBytes)
		return {QStringLiteral("asset_integrity_failed"), {}};
	QJsonParseError error{};
	const auto envelope = QJsonDocument::fromJson(credential.bytes, &error);
	const auto transactionId = envelope.object().value(QStringLiteral("transaction_id")).toString();
	if (error.error != QJsonParseError::NoError || !envelope.isObject() || !canonicalUuid(transactionId))
		return {QStringLiteral("asset_integrity_failed"), {}};
	const auto found = readSaveJournal(m_root->m_state, owner.rootId, projectPath, transactionId);
	if (!found.journal) return found;
	const auto& original = found.journal->original;
	if (original.projectId != owner.projectId || original.rootId != owner.rootId
		|| original.fileIdentity != owner.fileIdentity || original.documentSha256 != owner.documentSha256)
		return {QStringLiteral("project_identity_unverified"), {}};
	return found;
}

ManagedVocalProjectOwnerStore::SaveJournalResult ManagedVocalProjectOwnerStore::findPendingSaveJournalForTarget(
	const QString& projectPath) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	const auto targetHash = saveTargetHash(projectPath);
	if (targetHash.isEmpty()) return {QStringLiteral("project_identity_unverified"), {}};
	const auto credential = readCredential(saveTargetCredential(m_root->rootId(), targetHash));
	if (credential.code == QStringLiteral("asset_missing")) return {QStringLiteral("asset_incomplete"), {}};
	if (credential.code != QStringLiteral("ok")) return {credential.code, {}};
	if (credential.bytes.isEmpty() || credential.bytes.size() > MaxRecordBytes)
		return {QStringLiteral("asset_integrity_failed"), {}};
	QJsonParseError error{};
	const auto envelope = QJsonDocument::fromJson(credential.bytes, &error);
	const auto transactionId = envelope.object().value(QStringLiteral("transaction_id")).toString();
	if (error.error != QJsonParseError::NoError || !envelope.isObject() || !canonicalUuid(transactionId))
		return {QStringLiteral("asset_integrity_failed"), {}};
	const auto found = readSaveJournal(m_root->m_state, m_root->rootId(), projectPath, transactionId);
	if (!found.journal) return found;
	const auto& owner = found.journal->original;
	Snapshot original;
	original.fileId = owner.fileIdentity;
	original.digest = owner.documentSha256;
	const auto binding = readBinding(m_root->m_state, owner.rootId, original);
	if (!binding.owner) return {binding.code, {}};
	if (binding.owner->projectId != owner.projectId || binding.owner->rootId != owner.rootId
		|| binding.owner->fileIdentity != owner.fileIdentity || binding.owner->documentSha256 != owner.documentSha256)
		return {QStringLiteral("project_identity_unverified"), {}};
	return found;
}

ManagedVocalProjectOwnerStore::Result ManagedVocalProjectOwnerStore::bindPublishedSnapshot(
	const ProjectSavePublication& publication, const ManagedVocalProjectOwner& original) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	ProjectSavePublication::Evidence evidence;
	if (!publication.publishedEvidence(evidence) || original.rootId != m_root->rootId()
		|| evidence.originalIdentity.toHex() != original.fileIdentity.toLatin1()
		|| evidence.originalSha256.toHex() != original.documentSha256.toLatin1()
		|| evidence.stageIdentity.size() != 32 || evidence.stageSha256.size() != 32)
		return {QStringLiteral("project_identity_unverified"), {}};
	const auto journal = readSaveJournal(m_root->m_state, original.rootId, evidence.target, evidence.transactionId);
	if (!journal.journal) return {journal.code, {}};
	const auto& pending = *journal.journal;
	if (pending.original.projectId != original.projectId || pending.original.rootId != original.rootId
		|| pending.original.fileIdentity != original.fileIdentity || pending.original.documentSha256 != original.documentSha256
		|| pending.stagedFileIdentity.toLatin1() != evidence.stageIdentity.toHex()
		|| pending.stagedSha256.toLatin1() != evidence.stageSha256.toHex())
		return {QStringLiteral("project_identity_unverified"), {}};
	Snapshot previous; previous.fileId = original.fileIdentity; previous.digest = original.documentSha256;
	const auto prior = readBinding(m_root->m_state, original.rootId, previous);
	if (!prior.owner) return prior;
	if (prior.owner->projectId != original.projectId || prior.owner->rootId != original.rootId)
		return {QStringLiteral("project_identity_unverified"), {}};
	Snapshot next; next.fileId = QString::fromLatin1(evidence.stageIdentity.toHex());
	next.digest = QString::fromLatin1(evidence.stageSha256.toHex());
	const auto valid = [&] {
		ProjectSavePublication::Evidence retained;
		return m_root->isAuthorized() && publication.publishedEvidence(retained)
			&& retained.target == evidence.target && retained.transactionId == evidence.transactionId
			&& retained.stageIdentity == evidence.stageIdentity && retained.stageSha256 == evidence.stageSha256;
	};
	return bindSnapshot(m_root->m_state, original.rootId, next, original.projectId, valid);
}

QString ManagedVocalProjectOwnerStore::finishPublishedSnapshot(
	const ProjectSavePublication& publication, const ManagedVocalProjectOwner& original) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return QStringLiteral("asset_root_revoked");
	ProjectSavePublication::Evidence evidence;
	if (!publication.publishedEvidence(evidence) || original.rootId != m_root->rootId()
		|| evidence.originalIdentity.toHex() != original.fileIdentity.toLatin1()
		|| evidence.originalSha256.toHex() != original.documentSha256.toLatin1()) return QStringLiteral("project_identity_unverified");
	const auto journal = readSaveJournal(m_root->m_state, original.rootId, evidence.target, evidence.transactionId);
	if (!journal.journal) return journal.code;
	const auto& pending = *journal.journal;
	if (pending.original.projectId != original.projectId || pending.original.rootId != original.rootId
		|| pending.original.fileIdentity != original.fileIdentity || pending.original.documentSha256 != original.documentSha256
		|| pending.stagedFileIdentity.toLatin1() != evidence.stageIdentity.toHex()
		|| pending.stagedSha256.toLatin1() != evidence.stageSha256.toHex()) return QStringLiteral("project_identity_unverified");
	Snapshot stage; stage.fileId = pending.stagedFileIdentity; stage.digest = pending.stagedSha256;
	const auto binding = readBinding(m_root->m_state, original.rootId, stage);
	if (!binding.owner) return binding.code;
	if (binding.owner->projectId != original.projectId || binding.owner->rootId != original.rootId)
		return QStringLiteral("asset_integrity_failed");
	const auto saved = readCredential(saveJournalCredential(original.rootId, evidence.transactionId));
	if (saved.code != QStringLiteral("ok")) return saved.code;
	const auto terminalName = completedSaveCredential(original.rootId, pending.targetPathHash);
	const auto terminal = createCredential(terminalName, saved.bytes);
	if (terminal != QStringLiteral("ok")) return terminal;
	const auto stillValid = [&] {
		ProjectSavePublication::Evidence current;
		return m_root->isAuthorized() && publication.publishedEvidence(current)
			&& current.target == evidence.target && current.transactionId == evidence.transactionId
			&& current.stageIdentity == evidence.stageIdentity && current.stageSha256 == evidence.stageSha256;
	};
	for (const auto& name : {saveJournalName(evidence.transactionId), saveOwnerName(original.projectId)})
	{
		if (!stillValid()) return QStringLiteral("asset_incomplete");
		const auto removed = managed_vocal_detail::removeMatchingFile(m_root->m_state, name, saved.bytes);
		if (removed != QStringLiteral("ok")) return removed;
	}
	for (const auto& name : {saveTargetCredential(original.rootId, pending.targetPathHash),
		saveJournalCredential(original.rootId, evidence.transactionId), saveOwnerCredential(original.rootId, original.projectId)})
	{
		if (!stillValid()) return QStringLiteral("asset_incomplete");
		const auto removed = removeMatchingCredential(name, saved.bytes);
		if (removed != QStringLiteral("ok")) return removed;
	}
	if (!stillValid()) return QStringLiteral("asset_incomplete");
	return removeMatchingCredential(terminalName, saved.bytes);
}

QString ManagedVocalProjectOwnerStore::recoverPublishedSnapshot(const QString& projectPath) const
{
	return recoverSnapshot(projectPath, true);
}

QString ManagedVocalProjectOwnerStore::recoverSnapshot(const QString& projectPath, bool published) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return QStringLiteral("asset_root_revoked");
	const auto rootId = m_root->rootId(); const auto targetHash = saveTargetHash(projectPath);
	if (targetHash.isEmpty()) return QStringLiteral("project_identity_unverified");
	const auto terminalName = published ? completedSaveCredential(rootId, targetHash) : abortedSaveCredential(rootId, targetHash);
	auto terminal = readCredential(terminalName);
	const bool needsTerminal = terminal.code == QStringLiteral("asset_missing");
	if (needsTerminal) terminal = readCredential(saveTargetCredential(rootId, targetHash));
	if (terminal.code != QStringLiteral("ok")) return terminal.code;
	if (terminal.bytes.isEmpty() || terminal.bytes.size() > MaxRecordBytes) return QStringLiteral("asset_integrity_failed");
	QJsonParseError error{}; const auto envelope = QJsonDocument::fromJson(terminal.bytes, &error);
	const auto outer = envelope.object(); const auto tx = outer.value(QStringLiteral("transaction_id")).toString();
	const auto text = outer.value(QStringLiteral("protected")).toString().toLatin1(); const auto encrypted = QByteArray::fromBase64(text);
	if (error.error != QJsonParseError::NoError || !envelope.isObject() || outer.size() != 3
		|| outer.value(QStringLiteral("schema")) != QJsonValue(1) || !canonicalUuid(tx)
		|| !outer.value(QStringLiteral("protected")).isString() || encrypted.isEmpty() || encrypted.toBase64() != text
		|| QJsonDocument(outer).toJson(QJsonDocument::Compact) != terminal.bytes) return QStringLiteral("asset_integrity_failed");
	const auto plain = unprotectRecord(encrypted, saveJournalEntropy(rootId, tx));
	if (!plain) return QStringLiteral("asset_integrity_failed");
	const auto document = QJsonDocument::fromJson(*plain, &error); const auto value = document.object();
	if (error.error != QJsonParseError::NoError || !document.isObject() || value.size() != 9
		|| QJsonDocument(value).toJson(QJsonDocument::Compact) != *plain || value.value("schema") != QJsonValue(1)
		|| value.value("transaction_id") != QJsonValue(tx) || value.value("root_id") != QJsonValue(rootId)
		|| value.value("target_path_hash") != QJsonValue(targetHash)) return QStringLiteral("asset_integrity_failed");
	const auto projectId = value.value("project_id").toString();
	const auto oldId = value.value("original_file_id").toString(), oldSha = value.value("original_sha256").toString();
	const auto stageId = value.value("staged_file_id").toString(), stageSha = value.value("staged_sha256").toString();
	if (!canonicalUuid(projectId) || !saveDigest(oldId) || !saveDigest(oldSha) || !saveDigest(stageId)
		|| !saveDigest(stageSha) || oldId == stageId) return QStringLiteral("asset_integrity_failed");
	if (needsTerminal && published)
	{
		// Publishing a new owner requires every pending witness unless a
		// completion receipt has already been created.
		const auto full = readSaveJournal(m_root->m_state, rootId, projectPath, tx);
		if (!full.journal) return full.code;
	}
	auto current = snapshot(projectPath, m_root->m_state);
	const auto backupPath = QFileInfo(projectPath).absoluteDir().filePath(QStringLiteral(".lmms-vocal-backup-") + tx);
	const auto auxiliaryPath = published ? backupPath : QFileInfo(projectPath).absoluteDir().filePath(QStringLiteral(".lmms-vocal-stage-") + tx);
	auto auxiliary = snapshot(auxiliaryPath, m_root->m_state);
	if (!current || !auxiliary) return QStringLiteral("project_identity_unverified");
	if (published)
	{
		if (current->fileId != stageId || current->digest != stageSha || auxiliary->fileId != oldId || auxiliary->digest != oldSha)
			return QStringLiteral("project_identity_unverified");
	}
	else
	{
		if (current->fileId != oldId || current->digest != oldSha || auxiliary->fileId != stageId || auxiliary->digest != stageSha)
			return QStringLiteral("project_identity_unverified");
		bool missing = false; const auto unexpectedBackup = snapshot(backupPath, m_root->m_state, &missing);
		if (unexpectedBackup || !missing) return QStringLiteral("project_identity_unverified");
	}
	const auto oldBinding = readBinding(m_root->m_state, rootId, published ? *auxiliary : *current);
	if (!oldBinding.owner || oldBinding.owner->projectId != projectId) return QStringLiteral("asset_integrity_failed");
	const auto pinsValid = [&] {
		return m_root->isAuthorized() && (current->pin || current->rootPin) && (auxiliary->pin || auxiliary->rootPin);
	};
	if (published)
	{
		const auto currentBinding = bindSnapshot(m_root->m_state, rootId, *current, projectId, pinsValid);
		if (!currentBinding.owner) return currentBinding.code;
		if (currentBinding.owner->projectId != projectId) return QStringLiteral("asset_integrity_failed");
	}
	const QStringList fileNames{saveJournalName(tx), saveOwnerName(projectId)};
	const QStringList credentialNames{saveTargetCredential(rootId, targetHash), saveJournalCredential(rootId, tx),
		saveOwnerCredential(rootId, projectId)};
	for (const auto& name : fileNames)
	{
		std::shared_ptr<managed_vocal_detail::FileState> file;
		const auto code = managed_vocal_detail::openReadFile(m_root->m_state, name, file);
		if (code == QStringLiteral("asset_missing")) continue;
		if (code != QStringLiteral("ok")) return code;
		const auto length = managed_vocal_detail::size(file);
		if (length != terminal.bytes.size()) return QStringLiteral("asset_integrity_failed");
		QByteArray bytes(int(length), Qt::Uninitialized);
		if (managed_vocal_detail::read(file, bytes.data(), length) != length || bytes != terminal.bytes)
			return QStringLiteral("asset_integrity_failed");
	}
	for (const auto& name : credentialNames)
	{
		const auto existing = readCredential(name);
		if (existing.code == QStringLiteral("asset_missing")) continue;
		if (existing.code != QStringLiteral("ok")) return existing.code;
		if (existing.bytes != terminal.bytes) return QStringLiteral("asset_integrity_failed");
	}
	if (needsTerminal)
	{
		if (!m_root->isAuthorized()) return QStringLiteral("asset_root_revoked");
		const auto created = createCredential(terminalName, terminal.bytes);
		if (created != QStringLiteral("ok")) return created;
	}
	for (const auto& name : fileNames)
	{
		if (!m_root->isAuthorized()) return QStringLiteral("asset_root_revoked");
		const auto removed = managed_vocal_detail::removeMatchingFile(m_root->m_state, name, terminal.bytes);
		if (removed != QStringLiteral("ok") && removed != QStringLiteral("asset_missing")) return removed;
	}
	for (const auto& name : credentialNames)
	{
		if (!m_root->isAuthorized()) return QStringLiteral("asset_root_revoked");
		const auto removed = removeMatchingCredential(name, terminal.bytes);
		if (removed != QStringLiteral("ok") && removed != QStringLiteral("asset_missing")) return removed;
	}
	if (!m_root->isAuthorized()) return QStringLiteral("asset_root_revoked");
	return removeMatchingCredential(terminalName, terminal.bytes);
}

QString ManagedVocalProjectOwnerStore::recoverUnpublishedSnapshot(const QString& projectPath) const
{
	return recoverSnapshot(projectPath, false);
}

ManagedVocalProjectOwnerStore::SaveAsSourceResult ManagedVocalProjectOwnerStore::holdSaveAsSource(
	const QString& projectPath, const ManagedVocalProjectOwner& owner) const
{
	const auto pinned = holdVerified(projectPath, owner);
	if (!pinned.hold) return {pinned.code, {}};
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	auto source = std::shared_ptr<ManagedVocalSaveAsSource>(new ManagedVocalSaveAsSource);
	source->path = QDir::fromNativeSeparators(QFileInfo(projectPath).absoluteFilePath());
	source->owner = owner; source->hold = pinned.hold; source->root = m_root;
	return {QStringLiteral("ok"), std::move(source)};
}

ManagedVocalProjectOwnerStore::SaveAsJournalResult ManagedVocalProjectOwnerStore::prepareSaveAsJournal(
	const ManagedVocalSaveAsSource& source, const ProjectSavePublication& destination) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	if (!source.hold || !source.root || source.root.get() != m_root.get()
		|| source.owner.rootId != m_root->rootId()) return {QStringLiteral("project_identity_unverified"), {}};
	ProjectSavePublication::Evidence evidence;
	if (!destination.stagedEvidence(evidence) || !canonicalUuid(evidence.transactionId)
		|| (evidence.originalMissing ? (!evidence.originalIdentity.isEmpty() || !evidence.originalSha256.isEmpty())
			: (evidence.originalIdentity.size() != 32 || evidence.originalSha256.size() != 32))
		|| evidence.stageIdentity.size() != 32 || evidence.stageSha256.size() != 32)
		return {QStringLiteral("project_identity_unverified"), {}};
	const auto sourceHash = saveTargetHash(source.path), targetHash = saveTargetHash(evidence.target);
	if (sourceHash.isEmpty() || targetHash.isEmpty() || sourceHash == targetHash
		|| (!evidence.originalMissing && (evidence.originalIdentity.toHex() == source.owner.fileIdentity.toLatin1()
			|| evidence.stageIdentity == evidence.originalIdentity))) return {QStringLiteral("project_identity_unverified"), {}};
	Snapshot old; old.fileId = source.owner.fileIdentity; old.digest = source.owner.documentSha256;
	const auto binding = readBinding(m_root->m_state, source.owner.rootId, old);
	if (!binding.owner) return {binding.code, {}};
	if (binding.owner->projectId != source.owner.projectId || binding.owner->rootId != source.owner.rootId
		|| binding.owner->fileIdentity != source.owner.fileIdentity || binding.owner->documentSha256 != source.owner.documentSha256)
		return {QStringLiteral("project_identity_unverified"), {}};
	const auto pendingOwner = pendingOwnerStatus(m_root->m_state, source.owner.rootId, source.owner.projectId);
	if (pendingOwner != QStringLiteral("ok")) return {pendingOwner, {}};
	const auto pendingTarget = pendingTargetStatus(source.owner.rootId, evidence.target);
	if (pendingTarget != QStringLiteral("ok")) return {pendingTarget, {}};
	const auto tx = evidence.transactionId;
	const QJsonObject value{{"schema",2},{"transaction_id",tx},{"root_id",source.owner.rootId},
		{"project_id",source.owner.projectId},{"source_path",source.path},{"source_path_hash",sourceHash},
		{"source_file_id",source.owner.fileIdentity},{"source_sha256",source.owner.documentSha256},
		{"target_path_hash",targetHash},{"destination_missing",evidence.originalMissing},
		{"destination_file_id",QString::fromLatin1(evidence.originalIdentity.toHex())},
		{"destination_sha256",QString::fromLatin1(evidence.originalSha256.toHex())},
		{"staged_file_id",QString::fromLatin1(evidence.stageIdentity.toHex())},
		{"staged_sha256",QString::fromLatin1(evidence.stageSha256.toHex())}};
	const auto plain = QJsonDocument(value).toJson(QJsonDocument::Compact);
	const auto encrypted = protectRecord(plain, saveJournalEntropy(source.owner.rootId, tx));
	if (!encrypted) return {QStringLiteral("asset_write_failed"), {}};
	const auto bytes = QJsonDocument(QJsonObject{{"schema",2},{"transaction_id",tx},
		{"protected",QString::fromLatin1(encrypted->toBase64())}}).toJson(QJsonDocument::Compact);
	if (bytes.size() > MaxRecordBytes) return {QStringLiteral("asset_write_failed"), {}};
	const auto valid = [&] {
		ProjectSavePublication::Evidence retained;
		return m_root->isAuthorized() && source.root->isAuthorized() && source.hold
			&& destination.stagedEvidence(retained) && retained.target == evidence.target && retained.transactionId == tx
			&& retained.originalMissing == evidence.originalMissing
			&& retained.originalIdentity == evidence.originalIdentity && retained.originalSha256 == evidence.originalSha256
			&& retained.stageIdentity == evidence.stageIdentity && retained.stageSha256 == evidence.stageSha256;
	};
	for (const auto& name : {saveTargetCredential(source.owner.rootId, targetHash),
		saveOwnerCredential(source.owner.rootId, source.owner.projectId), saveJournalCredential(source.owner.rootId, tx)})
	{
		if (!valid()) return {QStringLiteral("asset_incomplete"), {}};
		const auto code = createCredential(name, bytes); if (code != QStringLiteral("ok")) return {code, {}};
	}
	for (const auto& name : {saveOwnerName(source.owner.projectId), saveJournalName(tx)})
	{
		if (!valid()) return {QStringLiteral("asset_incomplete"), {}};
		const auto code = publishRecord(m_root->m_state, name, bytes); if (code != QStringLiteral("ok")) return {code, {}};
	}
	for (const auto& name : {saveTargetCredential(source.owner.rootId, targetHash),
		saveOwnerCredential(source.owner.rootId, source.owner.projectId), saveJournalCredential(source.owner.rootId, tx)})
	{
		const auto stored = readCredential(name);
		if (stored.code != QStringLiteral("ok") || stored.bytes != bytes) return {QStringLiteral("asset_integrity_failed"), {}};
	}
	for (const auto& name : {saveOwnerName(source.owner.projectId), saveJournalName(tx)})
	{
		std::shared_ptr<managed_vocal_detail::FileState> file;
		const auto code = managed_vocal_detail::openReadFile(m_root->m_state, name, file);
		if (code != QStringLiteral("ok")) return {code, {}};
		const auto length = managed_vocal_detail::size(file);
		if (length != bytes.size()) return {QStringLiteral("asset_integrity_failed"), {}};
		QByteArray stored(int(length), Qt::Uninitialized);
		if (managed_vocal_detail::read(file, stored.data(), length) != length || stored != bytes)
			return {QStringLiteral("asset_integrity_failed"), {}};
	}
	const auto decoded = unprotectRecord(*encrypted, saveJournalEntropy(source.owner.rootId, tx));
	if (!decoded || *decoded != plain || !valid()) return {QStringLiteral("asset_integrity_failed"), {}};
	ManagedVocalSaveAsJournal result;
	result.transactionId = tx; result.sourcePathHash = sourceHash; result.targetPathHash = targetHash; result.source = source.owner;
	result.destinationMissing = evidence.originalMissing;
	result.destinationFileIdentity = QString::fromLatin1(evidence.originalIdentity.toHex());
	result.destinationSha256 = QString::fromLatin1(evidence.originalSha256.toHex());
	result.stagedFileIdentity = QString::fromLatin1(evidence.stageIdentity.toHex());
	result.stagedSha256 = QString::fromLatin1(evidence.stageSha256.toHex());
	return {QStringLiteral("ok"), std::move(result)};
}

ManagedVocalProjectOwnerStore::Result ManagedVocalProjectOwnerStore::bindPublishedSaveAs(
	const ManagedVocalSaveAsSource& source, const ProjectSavePublication& destination) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}};
	ProjectSavePublication::Evidence evidence;
	if (!source.hold || !source.root || source.root.get() != m_root.get() || !source.root->isAuthorized()
		|| !destination.publishedEvidence(evidence)) return {QStringLiteral("project_identity_unverified"), {}};
	const auto found = readSaveAsJournal(m_root->m_state, m_root->rootId(), evidence.target, evidence.transactionId);
	if (!found.journal) return {found.code, {}};
	const auto& journal = *found.journal;
	if (found.sourcePath != source.path || journal.source.projectId != source.owner.projectId
		|| journal.source.rootId != source.owner.rootId || journal.source.fileIdentity != source.owner.fileIdentity
		|| journal.source.documentSha256 != source.owner.documentSha256 || journal.destinationMissing != evidence.originalMissing
		|| journal.destinationFileIdentity.toLatin1() != evidence.originalIdentity.toHex()
		|| journal.destinationSha256.toLatin1() != evidence.originalSha256.toHex()
		|| journal.stagedFileIdentity.toLatin1() != evidence.stageIdentity.toHex()
		|| journal.stagedSha256.toLatin1() != evidence.stageSha256.toHex()) return {QStringLiteral("project_identity_unverified"), {}};
	Snapshot old; old.fileId = source.owner.fileIdentity; old.digest = source.owner.documentSha256;
	const auto binding = readBinding(m_root->m_state, source.owner.rootId, old);
	if (!binding.owner) return binding;
	if (binding.owner->projectId != source.owner.projectId) return {QStringLiteral("asset_integrity_failed"), {}};
	Snapshot next; next.fileId = journal.stagedFileIdentity; next.digest = journal.stagedSha256;
	const auto valid = [&] {
		ProjectSavePublication::Evidence retained;
		return m_root->isAuthorized() && source.root->isAuthorized() && source.hold
			&& destination.publishedEvidence(retained) && retained.target == evidence.target
			&& retained.transactionId == evidence.transactionId && retained.originalMissing == evidence.originalMissing
			&& retained.originalIdentity == evidence.originalIdentity && retained.originalSha256 == evidence.originalSha256
			&& retained.stageIdentity == evidence.stageIdentity && retained.stageSha256 == evidence.stageSha256;
	};
	return bindSnapshot(m_root->m_state, source.owner.rootId, next, source.owner.projectId, valid);
}

QString ManagedVocalProjectOwnerStore::finishPublishedSaveAs(
	const ManagedVocalSaveAsSource& source, const ProjectSavePublication& destination) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return QStringLiteral("asset_root_revoked");
	ProjectSavePublication::Evidence evidence;
	if (!source.hold || !source.root || source.root.get() != m_root.get() || !source.root->isAuthorized()
		|| !destination.publishedEvidence(evidence)) return QStringLiteral("project_identity_unverified");
	const auto found = readSaveAsJournal(m_root->m_state, m_root->rootId(), evidence.target, evidence.transactionId);
	if (!found.journal) return found.code;
	const auto& journal = *found.journal;
	if (found.sourcePath != source.path || journal.source.projectId != source.owner.projectId
		|| journal.source.rootId != source.owner.rootId || journal.source.fileIdentity != source.owner.fileIdentity
		|| journal.source.documentSha256 != source.owner.documentSha256 || journal.destinationMissing != evidence.originalMissing
		|| journal.destinationFileIdentity.toLatin1() != evidence.originalIdentity.toHex()
		|| journal.destinationSha256.toLatin1() != evidence.originalSha256.toHex()
		|| journal.stagedFileIdentity.toLatin1() != evidence.stageIdentity.toHex()
		|| journal.stagedSha256.toLatin1() != evidence.stageSha256.toHex()) return QStringLiteral("project_identity_unverified");
	Snapshot next; next.fileId = journal.stagedFileIdentity; next.digest = journal.stagedSha256;
	const auto bound = readBinding(m_root->m_state, source.owner.rootId, next);
	if (!bound.owner) return bound.code;
	if (bound.owner->projectId != source.owner.projectId) return QStringLiteral("asset_integrity_failed");
	const auto terminal = completedSaveCredential(source.owner.rootId, journal.targetPathHash);
	const auto created = createCredential(terminal, found.bytes); if (created != QStringLiteral("ok")) return created;
	const auto valid = [&] {
		ProjectSavePublication::Evidence retained;
		return m_root->isAuthorized() && source.root->isAuthorized() && source.hold
			&& destination.publishedEvidence(retained) && retained.target == evidence.target
			&& retained.transactionId == evidence.transactionId && retained.originalMissing == evidence.originalMissing
			&& retained.originalIdentity == evidence.originalIdentity && retained.originalSha256 == evidence.originalSha256
			&& retained.stageIdentity == evidence.stageIdentity && retained.stageSha256 == evidence.stageSha256;
	};
	for (const auto& name : {saveJournalName(evidence.transactionId), saveOwnerName(source.owner.projectId)})
	{
		if (!valid()) return QStringLiteral("asset_incomplete");
		const auto removed = managed_vocal_detail::removeMatchingFile(m_root->m_state, name, found.bytes);
		if (removed != QStringLiteral("ok")) return removed;
	}
	for (const auto& name : {saveTargetCredential(source.owner.rootId, journal.targetPathHash),
		saveJournalCredential(source.owner.rootId, evidence.transactionId), saveOwnerCredential(source.owner.rootId, source.owner.projectId)})
	{
		if (!valid()) return QStringLiteral("asset_incomplete");
		const auto removed = removeMatchingCredential(name, found.bytes); if (removed != QStringLiteral("ok")) return removed;
	}
	if (!valid()) return QStringLiteral("asset_incomplete");
	return removeMatchingCredential(terminal, found.bytes);
}

QString ManagedVocalProjectOwnerStore::recoverPublishedSaveAs(const QString& destinationPath) const
{
	return recoverSaveAs(destinationPath, true);
}
QString ManagedVocalProjectOwnerStore::recoverUnpublishedSaveAs(const QString& destinationPath) const
{
	return recoverSaveAs(destinationPath, false);
}

QString ManagedVocalProjectOwnerStore::recoverSaveAs(const QString& destinationPath, bool published) const
{
	const std::lock_guard lock(ownerStoreMutex);
	if (!m_root || !m_root->isAuthorized()) return QStringLiteral("asset_root_revoked");
	const auto rootId = m_root->rootId(); const auto targetHash = saveTargetHash(destinationPath);
	if (targetHash.isEmpty()) return QStringLiteral("project_identity_unverified");
	const auto receiptName = published ? completedSaveCredential(rootId, targetHash) : abortedSaveCredential(rootId, targetHash);
	auto receipt = readCredential(receiptName);
	const bool needsReceipt = receipt.code == QStringLiteral("asset_missing");
	if (needsReceipt) receipt = readCredential(saveTargetCredential(rootId, targetHash));
	if (receipt.code != QStringLiteral("ok")) return receipt.code;
	QJsonParseError error{}; const auto envelope = QJsonDocument::fromJson(receipt.bytes, &error);
	const auto tx = envelope.object().value(QStringLiteral("transaction_id")).toString();
	if (error.error != QJsonParseError::NoError || !envelope.isObject() || !canonicalUuid(tx)) return QStringLiteral("asset_integrity_failed");
	const auto decoded = decodeSaveAsJournal(receipt.bytes, rootId, destinationPath, tx);
	if (!decoded.journal) return decoded.code;
	const auto& journal = *decoded.journal;
	if (needsReceipt && published)
	{
		const auto complete = readSaveAsJournal(m_root->m_state, rootId, destinationPath, tx);
		if (!complete.journal || complete.bytes != receipt.bytes) return complete.journal ? QStringLiteral("asset_integrity_failed") : complete.code;
	}
	auto source = snapshot(decoded.sourcePath, m_root->m_state);
	if (!source || source->fileId != journal.source.fileIdentity || source->digest != journal.source.documentSha256)
		return QStringLiteral("project_identity_unverified");
	const auto sourceBinding = readBinding(m_root->m_state, rootId, *source);
	if (!sourceBinding.owner || sourceBinding.owner->projectId != journal.source.projectId) return QStringLiteral("asset_integrity_failed");
	bool targetMissing = false;
	auto target = snapshot(destinationPath, m_root->m_state, &targetMissing);
	const auto directory = QFileInfo(destinationPath).absoluteDir();
	const auto stagePath = directory.filePath(QStringLiteral(".lmms-vocal-stage-") + tx);
	const auto backupPath = directory.filePath(QStringLiteral(".lmms-vocal-backup-") + tx);
	std::optional<Snapshot> auxiliary;
	if (published)
	{
		if (!target || target->fileId != journal.stagedFileIdentity || target->digest != journal.stagedSha256)
			return QStringLiteral("project_identity_unverified");
		if (!journal.destinationMissing)
		{
			auxiliary = snapshot(backupPath, m_root->m_state);
			if (!auxiliary || auxiliary->fileId != journal.destinationFileIdentity || auxiliary->digest != journal.destinationSha256)
				return QStringLiteral("project_identity_unverified");
		}
	}
	else
	{
		if (journal.destinationMissing ? (!targetMissing || target.has_value())
			: (!target || target->fileId != journal.destinationFileIdentity || target->digest != journal.destinationSha256))
			return QStringLiteral("project_identity_unverified");
		auxiliary = snapshot(stagePath, m_root->m_state);
		if (!auxiliary || auxiliary->fileId != journal.stagedFileIdentity || auxiliary->digest != journal.stagedSha256)
			return QStringLiteral("project_identity_unverified");
	}
	if (!published || journal.destinationMissing)
	{
		bool backupMissing = false; const auto backup = snapshot(backupPath, m_root->m_state, &backupMissing);
		if (backup || !backupMissing) return QStringLiteral("project_identity_unverified");
	}
	const auto valid = [&] {
		return m_root->isAuthorized() && (source->pin || source->rootPin)
			&& (!target || target->pin || target->rootPin) && (!auxiliary || auxiliary->pin || auxiliary->rootPin);
	};
	if (published)
	{
		const auto bound = bindSnapshot(m_root->m_state, rootId, *target, journal.source.projectId, valid);
		if (!bound.owner) return bound.code;
	}
	const QStringList files{saveJournalName(tx), saveOwnerName(journal.source.projectId)};
	const QStringList credentials{saveTargetCredential(rootId, targetHash), saveJournalCredential(rootId, tx),
		saveOwnerCredential(rootId, journal.source.projectId)};
	for (const auto& name : files)
	{
		std::shared_ptr<managed_vocal_detail::FileState> file; const auto code = managed_vocal_detail::openReadFile(m_root->m_state, name, file);
		if (code == QStringLiteral("asset_missing")) continue;
		if (code != QStringLiteral("ok")) return code;
		const auto length = managed_vocal_detail::size(file); if (length != receipt.bytes.size()) return QStringLiteral("asset_integrity_failed");
		QByteArray bytes(int(length), Qt::Uninitialized);
		if (managed_vocal_detail::read(file, bytes.data(), length) != length || bytes != receipt.bytes) return QStringLiteral("asset_integrity_failed");
	}
	for (const auto& name : credentials)
	{
		const auto found = readCredential(name); if (found.code == QStringLiteral("asset_missing")) continue;
		if (found.code != QStringLiteral("ok")) return found.code;
		if (found.bytes != receipt.bytes) return QStringLiteral("asset_integrity_failed");
	}
	if (needsReceipt)
	{
		if (!valid()) return QStringLiteral("asset_incomplete");
		const auto created = createCredential(receiptName, receipt.bytes); if (created != QStringLiteral("ok")) return created;
	}
	for (const auto& name : files)
	{
		if (!valid()) return QStringLiteral("asset_incomplete");
		const auto removed = managed_vocal_detail::removeMatchingFile(m_root->m_state, name, receipt.bytes);
		if (removed != QStringLiteral("ok") && removed != QStringLiteral("asset_missing")) return removed;
	}
	for (const auto& name : credentials)
	{
		if (!valid()) return QStringLiteral("asset_incomplete");
		const auto removed = removeMatchingCredential(name, receipt.bytes);
		if (removed != QStringLiteral("ok") && removed != QStringLiteral("asset_missing")) return removed;
	}
	if (!valid()) return QStringLiteral("asset_incomplete");
	return removeMatchingCredential(receiptName, receipt.bytes);
}
}
