#include <QtTest/QtTest>

#include "AiBridgeAssetAdapters.h"

class AiBridgeAssetAdaptersTest : public QObject
{
	Q_OBJECT

private slots:
	void rejectsMalformedAndUnsupportedSamplesBeforeMutation()
	{
		const auto malformed = lmms::AudioFileProcessorAssetAdapter::validate(
			QStringLiteral("sample/audiofileprocessor/../secret.wav"));
		QCOMPARE(malformed.code, QStringLiteral("invalid_asset_id"));
		const auto unsupported = lmms::AudioFileProcessorAssetAdapter::validate(
			QStringLiteral("sample/audiofileprocessor/file.mp3"));
		QCOMPARE(unsupported.code, QStringLiteral("asset_not_found"));
	}
};

QTEST_MAIN(AiBridgeAssetAdaptersTest)
#include "AiBridgeAssetAdaptersTest.moc"
