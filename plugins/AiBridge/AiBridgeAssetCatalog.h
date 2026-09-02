#ifndef LMMS_AIBRIDGE_ASSET_CATALOG_H
#define LMMS_AIBRIDGE_ASSET_CATALOG_H

#include <QString>
#include <QVector>
#include <optional>

#include "AiBridgeAssetTypes.h"

namespace lmms
{

enum class AiBridgeAssetKind
{
	Preset,
	Sample
};

class AiBridgeAssetCatalog
{
public:
	static QVector<QString> trustedRoots(AiBridgeAssetKind kind);
	static bool canonicalContained(const QString &root, const QString &target);
	static std::optional<QString> makeAssetId(AiBridgeAssetKind kind,
		const QString &instrumentId, const QString &relativeResource);
	static std::optional<QString> parseAssetId(const QString &id,
		AiBridgeAssetKind expectedKind);
	static std::optional<QString> resolveAssetId(const QString &id,
		AiBridgeAssetKind expectedKind);
	static AiBridgeAssetPage<AiBridgeSampleValue> listBundledSamples(
		const AiBridgeAssetFilter &filter);
	static AiBridgeAssetPage<AiBridgeSampleValue> listBundledSamplesAtRoot(
		const QString &root, const AiBridgeAssetFilter &filter);
	static AiBridgeAssetPage<AiBridgePresetValue> listBundledPresets(
		const AiBridgeAssetFilter &filter);
};

} // namespace lmms

#endif
