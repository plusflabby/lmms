#include "AiBridgeAssetCatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <algorithm>

namespace
{

QString normalizedRelative(const QString &value)
{
	const QString normalized = QDir::fromNativeSeparators(value);
	if (normalized.isEmpty() || normalized.startsWith('/') || normalized.contains(':')
		|| normalized.contains("//"))
		return {};
	const QStringList components = normalized.split('/', Qt::KeepEmptyParts);
	for (const auto &component : components)
		if (component.isEmpty() || component == QStringLiteral(".")
			|| component == QStringLiteral(".."))
			return {};
	return components.join('/');
}

QString kindName(lmms::AiBridgeAssetKind kind)
{
	return kind == lmms::AiBridgeAssetKind::Sample ? QStringLiteral("sample")
		: QStringLiteral("preset");
}

bool componentContained(const QString &root, const QString &target)
{
	const QString rootPath = QDir::fromNativeSeparators(root).toCaseFolded();
	const QString targetPath = QDir::fromNativeSeparators(target).toCaseFolded();
	if (targetPath == rootPath)
		return true;
	return targetPath.startsWith(rootPath.endsWith('/') ? rootPath : rootPath + '/');
}

bool boundedFilter(const QString &value)
{
	return value.toUtf8().size() <= lmms::AiBridgeAssetTypes::MaxFilterBytes;
}

int boundedLimit(int requested)
{
	if (requested <= 0)
		return lmms::AiBridgeAssetTypes::DefaultAssetPageSize;
	return qMin(requested, lmms::AiBridgeAssetTypes::MaxAssetPageSize);
}

} // namespace

namespace lmms
{

QVector<QString> AiBridgeAssetCatalog::trustedRoots(AiBridgeAssetKind kind)
{
	const QString folder = kind == AiBridgeAssetKind::Sample ? QStringLiteral("samples")
		: QStringLiteral("presets");
	QVector<QString> roots;
	const QString applicationRoot = QDir(QCoreApplication::applicationDirPath())
		.filePath(QStringLiteral("data/") + folder);
	const QFileInfo applicationInfo(applicationRoot);
	if (applicationInfo.exists() && applicationInfo.isDir())
		roots.append(applicationInfo.canonicalFilePath());
#ifdef LMMS_PHASE3E_SOURCE_ROOT
	const QString developmentRoot = QDir(QStringLiteral(LMMS_PHASE3E_SOURCE_ROOT))
		.filePath(QStringLiteral("data/") + folder);
	const QFileInfo developmentInfo(developmentRoot);
	if (developmentInfo.exists() && developmentInfo.isDir())
	{
		const QString canonicalDevelopmentRoot = developmentInfo.canonicalFilePath();
		if (!roots.contains(canonicalDevelopmentRoot))
			roots.append(canonicalDevelopmentRoot);
	}
#endif
	return roots;
}

bool AiBridgeAssetCatalog::canonicalContained(const QString &root, const QString &target)
{
	const QFileInfo rootInfo(root);
	const QFileInfo targetInfo(target);
	if (!rootInfo.exists() || !rootInfo.isDir() || !targetInfo.exists())
		return false;
	const QString canonicalRoot = rootInfo.canonicalFilePath();
	const QString canonicalTarget = targetInfo.canonicalFilePath();
	return componentContained(canonicalRoot, canonicalTarget);
}

std::optional<QString> AiBridgeAssetCatalog::makeAssetId(
	AiBridgeAssetKind kind, const QString &instrumentId, const QString &relativeResource)
{
	const QString instrument = normalizedRelative(instrumentId);
	const QString resource = normalizedRelative(relativeResource);
	if (instrument.isEmpty() || resource.isEmpty() || instrument.contains('/'))
		return std::nullopt;
	const QString id = kindName(kind) + '/' + instrument + '/' + resource;
	if (id.toUtf8().size() > AiBridgeAssetTypes::MaxAssetIdBytes)
		return std::nullopt;
	return id;
}

std::optional<QString> AiBridgeAssetCatalog::parseAssetId(
	const QString &id, AiBridgeAssetKind expectedKind)
{
	if (id.toUtf8().size() > AiBridgeAssetTypes::MaxAssetIdBytes)
		return std::nullopt;
	const QString normalized = QDir::fromNativeSeparators(id);
	const QStringList parts = normalized.split('/', Qt::KeepEmptyParts);
	if (parts.size() < 3 || parts.at(0) != kindName(expectedKind)
		|| parts.at(1).isEmpty() || parts.at(1).contains(':'))
		return std::nullopt;
	const QString resource = parts.mid(2).join('/');
	if (!makeAssetId(expectedKind, parts.at(1), resource).has_value())
		return std::nullopt;
	return resource;
}

std::optional<QString> AiBridgeAssetCatalog::resolveAssetId(
	const QString &id, AiBridgeAssetKind expectedKind)
{
	const auto relative = parseAssetId(id, expectedKind);
	if (!relative.has_value())
		return std::nullopt;
	for (const auto &root : trustedRoots(expectedKind))
	{
		const QString candidate = QDir(root).filePath(*relative);
		if (canonicalContained(root, candidate))
			return QFileInfo(candidate).canonicalFilePath();
	}
	return std::nullopt;
}

AiBridgeAssetPage<AiBridgeSampleValue> AiBridgeAssetCatalog::listBundledSamples(
	const AiBridgeAssetFilter &filter)
{
	const auto roots = trustedRoots(AiBridgeAssetKind::Sample);
	for (const auto &root : roots)
	{
		QDirIterator iterator(root, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
		while (iterator.hasNext())
		{
			const QString suffix = QStringLiteral(".") + QFileInfo(iterator.next()).suffix().toLower();
			if (suffix == QStringLiteral(".wav") || suffix == QStringLiteral(".ogg")
				|| suffix == QStringLiteral(".ds") || suffix == QStringLiteral(".flac"))
				return listBundledSamplesAtRoot(root, filter);
		}
	}
	return {};
}

AiBridgeAssetPage<AiBridgeSampleValue> AiBridgeAssetCatalog::listBundledSamplesAtRoot(
	const QString &root, const AiBridgeAssetFilter &filter)
{
	AiBridgeAssetPage<AiBridgeSampleValue> page;
	page.offset = qMax(0, filter.offset);
	page.limit = boundedLimit(filter.limit);
	if (!boundedFilter(filter.instrumentId) || !boundedFilter(filter.query)
		|| !boundedFilter(filter.category) || !boundedFilter(filter.format))
		return page;
	const QFileInfo rootInfo(root);
	if (!rootInfo.exists() || !rootInfo.isDir())
		return page;
	const QString canonicalRoot = rootInfo.canonicalFilePath();
	QVector<AiBridgeSampleValue> values;
	QDirIterator iterator(canonicalRoot, QDir::Files | QDir::Readable,
		QDirIterator::Subdirectories);
	int examined = 0;
	while (iterator.hasNext() && examined < AiBridgeAssetTypes::MaxEntriesExamined)
	{
		const QString physicalPath = iterator.next();
		++examined;
		const QFileInfo info(physicalPath);
		const QString relativeCandidate = QDir(canonicalRoot).relativeFilePath(info.canonicalFilePath());
		if (relativeCandidate.count('/') > AiBridgeAssetTypes::MaxScanDepth)
			continue;
		const QString suffix = QStringLiteral(".") + info.suffix().toLower();
		if (suffix != QStringLiteral(".wav") && suffix != QStringLiteral(".ogg")
			&& suffix != QStringLiteral(".ds") && suffix != QStringLiteral(".flac"))
			continue;
		if (!canonicalContained(canonicalRoot, physicalPath))
			continue;
		const QString relative = relativeCandidate;
		const auto id = makeAssetId(AiBridgeAssetKind::Sample,
			QStringLiteral("audiofileprocessor"), relative);
		if (!id.has_value())
			continue;
		AiBridgeSampleValue value;
		value.assetId = *id;
		value.displayName = info.completeBaseName();
		value.instrumentId = QStringLiteral("audiofileprocessor");
		const QString parent = QFileInfo(relative).path();
		value.category = parent == QStringLiteral(".") ? QString() : parent;
		value.format = suffix;
		if (filter.instrumentId != value.instrumentId
			&& !filter.instrumentId.isEmpty())
			continue;
		if (!filter.format.isEmpty() && filter.format.toLower() != suffix
			&& (QStringLiteral(".") + filter.format.toLower()) != suffix)
			continue;
		const QString query = filter.query.toCaseFolded();
		if (!query.isEmpty() && !relative.toCaseFolded().contains(query))
			continue;
		if (!filter.category.isEmpty() && value.category != filter.category)
			continue;
		values.append(value);
	}
	std::sort(values.begin(), values.end(), [](const auto &left, const auto &right) {
		const QString l = left.assetId.toCaseFolded();
		const QString r = right.assetId.toCaseFolded();
		return l == r ? left.assetId < right.assetId : l < r;
	});
	page.total = values.size();
	page.hasMore = page.offset < page.total && page.offset + page.limit < page.total;
	for (int index = page.offset; index < qMin(page.offset + page.limit, page.total); ++index)
		page.items.append(values.at(index));
	return page;
}

AiBridgeAssetPage<AiBridgePresetValue> AiBridgeAssetCatalog::listBundledPresets(
	const AiBridgeAssetFilter &)
{
	return {};
}

} // namespace lmms
