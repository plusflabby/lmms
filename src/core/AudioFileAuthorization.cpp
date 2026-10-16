#include "AudioFileAuthorization.h"
#include <QCryptographicHash>
#include <QStringList>
#include <algorithm>
#include <array>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <winternl.h>
#endif

namespace lmms {
namespace {
QString canonicalInput(QString path)
{
	path.replace('\\', '/');
	if (path.size() < 3 || path.size() > 1024 || path[1] != ':' || path[2] != '/'
		|| !((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
		return {};
	path[0] = path[0].toUpper();
	const auto parts = path.mid(3).split('/');
	if (path.size() == 3) return path;
	for (const auto& part : parts)
	{
		if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')) return {};
		for (int i = 0; i < part.size(); ++i)
		{
			const auto c = part[i];
			if (c.unicode() < 32 || QStringLiteral(":*?\"<>|").contains(c)) return {};
			if (c.isHighSurrogate())
			{
				if (++i >= part.size() || !part[i].isLowSurrogate()) return {};
			}
			else if (c.isLowSurrogate()) return {};
		}
		const auto stem = part.section('.', 0, 0).toUpper();
		if (QStringList{"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}.contains(stem)
			|| (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT"))
				&& QStringLiteral("123456789¹²³").contains(stem[3]))) return {};
	}
	return path;
}

#ifdef Q_OS_WIN
struct Handles
{
	std::vector<HANDLE> values;
	~Handles() { for (auto i = values.rbegin(); i != values.rend(); ++i) CloseHandle(*i); }
};

QString finalName(HANDLE handle)
{
	std::array<wchar_t, 1025> buffer{};
	const auto count = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
	if (!count || count >= buffer.size()) return {};
	return QString::fromWCharArray(buffer.data(), int(count)).replace('\\', '/');
}

bool ordinary(HANDLE handle, bool directory, DWORD volume)
{
	BY_HANDLE_FILE_INFORMATION info{};
	return GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &info)
		&& info.dwVolumeSerialNumber == volume && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		&& bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
}

QByteArray fileIdentity(HANDLE handle)
{
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(handle, &info)) return {};
	const auto text = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
		+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
	return QCryptographicHash::hash(text, QCryptographicHash::Sha256);
}

// This device owns every non-reparse ancestor handle and the final source.
// No source pathname is reopened by the bounded decoder.
class AuthorizedDevice final : public QIODevice
{
public:
	explicit AuthorizedDevice(std::unique_ptr<Handles> handles) : m_handles(std::move(handles))
	{
		open(QIODevice::ReadOnly | QIODevice::Unbuffered);
	}
	qint64 size() const override
	{
		LARGE_INTEGER size{};
		return GetFileSizeEx(m_handles->values.back(), &size) ? size.QuadPart : -1;
	}
	bool seek(qint64 position) override
	{
		if (position < 0) return false;
		LARGE_INTEGER offset{}; offset.QuadPart = position;
		return SetFilePointerEx(m_handles->values.back(), offset, nullptr, FILE_BEGIN) && QIODevice::seek(position);
	}
protected:
	qint64 readData(char* data, qint64 length) override
	{
		if (length < 0) return -1;
		DWORD received = 0;
		return ReadFile(m_handles->values.back(), data, DWORD(std::min<quint64>(quint64(length), MAXDWORD)), &received, nullptr)
			? qint64(received) : -1;
	}
	qint64 writeData(const char*, qint64) override { return -1; }
private:
	std::unique_ptr<Handles> m_handles;
};
#endif
}

namespace {
using Result = AudioFileAuthorization::Result;
Result acquireImpl(const QString& approvedRoot, const QString& path, bool directoryTarget = false,
	const QByteArray& expectedRootIdentity = {})
{
	const auto denied = [] { return Result{QStringLiteral("import_path_denied"), {}, {}}; };
	if (approvedRoot.isEmpty()) return {QStringLiteral("import_root_not_configured"), {}, {}};
	const auto root = canonicalInput(approvedRoot);
	const auto candidate = canonicalInput(path);
	const auto prefix = root.endsWith('/') ? root : root + '/';
	if (root.isEmpty() || candidate.isEmpty() || !candidate.startsWith(prefix, Qt::CaseSensitive) || candidate == root) return denied();
#ifdef Q_OS_WIN
	const auto drive = root.left(3).replace('/', '\\');
	const auto driveType = GetDriveTypeW(reinterpret_cast<LPCWSTR>(drive.utf16()));
	if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE) return denied();
	auto handles = std::make_unique<Handles>();
	auto anchor = CreateFileW(reinterpret_cast<LPCWSTR>(drive.utf16()), FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
		FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (anchor == INVALID_HANDLE_VALUE) return denied();
	handles->values.push_back(anchor);
	BY_HANDLE_FILE_INFORMATION anchorInfo{};
	if (!GetFileInformationByHandle(anchor, &anchorInfo) || !ordinary(anchor, true, anchorInfo.dwVolumeSerialNumber)) return denied();
	const auto anchorName = finalName(anchor);
	// GUID volume names cannot name a UNC provider, device namespace, or SUBST alias.
	if (!anchorName.startsWith("//?/Volume{") || !anchorName.endsWith("}/")) return denied();
	const auto volumePath = anchorName;
	const auto volumeType = GetDriveTypeW(reinterpret_cast<LPCWSTR>(QString(volumePath).replace('/', '\\').utf16()));
	if (volumeType != DRIVE_FIXED && volumeType != DRIVE_REMOVABLE) return denied();
	const auto ntCreate = reinterpret_cast<decltype(&NtCreateFile)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
	if (!ntCreate) return denied();
	const auto parts = candidate.mid(3).split('/');
	QString expected = anchorName;
	for (int i = 0; i < parts.size(); ++i)
	{
		const bool directory = i + 1 < parts.size() || directoryTarget;
		const auto& part = parts[i];
		UNICODE_STRING name{};
		name.Buffer = const_cast<PWSTR>(reinterpret_cast<LPCWSTR>(part.utf16()));
		name.Length = USHORT(part.size() * sizeof(wchar_t));
		name.MaximumLength = name.Length;
		OBJECT_ATTRIBUTES attributes{};
		attributes.Length = sizeof(attributes);
		attributes.RootDirectory = handles->values.back();
		attributes.ObjectName = &name;
		IO_STATUS_BLOCK io{};
		HANDLE opened = nullptr;
		// One component relative to a pinned directory; never follow a reparse point.
		const auto status = ntCreate(&opened, (directory ? FILE_READ_ATTRIBUTES | FILE_TRAVERSE : GENERIC_READ) | SYNCHRONIZE,
			&attributes, &io, nullptr, 0, FILE_SHARE_READ, FILE_OPEN,
			FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT | (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE), nullptr, 0);
		if (status < 0)
		{
			// Only a missing final entry is a missing source; other OS diagnostics stay private.
			if (!directory && quint32(status) == 0xc0000034u) return {QStringLiteral("audio_source_missing"), {}, {}};
			return denied();
		}
		handles->values.push_back(opened);
		if (!ordinary(opened, directory, anchorInfo.dwVolumeSerialNumber)) return denied();
		if (!expectedRootIdentity.isEmpty() && i + 1 == root.mid(3).split('/').size()
			&& fileIdentity(opened) != expectedRootIdentity) return denied();
		expected += part;
		if (finalName(opened) != expected || expected.size() > 1024) return denied();
		if (directory) expected += '/';
	}
	const auto identity = fileIdentity(handles->values.back());
	if (identity.isEmpty()) return denied();
	return {QStringLiteral("ok"), identity, std::make_unique<AuthorizedDevice>(std::move(handles))};
#else
	return denied();
#endif
}
}

AudioFileAuthorization::Result AudioFileAuthorization::acquire(const QString& approvedRoot, const QString& path)
{
	return acquireImpl(approvedRoot, path);
}

AudioFileAuthorization::Approval AudioFileAuthorization::approveRoot(const QString& approvedRoot)
{
	if (approvedRoot.isEmpty()) return {QStringLiteral("import_root_not_configured"), {}};
	const auto path = canonicalInput(approvedRoot);
	// An approval names a scoped directory, never an entire drive.
	if (path.size() <= 3) return {QStringLiteral("import_path_denied"), {}};
	const auto parent = path.lastIndexOf('/') == 2 ? path.left(3) : path.left(path.lastIndexOf('/'));
	auto pinned = acquireImpl(parent, path, true);
	if (!pinned.source) return {pinned.code, {}};
	auto root = std::make_unique<Root>();
	root->m_path = path;
	root->m_identity = pinned.identity;
	root->m_pin = std::move(pinned.source);
	return {QStringLiteral("ok"), std::move(root)};
}

AudioFileAuthorization::Result AudioFileAuthorization::acquire(const Root& root, const QString& path)
{
	if (root.m_revoked || !root.m_pin) return {QStringLiteral("import_path_denied"), {}, {}};
	auto result = acquireImpl(root.m_path, path, false, root.m_identity);
	if (root.m_revoked) return {QStringLiteral("import_path_denied"), {}, {}};
	return result;
}
}
