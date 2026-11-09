#ifndef LMMS_MANAGED_VOCAL_ASSET_FILE_OPS_H
#define LMMS_MANAGED_VOCAL_ASSET_FILE_OPS_H

#include "ManagedVocalAssetRoot.h"
#include <QByteArray>
#include <QStringList>
#include <memory>
#include <vector>

namespace lmms::managed_vocal_detail {

struct FileState;
struct RootEntry
{
	QString name;
	quint64 bytes = 0;
	bool directory = false;
	bool reparse = false;
};

QString createNewFile(
	const std::shared_ptr<ManagedVocalAssetRootState>& root,
	const QString& name,
	quint64 maximumBytes,
	std::shared_ptr<FileState>& file);
QString openReadFile(
	const std::shared_ptr<ManagedVocalAssetRootState>& root,
	const QString& name,
	std::shared_ptr<FileState>& file);
// Removes only a handle-pinned root entry whose complete bounded bytes match.
QString removeMatchingFile(const std::shared_ptr<ManagedVocalAssetRootState>& root,
	const QString& name, const QByteArray& expected);
// Opens only a descendant of the pinned root; other parents return
// project_not_in_root so callers can use their separately authorized path.
QString openReadProjectInRoot(
	const std::shared_ptr<ManagedVocalAssetRootState>& root,
	const QString& absolutePath,
	std::shared_ptr<FileState>& file,
	QByteArray& identity);
bool authorized(const std::shared_ptr<ManagedVocalAssetRootState>& root);
bool write(const std::shared_ptr<FileState>& file, const char* data, qint64 length);
bool flush(const std::shared_ptr<FileState>& file);
bool seek(const std::shared_ptr<FileState>& file, qint64 position);
qint64 read(const std::shared_ptr<FileState>& file, char* destination, qint64 length);
qint64 size(const std::shared_ptr<FileState>& file);
QString renameNoReplace(const std::shared_ptr<FileState>& file, const QString& newName);
bool removeOwnedFile(const std::shared_ptr<FileState>& file);
QString fileName(const std::shared_ptr<FileState>& file);
QString listRootEntries(
	const std::shared_ptr<ManagedVocalAssetRootState>& root,
	std::vector<RootEntry>& entries,
	size_t maximumEntries);

} // namespace lmms::managed_vocal_detail

#endif
