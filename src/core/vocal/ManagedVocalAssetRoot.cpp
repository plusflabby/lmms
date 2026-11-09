#include "ManagedVocalAssetRoot.h"

#include "ManagedVocalAssetFileOps.h"
#include "ConfigManager.h"
#include "RecordingLibrary.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QDir>
#include <QScopeGuard>
#include <QStringList>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <functional>
#include <mutex>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <winternl.h>
#endif

namespace lmms {
struct ManagedVocalAssetRootState
{
	std::atomic_bool revoked{false};
#ifdef Q_OS_WIN
	HANDLE directory = INVALID_HANDLE_VALUE;
	HANDLE writerLock = INVALID_HANDLE_VALUE;
	std::shared_ptr<void> borrowedLock;
	std::function<bool()> parentValid;
	DWORD volume = 0;
	QString finalPath;
	QString selectedPath;
	QString identity;
	~ManagedVocalAssetRootState()
	{
		if (writerLock != INVALID_HANDLE_VALUE && !borrowedLock) CloseHandle(writerLock);
		if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
	}
#endif
};

namespace {
QString canonicalRoot(QString path)
{
	path.replace('\\', '/');
	if (path.size() <= 3 || path.size() > 1024 || path[1] != ':' || path[2] != '/'
		|| !((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
	{
		return {};
	}
	path[0] = path[0].toUpper();
	const auto components = path.mid(3).split('/');
	for (const auto& part : components)
	{
		if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")
			|| part.endsWith('.') || part.endsWith(' '))
		{
			return {};
		}
		for (int i = 0; i < part.size(); ++i)
		{
			const auto ch = part[i];
			if (ch.unicode() < 32 || QStringLiteral(":*?\"<>|").contains(ch))
			{
				return {};
			}
			if (ch.isHighSurrogate())
			{
				if (++i >= part.size() || !part[i].isLowSurrogate()) return {};
			}
			else if (ch.isLowSurrogate()) return {};
		}
		const auto stem = part.section('.', 0, 0).toUpper();
		if (QStringList{QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
				QStringLiteral("NUL"), QStringLiteral("CONIN$"), QStringLiteral("CONOUT$")}.contains(stem)
			|| (stem.size() == 4 && (stem.startsWith(QStringLiteral("COM")) || stem.startsWith(QStringLiteral("LPT")))
				&& QStringLiteral("123456789¹²³").contains(stem[3])))
		{
			return {};
		}
	}
	return path;
}

#ifdef Q_OS_WIN
QString finalName(HANDLE handle)
{
	std::array<wchar_t, 1025> buffer{};
	const auto count = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()),
		FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
	return count && count < buffer.size()
		? QString::fromWCharArray(buffer.data(), int(count)).replace('\\', '/') : QString{};
}

bool ordinary(HANDLE handle, bool directory, DWORD volume)
{
	BY_HANDLE_FILE_INFORMATION info{};
	return GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &info)
		&& info.dwVolumeSerialNumber == volume && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		&& bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory
		&& (directory || info.nNumberOfLinks == 1);
}

HANDLE relative(HANDLE parent, const QString& component, ACCESS_MASK access, ULONG disposition,
	bool directory, NTSTATUS& status, ULONG share)
{
	const auto create = reinterpret_cast<decltype(&NtCreateFile)>(
		GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
	if (!create)
	{
		status = NTSTATUS(0xc0000002u);
		return INVALID_HANDLE_VALUE;
	}
	UNICODE_STRING name{};
	name.Buffer = const_cast<PWSTR>(reinterpret_cast<LPCWSTR>(component.utf16()));
	name.Length = USHORT(component.size() * sizeof(wchar_t));
	name.MaximumLength = name.Length;
	OBJECT_ATTRIBUTES attributes{};
	attributes.Length = sizeof(attributes);
	attributes.RootDirectory = parent;
	attributes.ObjectName = &name;
	attributes.Attributes = OBJ_DONT_REPARSE;
	IO_STATUS_BLOCK io{};
	HANDLE handle = nullptr;
	status = create(&handle, access | SYNCHRONIZE, &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
		share, disposition, FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT
			| (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE), nullptr, 0);
	return status >= 0 ? handle : INVALID_HANDLE_VALUE;
}

bool rootAuthorized(const std::shared_ptr<ManagedVocalAssetRootState>& root)
{
	if (!root || root->revoked.load(std::memory_order_acquire) || (root->parentValid && !root->parentValid())
		|| root->directory == INVALID_HANDLE_VALUE || root->writerLock == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	LARGE_INTEGER size{};
	return ordinary(root->directory, true, root->volume) && finalName(root->directory) == root->finalPath
		&& ordinary(root->writerLock, false, root->volume)
		&& finalName(root->writerLock) == root->finalPath + QStringLiteral("/recording-library.lock")
		&& GetFileSizeEx(root->writerLock, &size) && size.QuadPart == 0;
}

QString openRoot(const QString& selected, std::shared_ptr<ManagedVocalAssetRootState>& result,
	std::shared_ptr<void> borrowedLock = {}, std::function<bool()> parentValid = {})
{
	const auto path = canonicalRoot(selected);
	if (path.isEmpty())
	{
		return QStringLiteral("asset_root_denied");
	}
	const auto drive = path.left(3).replace('/', '\\');
	const auto type = GetDriveTypeW(reinterpret_cast<LPCWSTR>(drive.utf16()));
	if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
	{
		return QStringLiteral("asset_root_denied");
	}
	auto state = std::make_shared<ManagedVocalAssetRootState>();
	state->selectedPath = path;
	auto anchor = CreateFileW(reinterpret_cast<LPCWSTR>(drive.utf16()),
		FILE_READ_ATTRIBUTES | FILE_TRAVERSE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (anchor == INVALID_HANDLE_VALUE)
	{
		return QStringLiteral("asset_root_denied");
	}
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(anchor, &info) || !ordinary(anchor, true, info.dwVolumeSerialNumber))
	{
		CloseHandle(anchor);
		return QStringLiteral("asset_root_denied");
	}
	const auto driveName = finalName(anchor);
	if (!driveName.startsWith(QStringLiteral("//?/Volume{")) || !driveName.endsWith(QStringLiteral("}/")))
	{
		CloseHandle(anchor);
		return QStringLiteral("asset_root_denied");
	}
	state->volume = info.dwVolumeSerialNumber;
	QString expected = driveName;
	const auto components = path.mid(3).split('/');
	HANDLE current = anchor;
	std::vector<HANDLE> opened;
	for (int index = 0; index < components.size(); ++index)
	{
		const auto& component = components[index];
		NTSTATUS status = 0;
		ACCESS_MASK directoryAccess = FILE_READ_ATTRIBUTES | FILE_TRAVERSE;
		if (index + 1 == components.size())
		{
			directoryAccess |= FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_DELETE_CHILD;
		}
		const auto next = relative(current, component, directoryAccess,
			FILE_OPEN, true, status, FILE_SHARE_READ | FILE_SHARE_WRITE);
		if (next == INVALID_HANDLE_VALUE)
		{
			for (auto handle = opened.rbegin(); handle != opened.rend(); ++handle) CloseHandle(*handle);
			CloseHandle(anchor);
			return QStringLiteral("asset_root_denied");
		}
		opened.push_back(next);
		current = next;
		expected += component;
		if (!ordinary(next, true, state->volume) || finalName(next) != expected)
		{
			for (auto handle = opened.rbegin(); handle != opened.rend(); ++handle) CloseHandle(*handle);
			CloseHandle(anchor);
			return QStringLiteral("asset_root_denied");
		}
		expected += '/';
	}
	state->directory = opened.back();
	opened.pop_back();
	for (auto handle = opened.rbegin(); handle != opened.rend(); ++handle) CloseHandle(*handle);
	CloseHandle(anchor);
	state->finalPath = finalName(state->directory);
	BY_HANDLE_FILE_INFORMATION rootInfo{};
	if (!GetFileInformationByHandle(state->directory, &rootInfo)
		|| rootInfo.dwVolumeSerialNumber != state->volume)
	{
		return QStringLiteral("asset_root_denied");
	}
	const auto identityMaterial = QByteArray::number(qulonglong(rootInfo.dwVolumeSerialNumber)) + ':'
		+ QByteArray::number(qulonglong(rootInfo.nFileIndexHigh)) + ':'
		+ QByteArray::number(qulonglong(rootInfo.nFileIndexLow));
	state->identity = QString::fromLatin1(QCryptographicHash::hash(identityMaterial,
		QCryptographicHash::Sha256).toHex());
	if (state->finalPath.isEmpty())
	{
		return QStringLiteral("asset_root_denied");
	}

	NTSTATUS lockStatus = 0;
	if (borrowedLock)
	{
		state->borrowedLock = std::move(borrowedLock);
		state->parentValid = std::move(parentValid);
		state->writerLock = static_cast<HANDLE>(state->borrowedLock.get());
	}
	else state->writerLock = relative(state->directory, QStringLiteral("recording-library.lock"),
		GENERIC_READ | GENERIC_WRITE, FILE_OPEN_IF, false, lockStatus, 0);
	LARGE_INTEGER lockSize{};
	if (state->writerLock == INVALID_HANDLE_VALUE || !ordinary(state->writerLock, false, state->volume)
		|| finalName(state->writerLock) != state->finalPath + QStringLiteral("/recording-library.lock")
		|| !GetFileSizeEx(state->writerLock, &lockSize) || lockSize.QuadPart != 0)
	{
		return QStringLiteral("asset_root_busy");
	}
	if (!rootAuthorized(state))
	{
		return QStringLiteral("asset_root_denied");
	}
	result = std::move(state);
	return QStringLiteral("ok");
}
#endif
} // namespace

ManagedVocalAssetRoot::ManagedVocalAssetRoot(std::shared_ptr<ManagedVocalAssetRootState> state)
	: m_state(std::move(state))
{
}

ManagedVocalAssetRoot::~ManagedVocalAssetRoot() = default;

ManagedVocalAssetRoot::Approval ManagedVocalAssetRoot::authorizeRecordingRoot(recording::RecordingLibrary& library)
{
#ifdef Q_OS_WIN
	auto grant = library.managedRootGrant();
	if (grant.code != QStringLiteral("ok") || !grant.lock || !grant.valid || !grant.valid())
		return {QStringLiteral("asset_root_denied"), {}};
	std::shared_ptr<ManagedVocalAssetRootState> state;
	const auto code = openRoot(grant.selectedPath, state, std::move(grant.lock), std::move(grant.valid));
	if (code != QStringLiteral("ok")) return {code, {}};
	return {code, std::shared_ptr<ManagedVocalAssetRoot>(new ManagedVocalAssetRoot(std::move(state)))};
#else
	Q_UNUSED(library); return {QStringLiteral("unsupported_backend"), {}};
#endif
}

ManagedVocalAssetRoot::Approval ManagedVocalAssetRoot::authorizeRecordingRoot(
	recording::RecordingLibrary& library, const QString& expectedRoot)
{
#ifdef Q_OS_WIN
	auto grant = library.managedRootGrant();
	const auto expected = canonicalRoot(expectedRoot);
	const auto selected = canonicalRoot(grant.selectedPath);
	if (grant.code != QStringLiteral("ok") || !grant.lock || !grant.valid || !grant.valid()
		|| expected.isEmpty() || expected != selected) return {QStringLiteral("asset_root_denied"), {}};
	std::shared_ptr<ManagedVocalAssetRootState> state;
	const auto code = openRoot(selected, state, std::move(grant.lock), std::move(grant.valid));
	if (code != QStringLiteral("ok")) return {code, {}};
	return {code, std::shared_ptr<ManagedVocalAssetRoot>(new ManagedVocalAssetRoot(std::move(state)))};
#else
	Q_UNUSED(library); Q_UNUSED(expectedRoot); return {QStringLiteral("unsupported_backend"), {}};
#endif
}

ManagedVocalAssetRoot::Approval ManagedVocalAssetRoot::authorizeLocalRoot(const QString& selectedRoot)
{
#ifdef Q_OS_WIN
	std::shared_ptr<ManagedVocalAssetRootState> state;
	const auto code = openRoot(selectedRoot, state);
	if (code != QStringLiteral("ok"))
	{
		return {code, {}};
	}
	return {code, std::shared_ptr<ManagedVocalAssetRoot>(new ManagedVocalAssetRoot(std::move(state)))};
#else
	Q_UNUSED(selectedRoot);
	return {QStringLiteral("unsupported_backend"), {}};
#endif
}

ManagedVocalAssetRoot::Approval ManagedVocalAssetRoot::authorizeConfiguredRoot()
{
	const auto selected = ConfigManager::inst()->value(QStringLiteral("vocal_assets"),
		QStringLiteral("managed_root"));
	if (selected.isEmpty())
	{
		return {QStringLiteral("asset_root_not_configured"), {}};
	}
#ifdef Q_OS_WIN
	const auto path = canonicalRoot(selected);
	if (path.isEmpty())
	{
		return {QStringLiteral("asset_root_denied"), {}};
	}
	static std::mutex mutex;
	static std::map<QString, std::weak_ptr<ManagedVocalAssetRoot>> roots;
	std::lock_guard lock(mutex);
	for (auto entry = roots.begin(); entry != roots.end();)
	{
		if (entry->second.expired()) entry = roots.erase(entry);
		else ++entry;
	}
	if (const auto found = roots.find(path); found != roots.end())
	{
		if (auto root = found->second.lock(); root && root->isAuthorized())
		{
			return {QStringLiteral("ok"), std::move(root)};
		}
		roots.erase(found);
	}
	std::shared_ptr<ManagedVocalAssetRootState> state;
	const auto code = openRoot(path, state);
	if (code != QStringLiteral("ok"))
	{
		return {code, {}};
	}
	auto root = std::shared_ptr<ManagedVocalAssetRoot>(new ManagedVocalAssetRoot(std::move(state)));
	roots.emplace(path, root);
	return {QStringLiteral("ok"), std::move(root)};
#else
	return authorizeLocalRoot(selected);
#endif
}

ManagedVocalAssetRoot::Approval ManagedVocalAssetRoot::authorizeConfiguredRoot(recording::RecordingLibrary* sharedRecordingLibrary)
{
	auto approval = authorizeConfiguredRoot();
	if (approval.root || approval.code != QStringLiteral("asset_root_busy") || !sharedRecordingLibrary) return approval;
	const auto configured = ConfigManager::inst()->value(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"));
	return authorizeRecordingRoot(*sharedRecordingLibrary, configured);
}

bool ManagedVocalAssetRoot::isAuthorized() const
{
#ifdef Q_OS_WIN
	return rootAuthorized(m_state);
#else
	return false;
#endif
}

QString ManagedVocalAssetRoot::rootId() const
{
#ifdef Q_OS_WIN
	return rootAuthorized(m_state) ? m_state->identity : QString{};
#else
	return {};
#endif
}

void ManagedVocalAssetRoot::revoke()
{
	if (m_state)
	{
		m_state->revoked.store(true, std::memory_order_release);
	}
}

namespace managed_vocal_detail {
struct FileState
{
	std::shared_ptr<ManagedVocalAssetRootState> root;
#ifdef Q_OS_WIN
	HANDLE handle = INVALID_HANDLE_VALUE;
	std::vector<HANDLE> ancestors;
	DWORD volume = 0;
#endif
	QString name;
	QString finalPath;
	quint64 maximumBytes = 0;
	quint64 writtenBytes = 0;
	bool writable = false;
	bool deleted = false;
	~FileState()
	{
#ifdef Q_OS_WIN
		if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
		for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) CloseHandle(*it);
#endif
	}
};

#ifdef Q_OS_WIN
namespace {
bool safeName(const QString& name)
{
	if (name.isEmpty() || name.size() > 128 || name == QStringLiteral(".") || name == QStringLiteral(".."))
	{
		return false;
	}
	for (const auto ch : name)
	{
		const bool alphaNumeric = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
			|| (ch >= '0' && ch <= '9');
		if (!alphaNumeric && ch != '-' && ch != '_' && ch != '.')
		{
			return false;
		}
	}
	return true;
}

bool validFile(const std::shared_ptr<FileState>& file)
{
	return file && !file->deleted && rootAuthorized(file->root)
		&& file->handle != INVALID_HANDLE_VALUE
		&& ordinary(file->handle, false, file->root->volume)
		&& file->root->volume == file->volume
		&& finalName(file->handle) == file->finalPath
		&& file->finalPath == file->root->finalPath + '/' + file->name;
}
}
#endif

bool authorized(const std::shared_ptr<ManagedVocalAssetRootState>& root)
{
#ifdef Q_OS_WIN
	return rootAuthorized(root);
#else
	Q_UNUSED(root);
	return false;
#endif
}

QString createNewFile(const std::shared_ptr<ManagedVocalAssetRootState>& root, const QString& name,
	quint64 maximumBytes, std::shared_ptr<FileState>& output)
{
#ifdef Q_OS_WIN
	if (!rootAuthorized(root) || !safeName(name) || !maximumBytes)
	{
		return QStringLiteral("asset_root_denied");
	}
	NTSTATUS status = 0;
	const auto handle = relative(root->directory, name, GENERIC_READ | GENERIC_WRITE | DELETE,
		FILE_CREATE, false, status, 0);
	if (handle == INVALID_HANDLE_VALUE)
	{
		return quint32(status) == 0xc0000035u ? QStringLiteral("asset_exists")
			: QStringLiteral("asset_write_failed");
	}
	if (!ordinary(handle, false, root->volume)
		|| finalName(handle) != root->finalPath + '/' + name)
	{
		CloseHandle(handle);
		return QStringLiteral("asset_integrity_failed");
	}
	auto file = std::make_shared<FileState>();
	file->root = root;
	file->handle = handle;
	file->volume = root->volume;
	file->name = name;
	file->finalPath = root->finalPath + '/' + name;
	file->maximumBytes = maximumBytes;
	file->writable = true;
	output = std::move(file);
	return QStringLiteral("ok");
#else
	Q_UNUSED(root); Q_UNUSED(name); Q_UNUSED(maximumBytes); Q_UNUSED(output);
	return QStringLiteral("unsupported_backend");
#endif
}

QString openReadFile(const std::shared_ptr<ManagedVocalAssetRootState>& root, const QString& name,
	std::shared_ptr<FileState>& output)
{
#ifdef Q_OS_WIN
	if (!rootAuthorized(root) || !safeName(name))
	{
		return QStringLiteral("asset_root_denied");
	}
	NTSTATUS status = 0;
	const auto handle = relative(root->directory, name, GENERIC_READ, FILE_OPEN, false, status, FILE_SHARE_READ);
	if (handle == INVALID_HANDLE_VALUE)
	{
		return quint32(status) == 0xc0000034u ? QStringLiteral("asset_missing")
			: QStringLiteral("asset_integrity_failed");
	}
	if (!ordinary(handle, false, root->volume)
		|| finalName(handle) != root->finalPath + '/' + name)
	{
		CloseHandle(handle);
		return QStringLiteral("asset_integrity_failed");
	}
	auto file = std::make_shared<FileState>();
	file->root = root;
	file->handle = handle;
	file->volume = root->volume;
	file->name = name;
	file->finalPath = root->finalPath + '/' + name;
	output = std::move(file);
	return QStringLiteral("ok");
#else
	Q_UNUSED(root); Q_UNUSED(name); Q_UNUSED(output);
	return QStringLiteral("unsupported_backend");
#endif
}

QString removeMatchingFile(const std::shared_ptr<ManagedVocalAssetRootState>& root,
	const QString& name, const QByteArray& expected)
{
#ifdef Q_OS_WIN
	if (!rootAuthorized(root) || !safeName(name) || expected.isEmpty() || expected.size() > 2048)
		return QStringLiteral("asset_integrity_failed");
	NTSTATUS status = 0;
	const auto handle = relative(root->directory, name, GENERIC_READ | DELETE, FILE_OPEN, false, status, FILE_SHARE_READ);
	if (handle == INVALID_HANDLE_VALUE)
		return quint32(status) == 0xc0000034u ? QStringLiteral("asset_missing") : QStringLiteral("asset_integrity_failed");
	const auto release = qScopeGuard([handle] { CloseHandle(handle); });
	LARGE_INTEGER length{};
	if (!ordinary(handle, false, root->volume) || finalName(handle) != root->finalPath + '/' + name
		|| !GetFileSizeEx(handle, &length) || length.QuadPart != expected.size()) return QStringLiteral("asset_integrity_failed");
	QByteArray actual(expected.size(), Qt::Uninitialized); DWORD received = 0;
	if (!ReadFile(handle, actual.data(), DWORD(actual.size()), &received, nullptr)
		|| received != DWORD(actual.size()) || actual != expected || !rootAuthorized(root)) return QStringLiteral("asset_integrity_failed");
	FILE_DISPOSITION_INFO disposition{TRUE};
	return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition))
		? QStringLiteral("ok") : QStringLiteral("asset_write_failed");
#else
	Q_UNUSED(root); Q_UNUSED(name); Q_UNUSED(expected); return QStringLiteral("unsupported_backend");
#endif
}

QString openReadProjectInRoot(const std::shared_ptr<ManagedVocalAssetRootState>& root,
	const QString& absolutePath, std::shared_ptr<FileState>& output, QByteArray& identity)
{
#ifdef Q_OS_WIN
	if (!rootAuthorized(root)) return QStringLiteral("asset_root_revoked");
	if (!QDir::isAbsolutePath(absolutePath)) return QStringLiteral("project_identity_unverified");
	const QFileInfo candidate(absolutePath);
	if (canonicalRoot(QDir::fromNativeSeparators(candidate.absoluteFilePath())).isEmpty())
		return QStringLiteral("project_identity_unverified");
	const auto parent = canonicalRoot(QDir::fromNativeSeparators(candidate.absolutePath()));
	const auto prefix = root->selectedPath + QLatin1Char('/');
	if (parent.isEmpty() || (parent.compare(root->selectedPath, Qt::CaseInsensitive) != 0
		&& !parent.startsWith(prefix, Qt::CaseInsensitive)))
		return QStringLiteral("project_not_in_root");
	const auto name = candidate.fileName();
	if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
		|| name.size() > 255 || name.endsWith('.') || name.endsWith(' '))
		return QStringLiteral("project_identity_unverified");
	for (const auto ch : name)
		if (ch.unicode() < 32 || QStringLiteral("\\/:*?\"<>|").contains(ch))
			return QStringLiteral("project_identity_unverified");
	std::vector<HANDLE> ancestors;
	const auto closeAncestors = [&] { for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) CloseHandle(*it); };
	auto ancestorGuard = qScopeGuard(closeAncestors);
	HANDLE parentHandle = root->directory;
	QString relativeName;
	if (parent.size() > root->selectedPath.size())
	{
		const auto components = parent.mid(prefix.size()).split('/');
		ancestors.reserve(size_t(components.size()));
		for (const auto& component : components)
		{
			NTSTATUS directoryStatus = 0;
			const auto directory = relative(parentHandle, component, FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
				FILE_OPEN, true, directoryStatus, FILE_SHARE_READ | FILE_SHARE_WRITE);
			if (directory == INVALID_HANDLE_VALUE) return QStringLiteral("project_identity_unverified");
			ancestors.push_back(directory);
			relativeName += component + QLatin1Char('/');
			if (!ordinary(directory, true, root->volume) || finalName(directory) != root->finalPath + '/' + relativeName.chopped(1))
				return QStringLiteral("project_identity_unverified");
			parentHandle = directory;
		}
	}
	relativeName += name;
	NTSTATUS status = 0;
	const auto handle = relative(parentHandle, name, GENERIC_READ, FILE_OPEN, false, status, FILE_SHARE_READ);
	if (handle == INVALID_HANDLE_VALUE)
		return quint32(status) == 0xc0000034u ? QStringLiteral("project_missing")
			: QStringLiteral("project_identity_unverified");
	auto fileGuard = qScopeGuard([handle] { CloseHandle(handle); });
	BY_HANDLE_FILE_INFORMATION info{};
	if (!ordinary(handle, false, root->volume) || finalName(handle) != root->finalPath + '/' + relativeName
		|| !GetFileInformationByHandle(handle, &info))
	{
		return QStringLiteral("project_identity_unverified");
	}
	const auto material = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
		+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
	identity = QCryptographicHash::hash(material, QCryptographicHash::Sha256);
	auto file = std::make_shared<FileState>();
	file->root = root;
	file->handle = handle;
	fileGuard.dismiss();
	file->ancestors = std::move(ancestors);
	ancestorGuard.dismiss();
	file->volume = root->volume;
	file->name = relativeName;
	file->finalPath = root->finalPath + '/' + relativeName;
	output = std::move(file);
	return QStringLiteral("ok");
#else
	Q_UNUSED(root); Q_UNUSED(absolutePath); Q_UNUSED(output); Q_UNUSED(identity);
	return QStringLiteral("unsupported_backend");
#endif
}

bool write(const std::shared_ptr<FileState>& file, const char* data, qint64 length)
{
#ifdef Q_OS_WIN
	if (!validFile(file) || !file->writable || length < 0
		|| quint64(length) > file->maximumBytes - std::min(file->maximumBytes, file->writtenBytes))
	{
		return false;
	}
	qint64 remaining = length;
	while (remaining > 0)
	{
		DWORD written = 0;
		const auto request = DWORD(std::min<qint64>(remaining, MAXDWORD));
		if (!WriteFile(file->handle, data + (length - remaining), request, &written, nullptr) || written == 0)
		{
			return false;
		}
		remaining -= qint64(written);
		file->writtenBytes += written;
	}
	return true;
#else
	Q_UNUSED(file); Q_UNUSED(data); Q_UNUSED(length);
	return false;
#endif
}

bool flush(const std::shared_ptr<FileState>& file)
{
#ifdef Q_OS_WIN
	return validFile(file) && FlushFileBuffers(file->handle);
#else
	Q_UNUSED(file);
	return false;
#endif
}

bool seek(const std::shared_ptr<FileState>& file, qint64 position)
{
#ifdef Q_OS_WIN
	if (!validFile(file) || position < 0) return false;
	LARGE_INTEGER offset{}; offset.QuadPart = position;
	return SetFilePointerEx(file->handle, offset, nullptr, FILE_BEGIN) != FALSE;
#else
	Q_UNUSED(file); Q_UNUSED(position);
	return false;
#endif
}

qint64 read(const std::shared_ptr<FileState>& file, char* destination, qint64 length)
{
#ifdef Q_OS_WIN
	if (!validFile(file) || length < 0) return -1;
	DWORD received = 0;
	const auto requested = DWORD(std::min<qint64>(length, 65536));
	return ReadFile(file->handle, destination, requested, &received, nullptr)
		? qint64(received) : -1;
#else
	Q_UNUSED(file); Q_UNUSED(destination); Q_UNUSED(length);
	return -1;
#endif
}

qint64 size(const std::shared_ptr<FileState>& file)
{
#ifdef Q_OS_WIN
	LARGE_INTEGER value{};
	return validFile(file) && GetFileSizeEx(file->handle, &value) ? value.QuadPart : -1;
#else
	Q_UNUSED(file);
	return -1;
#endif
}

QString renameNoReplace(const std::shared_ptr<FileState>& file, const QString& newName)
{
#ifdef Q_OS_WIN
	if (!validFile(file) || !file->writable || !safeName(newName))
	{
		return QStringLiteral("asset_integrity_failed");
	}
	const auto nameBytes = std::size_t(newName.size()) * sizeof(wchar_t);
	const auto offset = offsetof(FILE_RENAME_INFORMATION, FileName);
	std::vector<char> storage(offset + nameBytes);
	auto* rename = reinterpret_cast<FILE_RENAME_INFORMATION*>(storage.data());
	rename->ReplaceIfExists = FALSE;
	rename->RootDirectory = file->root->directory;
	rename->FileNameLength = ULONG(nameBytes);
	std::memcpy(rename->FileName, newName.utf16(), nameBytes);
	const auto ntSetInformation = reinterpret_cast<decltype(&NtSetInformationFile)>(
		GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
	IO_STATUS_BLOCK io{};
	if (!ntSetInformation)
	{
		return QStringLiteral("asset_publish_failed");
	}
	const auto status = ntSetInformation(file->handle, &io, rename, ULONG(storage.size()),
		FileRenameInformation);
	if (status < 0)
	{
		return quint32(status) == 0xc0000035u ? QStringLiteral("asset_exists")
			: QStringLiteral("asset_publish_failed");
	}
	file->name = newName;
	file->finalPath = file->root->finalPath + '/' + newName;
	return validFile(file) ? QStringLiteral("ok") : QStringLiteral("asset_integrity_failed");
#else
	Q_UNUSED(file); Q_UNUSED(newName);
	return QStringLiteral("unsupported_backend");
#endif
}

bool removeOwnedFile(const std::shared_ptr<FileState>& file)
{
#ifdef Q_OS_WIN
	if (!validFile(file) || !file->writable) return false;
	FILE_DISPOSITION_INFO disposition{TRUE};
	const bool removed = SetFileInformationByHandle(file->handle, FileDispositionInfo,
		&disposition, sizeof(disposition)) != FALSE;
	file->deleted = removed;
	return removed;
#else
	Q_UNUSED(file);
	return false;
#endif
}

QString fileName(const std::shared_ptr<FileState>& file)
{
	return file ? file->name : QString{};
}

QString listRootEntries(
	const std::shared_ptr<ManagedVocalAssetRootState>& root,
	std::vector<RootEntry>& entries,
	size_t maximumEntries)
{
#ifdef Q_OS_WIN
	if (!rootAuthorized(root) || !maximumEntries)
	{
		return QStringLiteral("asset_root_revoked");
	}
	alignas(FILE_ID_BOTH_DIR_INFO) std::array<char, 65536> buffer{};
	auto informationClass = FileIdBothDirectoryRestartInfo;
	for (;;)
	{
		if (!GetFileInformationByHandleEx(root->directory, informationClass, buffer.data(), DWORD(buffer.size())))
		{
			if (GetLastError() == ERROR_NO_MORE_FILES)
			{
				break;
			}
			return QStringLiteral("asset_recovery_scan_failed");
		}
		auto* entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buffer.data());
		for (;;)
		{
			if ((entry->FileNameLength % sizeof(wchar_t)) != 0
				|| entry->FileNameLength > buffer.size() - offsetof(FILE_ID_BOTH_DIR_INFO, FileName)
				|| entry->EndOfFile.QuadPart < 0)
			{
				return QStringLiteral("asset_recovery_scan_failed");
			}
			const auto name = QString::fromWCharArray(entry->FileName,
				int(entry->FileNameLength / sizeof(wchar_t)));
			if (name != QStringLiteral(".") && name != QStringLiteral(".."))
			{
				if (entries.size() >= maximumEntries)
				{
					return QStringLiteral("resource_limit");
				}
				entries.push_back({name, quint64(entry->EndOfFile.QuadPart),
					bool(entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY),
					bool(entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)});
			}
			if (!entry->NextEntryOffset)
			{
				break;
			}
			if (entry->NextEntryOffset > buffer.size()
				|| entry->NextEntryOffset < offsetof(FILE_ID_BOTH_DIR_INFO, FileName))
			{
				return QStringLiteral("asset_recovery_scan_failed");
			}
			entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(
				reinterpret_cast<char*>(entry) + entry->NextEntryOffset);
		}
		informationClass = FileIdBothDirectoryInfo;
	}
	return rootAuthorized(root) ? QStringLiteral("ok") : QStringLiteral("asset_root_revoked");
#else
	Q_UNUSED(root); Q_UNUSED(entries); Q_UNUSED(maximumEntries);
	return QStringLiteral("unsupported_backend");
#endif
}

} // namespace lmms::managed_vocal_detail
} // namespace lmms
