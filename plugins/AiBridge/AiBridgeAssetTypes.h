#ifndef LMMS_AIBRIDGE_ASSET_TYPES_H
#define LMMS_AIBRIDGE_ASSET_TYPES_H

#include <QString>
#include <QVector>

namespace lmms
{

struct AiBridgeAssetTypes
{
	static constexpr int DefaultAssetPageSize = 64;
	static constexpr int MaxAssetPageSize = 256;
	static constexpr int MaxFilterBytes = 64;
	static constexpr int MaxAssetIdBytes = 512;
	static constexpr int MaxScanDepth = 8;
	static constexpr int MaxEntriesExamined = 4096;
};

struct AiBridgeAssetFilter
{
	QString instrumentId;
	QString query;
	QString category;
	QString format;
	int offset = 0;
	int limit = AiBridgeAssetTypes::DefaultAssetPageSize;
};

struct AiBridgeSampleValue
{
	QString assetId;
	QString displayName;
	QString instrumentId;
	QString category;
	QString format;
	qint64 durationFrames = 0;
	bool hasDuration = false;
};

struct AiBridgePresetValue
{
	QString assetId;
	QString displayName;
	QString instrumentId;
	QString category;
	QString format;
};

struct AiBridgeAssetLoadValue
{
	QString assetId;
	QString trackId;
	QString trackName;
	QString instrumentId;
};

struct AiBridgeAssetValidationResult
{
	bool valid = false;
	QString code;
	QString message;
};

template <typename T>
struct AiBridgeAssetPage
{
	QVector<T> items;
	int total = 0;
	int offset = 0;
	int limit = 0;
	bool hasMore = false;
};

} // namespace lmms

#endif
