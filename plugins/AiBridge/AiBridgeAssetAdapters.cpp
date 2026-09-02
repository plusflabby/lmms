#include "AiBridgeAssetAdapters.h"

#include <QFileInfo>

#include "InstrumentTrack.h"
#include "Instrument.h"
#include "AiBridgeAssetCatalog.h"

namespace
{
lmms::AiBridgeAssetValidationResult invalid(const QString &code, const QString &message)
{
	return {false, code, message};
}
}

namespace lmms
{

bool AudioFileProcessorAssetAdapter::isCompatible(const Instrument *instrument)
{
	return instrument != nullptr && instrument->nodeName() == QStringLiteral("audiofileprocessor");
}

AiBridgeAssetValidationResult AudioFileProcessorAssetAdapter::validate(const QString &sampleId)
{
	const auto relative = AiBridgeAssetCatalog::parseAssetId(sampleId, AiBridgeAssetKind::Sample);
	if (!relative.has_value())
		return invalid(QStringLiteral("invalid_asset_id"), QStringLiteral("invalid sample asset ID"));
	const auto path = AiBridgeAssetCatalog::resolveAssetId(sampleId, AiBridgeAssetKind::Sample);
	if (!path.has_value())
		return invalid(QStringLiteral("asset_not_found"), QStringLiteral("sample asset is unavailable"));
	const QString suffix = QStringLiteral(".") + QFileInfo(*path).suffix().toLower();
	if (suffix != QStringLiteral(".wav") && suffix != QStringLiteral(".ogg")
		&& suffix != QStringLiteral(".ds") && suffix != QStringLiteral(".flac"))
		return invalid(QStringLiteral("unsupported_asset_format"), QStringLiteral("unsupported sample format"));
	return {true, {}, {}};
}

AiBridgeAssetLoadValue AudioFileProcessorAssetAdapter::load(
	InstrumentTrack *track, const QString &sampleId, AiBridgeAssetValidationResult *validation)
{
	AiBridgeAssetValidationResult result = validate(sampleId);
	if (!track || !result.valid)
	{
		if (track == nullptr && result.valid)
			result = invalid(QStringLiteral("invalid_track"), QStringLiteral("track is unavailable"));
		if (validation)
			*validation = result;
		return {};
	}
	Instrument *instrument = track->instrument();
	if (!isCompatible(instrument))
	{
		result = invalid(QStringLiteral("asset_incompatible"), QStringLiteral("track instrument is incompatible"));
		if (validation)
			*validation = result;
		return {};
	}
	const auto path = AiBridgeAssetCatalog::resolveAssetId(sampleId, AiBridgeAssetKind::Sample);
	const QString name = track->name();
	const bool invoked = QMetaObject::invokeMethod(instrument, "setAudioFile",
		Qt::DirectConnection, Q_ARG(QString, *path), Q_ARG(bool, false));
	if (!invoked)
	{
		result = invalid(QStringLiteral("sample_load_failed"),
			QStringLiteral("AudioFileProcessor sample load could not be invoked"));
		if (validation)
			*validation = result;
		return {};
	}
	if (track->name() != name)
		track->setName(name);
	if (validation)
		*validation = {true, {}, {}};
	AiBridgeAssetLoadValue value;
	value.assetId = sampleId;
	value.trackName = track->name();
	value.instrumentId = QStringLiteral("audiofileprocessor");
	return value;
}

} // namespace lmms
