#include "ProjectSavePublication.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QUuid>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#include <wincred.h>
#include <winternl.h>
#endif

namespace lmms {
namespace {
constexpr qint64 MaxBytes = 64LL * 1024 * 1024;
#ifdef Q_OS_WIN
QString cleanPath(QString path)
{
	path.replace('\\', '/');
	if (path.size() < 4 || path.size() > 1024 || path[1] != ':' || path[2] != '/'
		|| !((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))) return {};
	path[0] = path[0].toUpper();
	for (const auto& part : path.mid(3).split('/'))
	{
		if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')) return {};
		for (int i = 0; i < part.size(); ++i)
		{
			const auto c = part[i];
			if (c.unicode() < 32 || QStringLiteral(":*?\"<>|").contains(c)) return {};
			if (c.isHighSurrogate()) { if (++i >= part.size() || !part[i].isLowSurrogate()) return {}; }
			else if (c.isLowSurrogate()) return {};
		}
		const auto stem = part.section('.', 0, 0).toUpper();
		if (QStringList{"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}.contains(stem)
			|| (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT"))
				&& QStringLiteral("123456789¹²³").contains(stem[3]))) return {};
	}
	return path;
}
QString finalName(HANDLE h)
{
	std::array<wchar_t, 1025> b{};
	const auto n = GetFinalPathNameByHandleW(h, b.data(), DWORD(b.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
	return n && n < b.size() ? QString::fromWCharArray(b.data(), int(n)).replace('\\', '/') : QString{};
}
bool ordinary(HANDLE h, bool dir, DWORD volume)
{
	BY_HANDLE_FILE_INFORMATION i{};
	return GetFileType(h) == FILE_TYPE_DISK && GetFileInformationByHandle(h, &i)
		&& i.dwVolumeSerialNumber == volume && !(i.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		&& bool(i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == dir && (dir || i.nNumberOfLinks == 1);
}
HANDLE relative(HANDLE parent, const QString& name, ACCESS_MASK access, ULONG disposition, bool dir, ULONG share,
	NTSTATUS* resultStatus = nullptr)
{
	const auto fn = reinterpret_cast<decltype(&NtCreateFile)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
	if (!fn) { if (resultStatus) *resultStatus = NTSTATUS(0xc0000002u); return INVALID_HANDLE_VALUE; }
	UNICODE_STRING part{}; part.Buffer = const_cast<PWSTR>(reinterpret_cast<LPCWSTR>(name.utf16()));
	part.Length = USHORT(name.size() * sizeof(wchar_t)); part.MaximumLength = part.Length;
	OBJECT_ATTRIBUTES a{}; a.Length = sizeof(a); a.RootDirectory = parent; a.ObjectName = &part; a.Attributes = OBJ_DONT_REPARSE;
	IO_STATUS_BLOCK io{}; HANDLE h = nullptr;
	const auto status = fn(&h, access | SYNCHRONIZE, &a, &io, nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition,
		FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT | (dir ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE), nullptr, 0);
	if (resultStatus) *resultStatus = status;
	return status >= 0 ? h : INVALID_HANDLE_VALUE;
}
bool renameNoReplace(HANDLE file, HANDLE parent, const QString& name)
{
	const auto fn = reinterpret_cast<decltype(&NtSetInformationFile)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
	if (!fn) return false;
	const auto bytes = std::size_t(name.size()) * sizeof(wchar_t);
	std::vector<char> buffer(offsetof(FILE_RENAME_INFORMATION, FileName) + bytes, 0);
	auto* data = reinterpret_cast<FILE_RENAME_INFORMATION*>(buffer.data());
	data->ReplaceIfExists = FALSE; data->RootDirectory = parent; data->FileNameLength = ULONG(bytes);
	std::memcpy(data->FileName, name.utf16(), bytes); IO_STATUS_BLOCK io{};
	return fn(file, &io, data, ULONG(buffer.size()), FileRenameInformation) >= 0;
}
bool validUuid(const QString& text)
{
	const auto id = QUuid(text); return !id.isNull() && id.toString(QUuid::WithoutBraces) == text;
}
#endif
}

struct ProjectSavePublication::State
{
#ifdef Q_OS_WIN
	std::vector<HANDLE> directories;
	HANDLE original = INVALID_HANDLE_VALUE, stage = INVALID_HANDLE_VALUE, previousBackup = INVALID_HANDLE_VALUE;
	HANDLE nativeJournal = INVALID_HANDLE_VALUE;
	DWORD volume = 0;
	DWORD owningThread = GetCurrentThreadId();
	QString parentPath, targetName, stageName, backupName, tx, targetPath;
	QByteArray originalIdentity, originalSha256, stageIdentity, stageSha256;
	QString stageNativeFileId;
	QByteArray nativeRecord;
	QString nativeCredentialName;
	bool stageReady = false, moved = false, published = false, nativeReady = false, terminalVerified = false, originalMissing = false;
	~State()
	{
		if (nativeJournal != INVALID_HANDLE_VALUE) CloseHandle(nativeJournal);
		if (stage != INVALID_HANDLE_VALUE) CloseHandle(stage);
		if (original != INVALID_HANDLE_VALUE) CloseHandle(original);
		if (previousBackup != INVALID_HANDLE_VALUE) CloseHandle(previousBackup);
		for (auto i = directories.rbegin(); i != directories.rend(); ++i) CloseHandle(*i);
	}
	bool valid(HANDLE h, const QString& name) const
	{
		return GetCurrentThreadId() == owningThread && !directories.empty() && ordinary(directories.back(), true, volume)
			&& finalName(directories.back()) == parentPath && h != INVALID_HANDLE_VALUE
			&& ordinary(h, false, volume) && finalName(h) == parentPath + '/' + name;
	}
	bool targetAbsent() const
	{
		if (GetCurrentThreadId() != owningThread || directories.empty()
			|| !ordinary(directories.back(), true, volume) || finalName(directories.back()) != parentPath) return false;
		NTSTATUS status = 0;
		const auto h = relative(directories.back(), targetName, GENERIC_READ, FILE_OPEN, false,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &status);
		if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return false; }
		return quint32(status) == 0xc0000034u;
	}
	bool originalValid() const { return originalMissing ? targetAbsent() : valid(original, targetName); }
	bool pinPreviousBackup()
	{
		const auto name = targetName + QStringLiteral(".bak"); NTSTATUS status = 0;
		previousBackup = relative(directories.back(), name, GENERIC_READ, FILE_OPEN, false, FILE_SHARE_READ, &status);
		return previousBackup == INVALID_HANDLE_VALUE ? quint32(status) == 0xc0000034u : valid(previousBackup, name);
	}
#endif
};
ProjectSavePublication::ProjectSavePublication(std::unique_ptr<State> s) : m_state(std::move(s)) {}
ProjectSavePublication::~ProjectSavePublication() = default;

QString ProjectSavePublication::publishNativePending()
{
#ifdef Q_OS_WIN
	const auto fail = [] { return QStringLiteral("project_recovery_required"); };
	if (!m_state || !m_state->nativeReady || m_state->moved || m_state->published
		|| !m_state->originalValid() || !m_state->valid(m_state->stage, m_state->stageName)
		|| !nativeWitnessValid()) return fail();
	if (m_state->originalMissing)
	{
		if (!renameNoReplace(m_state->stage, m_state->directories.back(), m_state->targetName)) return fail();
		m_state->published = true;
	}
	else if (moveOriginalToBackup(m_state->tx) != QStringLiteral("ok")) return fail();
	else if (publishStage() != QStringLiteral("ok"))
	{
		restoreOriginal();
		return fail();
	}
	return m_state->valid(m_state->stage, m_state->targetName) && nativeWitnessValid()
		? QStringLiteral("ok") : fail();
#else
	return QStringLiteral("unsupported_backend");
#endif
}
QString ProjectSavePublication::finalizeNativePublication()
{
#ifdef Q_OS_WIN
	const auto fail = [] { return QStringLiteral("project_recovery_required"); };
	if (!m_state || !m_state->nativeReady || !m_state->published
		|| (!m_state->originalMissing && !m_state->valid(m_state->original, m_state->backupName))
		|| !m_state->valid(m_state->stage, m_state->targetName) || !nativeWitnessValid()) return fail();
	FILE_DISPOSITION_INFO disposition{TRUE};
	if (!SetFileInformationByHandle(m_state->nativeJournal, FileDispositionInfo, &disposition, sizeof(disposition))) return fail();
	CloseHandle(m_state->nativeJournal); m_state->nativeJournal = INVALID_HANDLE_VALUE;
	if (!CredDeleteW(reinterpret_cast<LPCWSTR>(m_state->nativeCredentialName.utf16()), CRED_TYPE_GENERIC, 0)) return fail();
	m_state->nativeReady = false;
	m_state->terminalVerified = true;
	return QStringLiteral("ok");
#else
	return QStringLiteral("unsupported_backend");
#endif
}

ProjectSavePublication::Result ProjectSavePublication::acquireSelectedTarget(const QString& target)
{
	return acquireImpl(target, {}, {}, false);
}

ProjectSavePublication::Result ProjectSavePublication::acquireNewTarget(const QString& target)
{
#ifdef Q_OS_WIN
	const auto denied = [] { return Result{QStringLiteral("project_identity_unverified"), {}}; };
	auto s = openParent(target); if (!s) return denied();
	if (!s->pinPreviousBackup() || !s->targetAbsent()) return denied();
	s->originalMissing = true;
	return {QStringLiteral("ok"), std::unique_ptr<ProjectSavePublication>(new ProjectSavePublication(std::move(s)))};
#else
	Q_UNUSED(target); return {QStringLiteral("unsupported_backend"), {}};
#endif
}

QString ProjectSavePublication::nativeJournalStatus(const QString& target)
{
#ifdef Q_OS_WIN
	auto parent = openParent(target); if (!parent) return QStringLiteral("project_status_unavailable");
	NTSTATUS status = 0;
	const auto marker = relative(parent->directories.back(), QStringLiteral(".lmms-native-save-journal-v1"),
		GENERIC_READ, FILE_OPEN, false, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &status);
	if (marker != INVALID_HANDLE_VALUE) { CloseHandle(marker); return QStringLiteral("pending"); }
	if (quint32(status) != 0xc0000034u) return QStringLiteral("pending");
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(parent->directories.back(), &info)) return QStringLiteral("project_status_unavailable");
	const auto material = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
		+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
	const auto parentId = QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex();
	const auto name = QStringLiteral("LMMS/NativeProjectSave/v1/") + QString::fromLatin1(parentId);
	PCREDENTIALW credential = nullptr;
	if (CredReadW(reinterpret_cast<LPCWSTR>(name.utf16()), CRED_TYPE_GENERIC, 0, &credential))
	{
		CredFree(credential); return QStringLiteral("pending");
	}
	return GetLastError() == ERROR_NOT_FOUND ? QStringLiteral("none") : QStringLiteral("project_status_unavailable");
#else
	Q_UNUSED(target); return QStringLiteral("unsupported_backend");
#endif
}

QString ProjectSavePublication::commitNativeSave()
{
	const auto published = publishNativePending();
	return published == QStringLiteral("ok") ? finalizeNativePublication() : published;
}

bool ProjectSavePublication::nativeWitnessValid() const
{
#ifdef Q_OS_WIN
	if (!m_state || !m_state->nativeReady
		|| !m_state->valid(m_state->nativeJournal, QStringLiteral(".lmms-native-save-journal-v1"))) return false;
	LARGE_INTEGER length{}, zero{}; DWORD count = 0;
	if (!GetFileSizeEx(m_state->nativeJournal, &length) || length.QuadPart != m_state->nativeRecord.size()
		|| !SetFilePointerEx(m_state->nativeJournal, zero, nullptr, FILE_BEGIN)) return false;
	QByteArray marker(m_state->nativeRecord.size(), Qt::Uninitialized);
	if (!ReadFile(m_state->nativeJournal, marker.data(), DWORD(marker.size()), &count, nullptr)
		|| count != DWORD(marker.size()) || marker != m_state->nativeRecord) return false;
	PCREDENTIALW credential = nullptr;
	if (!CredReadW(reinterpret_cast<LPCWSTR>(m_state->nativeCredentialName.utf16()), CRED_TYPE_GENERIC, 0, &credential)) return false;
	const bool matches = credential && credential->CredentialBlob && credential->CredentialBlobSize == DWORD(marker.size())
		&& std::memcmp(credential->CredentialBlob, marker.constData(), size_t(marker.size())) == 0;
	if (credential) CredFree(credential);
	return matches;
#else
	return false;
#endif
}

QString ProjectSavePublication::resolveNativeJournal(const QString& target)
{
#ifdef Q_OS_WIN
	const auto denied = [] { return QStringLiteral("project_recovery_required"); };
	auto parent = openParent(target); if (!parent) return denied();
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(parent->directories.back(), &info)) return denied();
	const auto material = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
		+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
	const auto parentId = QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex();
	const auto credentialName = QStringLiteral("LMMS/NativeProjectSave/v1/") + QString::fromLatin1(parentId);
	PCREDENTIALW credential = nullptr;
	if (!CredReadW(reinterpret_cast<LPCWSTR>(credentialName.utf16()), CRED_TYPE_GENERIC, 0, &credential)) return denied();
	if (!credential || !credential->CredentialBlob || !credential->CredentialBlobSize || credential->CredentialBlobSize > 2048)
	{
		if (credential) CredFree(credential);
		return denied();
	}
	QByteArray bytes(reinterpret_cast<const char*>(credential->CredentialBlob), int(credential->CredentialBlobSize));
	CredFree(credential);
	const auto markerName = QStringLiteral(".lmms-native-save-journal-v1"); NTSTATUS status = 0;
	parent->nativeJournal = relative(parent->directories.back(), markerName, GENERIC_READ | DELETE,
		FILE_OPEN, false, FILE_SHARE_READ, &status);
	const bool markerMissing = parent->nativeJournal == INVALID_HANDLE_VALUE && quint32(status) == 0xc0000034u;
	if (parent->nativeJournal == INVALID_HANDLE_VALUE && !markerMissing) return denied();
	if (!markerMissing)
	{
		if (!parent->valid(parent->nativeJournal, markerName)) return denied();
		LARGE_INTEGER length{}; DWORD count = 0;
		if (!GetFileSizeEx(parent->nativeJournal, &length) || length.QuadPart != bytes.size()) return denied();
		QByteArray marker(bytes.size(), Qt::Uninitialized);
		if (!ReadFile(parent->nativeJournal, marker.data(), DWORD(marker.size()), &count, nullptr)
			|| count != DWORD(marker.size()) || marker != bytes) return denied();
	}
	auto entropy = QCryptographicHash::hash(QByteArrayLiteral("lmms-native-project-save-v1:") + parentId, QCryptographicHash::Sha256);
	DATA_BLOB input{DWORD(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
	DATA_BLOB extra{DWORD(entropy.size()), reinterpret_cast<BYTE*>(entropy.data())}; DATA_BLOB plain{};
	if (!CryptUnprotectData(&input, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) return denied();
	if (!plain.pbData || !plain.cbData || plain.cbData > 2048)
	{
		if (plain.pbData) LocalFree(plain.pbData);
		return denied();
	}
	const QByteArray raw(reinterpret_cast<const char*>(plain.pbData), int(plain.cbData));
	SecureZeroMemory(plain.pbData, plain.cbData); LocalFree(plain.pbData);
	QJsonParseError error{}; const auto document = QJsonDocument::fromJson(raw, &error); const auto object = document.object();
	const bool originalMissing = object.value("schema") == QJsonValue(2)
		&& object.size() == 9 && object.value("original_missing") == QJsonValue(true);
	if (error.error != QJsonParseError::NoError || !document.isObject()
		|| (!originalMissing && (object.size() != 8 || object.value("schema") != QJsonValue(1)))
		|| QJsonDocument(object).toJson(QJsonDocument::Compact) != raw
		|| object.value("parent_id") != QJsonValue(QString::fromLatin1(parentId))
		|| object.value("target") != QJsonValue(parent->targetName)) return denied();
	const auto tx = object.value("transaction_id").toString();
	const auto readDigest = [&](const char* field) {
		const auto text = object.value(QLatin1String(field)).toString().toLatin1(); const auto value = QByteArray::fromHex(text);
		return text.size() == 64 && value.size() == 32 && value.toHex() == text ? value : QByteArray{};
	};
	const auto originalId = readDigest("original_id"), originalSha = readDigest("original_sha256");
	const auto stageId = readDigest("stage_id"), stageSha = readDigest("stage_sha256");
	if (!validUuid(tx) || stageId.isEmpty() || stageSha.isEmpty()
		|| (!originalMissing && (originalId.isEmpty() || originalSha.isEmpty() || originalId == stageId))
		|| (originalMissing && (object.value("original_id") != QJsonValue(QString{})
			|| object.value("original_sha256") != QJsonValue(QString{})))) return denied();
	const auto slash = parent->targetPath.lastIndexOf('/');
	const auto backup = parent->targetPath.left(slash + 1) + QStringLiteral(".lmms-vocal-backup-") + tx;
	const auto stage = parent->targetPath.left(slash + 1) + QStringLiteral(".lmms-vocal-stage-") + tx;
	auto published = acquire(parent->targetPath, stageId, stageSha);
	Result original{}, staged{}, recovered{};
	QString outcome;
	if (originalMissing)
	{
		if (published.publication) outcome = QStringLiteral("committed");
		else
		{
			staged = acquire(stage, stageId, stageSha);
			if (!staged.publication || !parent->targetAbsent()) return denied();
			outcome = QStringLiteral("aborted");
		}
	}
	else if (published.publication)
	{
		original = acquire(backup, originalId, originalSha);
		if (!original.publication) return denied();
		outcome = QStringLiteral("committed");
	}
	else
	{
		// The marker must still be authenticated before moving any file.
		if (!markerMissing)
		{
			recovered = reopenForRecovery(parent->targetPath, tx, originalId, originalSha, stageId, stageSha);
			if (recovered.publication)
			{
				if (recovered.publication->restoreOriginal() != QStringLiteral("ok")) return denied();
				outcome = QStringLiteral("restored");
			}
		}
		if (outcome.isEmpty())
		{
			// This also covers interruption after the original was restored but
			// before the credential was cleared. Both exact files stay pinned.
			original = acquire(parent->targetPath, originalId, originalSha);
			staged = acquire(stage, stageId, stageSha);
			if (!original.publication || !staged.publication) return denied();
			NTSTATUS backupStatus = 0;
			const auto backupHandle = relative(parent->directories.back(), QStringLiteral(".lmms-vocal-backup-") + tx,
				GENERIC_READ, FILE_OPEN, false, FILE_SHARE_READ, &backupStatus);
			if (backupHandle != INVALID_HANDLE_VALUE) { CloseHandle(backupHandle); return denied(); }
			if (quint32(backupStatus) != 0xc0000034u) return denied();
			outcome = QStringLiteral("restored");
		}
	}
	if (!markerMissing)
	{
		if (!parent->valid(parent->nativeJournal, markerName)) return denied();
		FILE_DISPOSITION_INFO disposition{TRUE};
		if (!SetFileInformationByHandle(parent->nativeJournal, FileDispositionInfo, &disposition, sizeof(disposition))) return denied();
		CloseHandle(parent->nativeJournal); parent->nativeJournal = INVALID_HANDLE_VALUE;
	}
	if (!CredDeleteW(reinterpret_cast<LPCWSTR>(credentialName.utf16()), CRED_TYPE_GENERIC, 0)) return denied();
	return outcome;
#else
	Q_UNUSED(target); return QStringLiteral("unsupported_backend");
#endif
}

std::unique_ptr<ProjectSavePublication::State> ProjectSavePublication::openParent(const QString& target)
{
#ifdef Q_OS_WIN
	const auto path = cleanPath(target); if (path.isEmpty()) return {};
	const auto drive = path.left(3).replace('/', '\\');
	const auto driveType = GetDriveTypeW(reinterpret_cast<LPCWSTR>(drive.utf16()));
	if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE) return {};
	auto s = std::make_unique<State>();
	const auto anchor = CreateFileW(reinterpret_cast<LPCWSTR>(drive.utf16()), FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (anchor == INVALID_HANDLE_VALUE) return {};
	s->directories.push_back(anchor); BY_HANDLE_FILE_INFORMATION base{};
	if (!GetFileInformationByHandle(anchor, &base) || !ordinary(anchor, true, base.dwVolumeSerialNumber)) return {};
	s->volume = base.dwVolumeSerialNumber; auto expected = finalName(anchor);
	if (!expected.startsWith("//?/Volume{") || !expected.endsWith("}/")) return {};
	const auto volumeType = GetDriveTypeW(reinterpret_cast<LPCWSTR>(QString(expected).replace('/', '\\').utf16()));
	if (volumeType != DRIVE_FIXED && volumeType != DRIVE_REMOVABLE) return {};
	const auto parts = path.mid(3).split('/');
	for (int i = 0; i + 1 < parts.size(); ++i)
	{
		const auto access = FILE_READ_ATTRIBUTES | FILE_TRAVERSE | (i + 2 == parts.size() ? FILE_ADD_FILE : 0);
		const auto h = relative(s->directories.back(), parts[i], access, FILE_OPEN, true, FILE_SHARE_READ | FILE_SHARE_WRITE);
		if (h == INVALID_HANDLE_VALUE) return {};
		s->directories.push_back(h); expected += parts[i];
		if (!ordinary(h, true, s->volume) || finalName(h) != expected) return {};
		expected += '/';
	}
	s->parentPath = expected.endsWith('/') ? expected.chopped(1) : expected;
	s->targetName = parts.back(); s->targetPath = path;
	return s;
#else
	Q_UNUSED(target); return {};
#endif
}

QString ProjectSavePublication::restoreOriginalFromNativeJournal(const QString& target)
{
#ifdef Q_OS_WIN
	const auto denied = [] { return QStringLiteral("project_recovery_required"); };
	auto parent = openParent(target); if (!parent) return denied();
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(parent->directories.back(), &info)) return denied();
	const auto material = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
		+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
	const auto parentId = QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex();
	const auto name = QStringLiteral(".lmms-native-save-journal-v1");
	parent->nativeJournal = relative(parent->directories.back(), name, GENERIC_READ, FILE_OPEN, false, FILE_SHARE_READ);
	if (parent->nativeJournal == INVALID_HANDLE_VALUE || !parent->valid(parent->nativeJournal, name)) return denied();
	LARGE_INTEGER length{};
	if (!GetFileSizeEx(parent->nativeJournal, &length) || length.QuadPart <= 0 || length.QuadPart > 2048) return denied();
	QByteArray bytes(int(length.QuadPart), Qt::Uninitialized); DWORD received = 0;
	if (!ReadFile(parent->nativeJournal, bytes.data(), DWORD(bytes.size()), &received, nullptr)
		|| received != DWORD(bytes.size())) return denied();
	const auto credentialName = QStringLiteral("LMMS/NativeProjectSave/v1/") + QString::fromLatin1(parentId);
	PCREDENTIALW credential = nullptr;
	if (!CredReadW(reinterpret_cast<LPCWSTR>(credentialName.utf16()), CRED_TYPE_GENERIC, 0, &credential)) return denied();
	const bool matches = credential && credential->CredentialBlob && credential->CredentialBlobSize == DWORD(bytes.size())
		&& std::memcmp(credential->CredentialBlob, bytes.constData(), size_t(bytes.size())) == 0;
	if (credential) CredFree(credential);
	if (!matches) return denied();
	auto entropy = QCryptographicHash::hash(QByteArrayLiteral("lmms-native-project-save-v1:") + parentId, QCryptographicHash::Sha256);
	DATA_BLOB input{DWORD(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
	DATA_BLOB extra{DWORD(entropy.size()), reinterpret_cast<BYTE*>(entropy.data())}; DATA_BLOB plain{};
	if (!CryptUnprotectData(&input, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) return denied();
	if (!plain.pbData || !plain.cbData || plain.cbData > 2048)
	{
		if (plain.pbData) LocalFree(plain.pbData);
		return denied();
	}
	const QByteArray raw(reinterpret_cast<const char*>(plain.pbData), int(plain.cbData));
	SecureZeroMemory(plain.pbData, plain.cbData); LocalFree(plain.pbData);
	QJsonParseError error{}; const auto document = QJsonDocument::fromJson(raw, &error); const auto object = document.object();
	if (error.error != QJsonParseError::NoError || !document.isObject() || object.size() != 8
		|| QJsonDocument(object).toJson(QJsonDocument::Compact) != raw || object.value("schema") != QJsonValue(1)
		|| object.value("parent_id") != QJsonValue(QString::fromLatin1(parentId))
		|| object.value("target") != QJsonValue(parent->targetName)) return denied();
	const auto tx = object.value("transaction_id").toString();
	const auto readDigest = [&](const char* field) {
		const auto text = object.value(QLatin1String(field)).toString().toLatin1();
		const auto value = QByteArray::fromHex(text);
		return text.size() == 64 && value.size() == 32 && value.toHex() == text ? value : QByteArray{};
	};
	const auto originalId = readDigest("original_id"), originalSha = readDigest("original_sha256");
	const auto stageId = readDigest("stage_id"), stageSha = readDigest("stage_sha256");
	if (!validUuid(tx) || originalId.isEmpty() || originalSha.isEmpty() || stageId.isEmpty() || stageSha.isEmpty()) return denied();
	auto held = reopenForRecovery(parent->targetPath, tx, originalId, originalSha, stageId, stageSha);
	if (!held.publication || !parent->valid(parent->nativeJournal, name)) return denied();
	return held.publication->restoreOriginal();
#else
	Q_UNUSED(target); return QStringLiteral("unsupported_backend");
#endif
}

ProjectSavePublication::Result ProjectSavePublication::reopenForRecovery(const QString& target, const QString& tx,
	const QByteArray& originalIdentity, const QByteArray& originalSha256,
	const QByteArray& stageIdentity, const QByteArray& stageSha256)
{
#ifdef Q_OS_WIN
	const auto denied = [] { return Result{QStringLiteral("project_identity_unverified"), {}}; };
	const auto path = cleanPath(target);
	if (path.isEmpty() || !validUuid(tx) || stageIdentity.size() != 32 || stageSha256.size() != 32
		|| stageIdentity == originalIdentity) return denied();
	const auto slash = path.lastIndexOf('/');
	const auto backupName = QStringLiteral(".lmms-vocal-backup-") + tx;
	auto held = acquire(path.left(slash + 1) + backupName, originalIdentity, originalSha256);
	if (!held.publication) return held;
	auto& s = *held.publication->m_state;
	const auto stageName = QStringLiteral(".lmms-vocal-stage-") + tx;
	s.stage = relative(s.directories.back(), stageName, GENERIC_READ | DELETE, FILE_OPEN, false, FILE_SHARE_READ);
	if (s.stage == INVALID_HANDLE_VALUE || !s.valid(s.stage, stageName)) return denied();
	BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER length{};
	if (!GetFileInformationByHandle(s.stage, &info) || !GetFileSizeEx(s.stage, &length)
		|| length.QuadPart < 0 || length.QuadPart > MaxBytes) return denied();
	const auto material = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
		+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
	if (QCryptographicHash::hash(material, QCryptographicHash::Sha256) != stageIdentity) return denied();
	QCryptographicHash hash(QCryptographicHash::Sha256); std::array<char, 65536> block{}; qint64 consumed = 0;
	while (consumed < length.QuadPart)
	{
		DWORD read = 0; const auto count = DWORD(std::min<qint64>(block.size(), length.QuadPart - consumed));
		if (!ReadFile(s.stage, block.data(), count, &read, nullptr) || !read) return denied();
		hash.addData(block.data(), int(read)); consumed += read;
	}
	LARGE_INTEGER after{};
	if (!GetFileSizeEx(s.stage, &after) || after.QuadPart != length.QuadPart || hash.result() != stageSha256
		|| !s.valid(s.original, backupName) || !s.valid(s.stage, stageName)) return denied();
	s.backupName = backupName; s.targetName = path.mid(slash + 1); s.stageName = stageName;
	s.tx = tx; s.stageReady = true; s.moved = true;
	return held;
#else
	Q_UNUSED(target); Q_UNUSED(tx); Q_UNUSED(originalIdentity); Q_UNUSED(originalSha256);
	Q_UNUSED(stageIdentity); Q_UNUSED(stageSha256); return {QStringLiteral("unsupported_backend"), {}};
#endif
}

ProjectSavePublication::Result ProjectSavePublication::acquire(const QString& target, const QByteArray& identity, const QByteArray& digest)
{
	return acquireImpl(target, identity, digest, true);
}

ProjectSavePublication::Result ProjectSavePublication::acquireImpl(const QString& target, const QByteArray& identity,
	const QByteArray& digest, bool requireExpected)
{
#ifdef Q_OS_WIN
	const auto denied = [] { return Result{QStringLiteral("project_identity_unverified"), {}}; };
	if (requireExpected && (identity.size() != 32 || digest.size() != 32)) return denied();
	auto s = openParent(target); if (!s) return denied();
	// Pin an existing legacy backup before pinning the project. A legacy writer
	// otherwise deletes .bak before discovering that the project is locked.
	if (!s->pinPreviousBackup()) return denied();
	s->original = relative(s->directories.back(), s->targetName, GENERIC_READ | DELETE, FILE_OPEN, false, FILE_SHARE_READ);
	if (s->original == INVALID_HANDLE_VALUE || !s->valid(s->original, s->targetName)) return denied();
	BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER length{};
	if (!GetFileInformationByHandle(s->original, &info) || !GetFileSizeEx(s->original, &length)
		|| length.QuadPart < 0 || length.QuadPart > MaxBytes) return denied();
	const auto material = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
		+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
	const auto actualIdentity = QCryptographicHash::hash(material, QCryptographicHash::Sha256);
	if (requireExpected && actualIdentity != identity) return denied();
	QCryptographicHash hash(QCryptographicHash::Sha256); std::array<char, 65536> block{}; qint64 consumed = 0;
	while (consumed < length.QuadPart)
	{
		DWORD read = 0; const auto count = DWORD(std::min<qint64>(block.size(), length.QuadPart - consumed));
		if (!ReadFile(s->original, block.data(), count, &read, nullptr) || !read) return denied();
		hash.addData(block.data(), int(read)); consumed += read;
	}
	LARGE_INTEGER after{};
	const auto actualDigest = hash.result();
	if (!GetFileSizeEx(s->original, &after) || after.QuadPart != length.QuadPart
		|| (requireExpected && actualDigest != digest)
		|| !s->valid(s->original, s->targetName)) return denied();
	s->originalIdentity = actualIdentity; s->originalSha256 = actualDigest;
	return {QStringLiteral("ok"), std::unique_ptr<ProjectSavePublication>(new ProjectSavePublication(std::move(s)))};
#else
	Q_UNUSED(target); Q_UNUSED(identity); Q_UNUSED(digest); Q_UNUSED(requireExpected); return {QStringLiteral("unsupported_backend"), {}};
#endif
}

QString ProjectSavePublication::createStage(const QString& tx, const QByteArray& bytes)
{
#ifdef Q_OS_WIN
	if (!m_state || !validUuid(tx) || bytes.isEmpty() || bytes.size() > MaxBytes
		|| m_state->stage != INVALID_HANDLE_VALUE || !m_state->originalValid())
		return QStringLiteral("project_identity_unverified");
	const auto name = QStringLiteral(".lmms-vocal-stage-") + tx;
	const auto file = relative(m_state->directories.back(), name, GENERIC_READ | GENERIC_WRITE | DELETE,
		FILE_CREATE, false, FILE_SHARE_READ);
	if (file == INVALID_HANDLE_VALUE) return QStringLiteral("project_stage_failed");
	m_state->stage = file; m_state->stageName = name; m_state->tx = tx;
	if (!m_state->valid(file, name)) return QStringLiteral("project_stage_failed");
	qint64 written = 0;
	while (written < bytes.size())
	{
		DWORD count = 0;
		if (!WriteFile(file, bytes.constData() + written, DWORD(bytes.size() - written), &count, nullptr) || !count)
			return QStringLiteral("project_stage_failed");
		written += count;
	}
	if (!FlushFileBuffers(file) || !m_state->valid(file, name)) return QStringLiteral("project_stage_failed");
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(file, &info)) return QStringLiteral("project_stage_failed");
	const auto material = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
		+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
	m_state->stageIdentity = QCryptographicHash::hash(material, QCryptographicHash::Sha256);
	m_state->stageSha256 = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
	m_state->stageNativeFileId = QStringLiteral("%1:%2:%3")
		.arg(info.dwVolumeSerialNumber, 8, 16, QLatin1Char('0'))
		.arg(info.nFileIndexHigh, 8, 16, QLatin1Char('0'))
		.arg(info.nFileIndexLow, 8, 16, QLatin1Char('0'));
	m_state->stageReady = true;
	return QStringLiteral("ok");
#else
	Q_UNUSED(tx); Q_UNUSED(bytes); return QStringLiteral("unsupported_backend");
#endif
}
QString ProjectSavePublication::moveOriginalToBackup(const QString& tx)
{
#ifdef Q_OS_WIN
	if (!m_state || !m_state->stageReady || tx != m_state->tx || m_state->moved || !m_state->valid(m_state->original, m_state->targetName)
		|| !m_state->valid(m_state->stage, m_state->stageName)) return QStringLiteral("project_identity_unverified");
	const auto backup = QStringLiteral(".lmms-vocal-backup-") + tx;
	if (!renameNoReplace(m_state->original, m_state->directories.back(), backup)) return QStringLiteral("project_publish_failed");
	m_state->backupName = backup; m_state->moved = true;
	return m_state->valid(m_state->original, backup) ? QStringLiteral("ok") : QStringLiteral("project_integrity_failed");
#else
	Q_UNUSED(tx); return QStringLiteral("unsupported_backend");
#endif
}
QString ProjectSavePublication::prepareNativeJournal()
{
#ifdef Q_OS_WIN
	if (!m_state || !m_state->stageReady || m_state->moved || m_state->published
		|| m_state->nativeJournal != INVALID_HANDLE_VALUE
		|| !m_state->originalValid()
		|| !m_state->valid(m_state->stage, m_state->stageName)) return QStringLiteral("project_identity_unverified");
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(m_state->directories.back(), &info)) return QStringLiteral("project_identity_unverified");
	const auto material = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
		+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
	const auto parentId = QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex();
	const auto credentialName = QStringLiteral("LMMS/NativeProjectSave/v1/") + QString::fromLatin1(parentId);
	PCREDENTIALW existing = nullptr;
	if (CredReadW(reinterpret_cast<LPCWSTR>(credentialName.utf16()), CRED_TYPE_GENERIC, 0, &existing))
	{
		CredFree(existing); return QStringLiteral("project_recovery_required");
	}
	if (GetLastError() != ERROR_NOT_FOUND) return QStringLiteral("project_journal_failed");
	QJsonObject object{{"schema",m_state->originalMissing ? 2 : 1},{"parent_id",QString::fromLatin1(parentId)},
		{"target",m_state->targetName},{"transaction_id",m_state->tx},
		{"original_id",QString::fromLatin1(m_state->originalIdentity.toHex())},
		{"original_sha256",QString::fromLatin1(m_state->originalSha256.toHex())},
		{"stage_id",QString::fromLatin1(m_state->stageIdentity.toHex())},
		{"stage_sha256",QString::fromLatin1(m_state->stageSha256.toHex())}};
	if (m_state->originalMissing) object.insert(QStringLiteral("original_missing"), true);
	auto plain = QJsonDocument(object).toJson(QJsonDocument::Compact);
	auto entropy = QCryptographicHash::hash(QByteArrayLiteral("lmms-native-project-save-v1:") + parentId, QCryptographicHash::Sha256);
	DATA_BLOB input{DWORD(plain.size()), reinterpret_cast<BYTE*>(plain.data())};
	DATA_BLOB extra{DWORD(entropy.size()), reinterpret_cast<BYTE*>(entropy.data())}; DATA_BLOB protectedData{};
	if (!CryptProtectData(&input, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &protectedData))
		return QStringLiteral("project_journal_failed");
	if (!protectedData.pbData || !protectedData.cbData || protectedData.cbData > 2048)
	{
		if (protectedData.pbData) LocalFree(protectedData.pbData);
		return QStringLiteral("project_journal_failed");
	}
	const QByteArray bytes(reinterpret_cast<const char*>(protectedData.pbData), int(protectedData.cbData));
	LocalFree(protectedData.pbData);
	const auto name = QStringLiteral(".lmms-native-save-journal-v1");
	const auto marker = relative(m_state->directories.back(), name, GENERIC_READ | GENERIC_WRITE | DELETE,
		FILE_CREATE, false, FILE_SHARE_READ);
	if (marker == INVALID_HANDLE_VALUE) return QStringLiteral("project_recovery_required");
	m_state->nativeJournal = marker;
	if (!m_state->valid(marker, name)) return QStringLiteral("project_journal_failed");
	DWORD written = 0;
	if (!WriteFile(marker, bytes.constData(), DWORD(bytes.size()), &written, nullptr)
		|| written != DWORD(bytes.size()) || !FlushFileBuffers(marker)) return QStringLiteral("project_journal_failed");
	CREDENTIALW credential{}; credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(credentialName.utf16()));
	credential.CredentialBlobSize = DWORD(bytes.size());
	credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(bytes.constData()));
	credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	if (!CredWriteW(&credential, 0)) return QStringLiteral("project_journal_failed");
	PCREDENTIALW saved = nullptr;
	if (!CredReadW(reinterpret_cast<LPCWSTR>(credentialName.utf16()), CRED_TYPE_GENERIC, 0, &saved))
		return QStringLiteral("project_journal_failed");
	const bool matches = saved && saved->CredentialBlob && saved->CredentialBlobSize == DWORD(bytes.size())
		&& std::memcmp(saved->CredentialBlob, bytes.constData(), size_t(bytes.size())) == 0;
	if (saved) CredFree(saved);
	if (!matches || !m_state->valid(marker, name) || !m_state->originalValid()
		|| !m_state->valid(m_state->stage, m_state->stageName)) return QStringLiteral("project_journal_failed");
	m_state->nativeRecord = bytes; m_state->nativeCredentialName = credentialName; m_state->nativeReady = true;
	return QStringLiteral("ok");
#else
	return QStringLiteral("unsupported_backend");
#endif
}
QString ProjectSavePublication::publishStage()
{
#ifdef Q_OS_WIN
	if (m_state && m_state->originalMissing && m_state->stageReady && !m_state->published
		&& m_state->originalValid() && m_state->valid(m_state->stage, m_state->stageName))
	{
		if (!renameNoReplace(m_state->stage, m_state->directories.back(), m_state->targetName))
			return QStringLiteral("project_publish_failed");
		m_state->published = true;
		return m_state->valid(m_state->stage, m_state->targetName) ? QStringLiteral("ok") : QStringLiteral("project_integrity_failed");
	}
	if (!m_state || !m_state->moved || m_state->published || !m_state->valid(m_state->original, m_state->backupName)
		|| !m_state->valid(m_state->stage, m_state->stageName)) return QStringLiteral("project_identity_unverified");
	if (!renameNoReplace(m_state->stage, m_state->directories.back(), m_state->targetName)) return QStringLiteral("project_publish_failed");
	m_state->published = true;
	return m_state->valid(m_state->stage, m_state->targetName) ? QStringLiteral("ok") : QStringLiteral("project_integrity_failed");
#else
	return QStringLiteral("unsupported_backend");
#endif
}
QString ProjectSavePublication::restoreOriginal()
{
#ifdef Q_OS_WIN
	if (!m_state || !m_state->moved || m_state->published
		|| !m_state->valid(m_state->original, m_state->backupName)) return QStringLiteral("project_identity_unverified");
	if (!renameNoReplace(m_state->original, m_state->directories.back(), m_state->targetName))
		return QStringLiteral("project_publish_failed");
	m_state->moved = false;
	return m_state->valid(m_state->original, m_state->targetName) ? QStringLiteral("ok") : QStringLiteral("project_integrity_failed");
#else
	return QStringLiteral("unsupported_backend");
#endif
}
bool ProjectSavePublication::evidence(Evidence& output) const
{
#ifdef Q_OS_WIN
	Evidence value;
	if (!stagedEvidence(value) || value.originalMissing) return false;
	output = std::move(value);
	return true;
#else
	Q_UNUSED(output); return false;
#endif
}

bool ProjectSavePublication::stagedEvidence(Evidence& output) const
{
#ifdef Q_OS_WIN
	if (!m_state || !m_state->stageReady || m_state->moved || m_state->published
		|| !m_state->originalValid() || !m_state->valid(m_state->stage, m_state->stageName)) return false;
	output = {m_state->targetPath, m_state->tx, m_state->originalIdentity, m_state->originalSha256,
		m_state->stageIdentity, m_state->stageSha256, m_state->stageNativeFileId, m_state->originalMissing};
	return true;
#else
	Q_UNUSED(output); return false;
#endif
}

bool ProjectSavePublication::publishedEvidence(Evidence& output) const
{
#ifdef Q_OS_WIN
	if (!m_state || !m_state->published || !(m_state->terminalVerified || (m_state->nativeReady && nativeWitnessValid()))
		|| !m_state->valid(m_state->stage, m_state->targetName)
		|| (!m_state->originalMissing && !m_state->valid(m_state->original, m_state->backupName))) return false;
	output = {m_state->targetPath, m_state->tx, m_state->originalIdentity, m_state->originalSha256,
		m_state->stageIdentity, m_state->stageSha256, m_state->stageNativeFileId, m_state->originalMissing};
	return true;
#else
	Q_UNUSED(output); return false;
#endif
}
}
