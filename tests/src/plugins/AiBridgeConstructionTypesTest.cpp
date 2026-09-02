#include <QtTest>

#include "AiBridgeConstructionTypes.h"

class AiBridgeConstructionTypesTest : public QObject
{
	Q_OBJECT

private slots:
	void copiedValuesPreserveConstructionData()
	{
		lmms::AiBridgeInstrumentValue instrument;
		instrument.instrumentId = QStringLiteral("tripleoscillator");
		instrument.displayName = QStringLiteral("TripleOscillator");
		instrument.available = true;
		const auto copiedInstrument = instrument;
		QCOMPARE(copiedInstrument.instrumentId, instrument.instrumentId);

		lmms::AiBridgeConstructionPage page;
		page.total = 2;
		page.offset = 1;
		page.limit = 1;
		page.hasMore = true;
		page.items.append(instrument);
		const auto copiedPage = page;
		QCOMPARE(copiedPage.total, 2);
		QCOMPARE(copiedPage.items.size(), 1);
		QCOMPARE(copiedPage.items.front().instrumentId, QStringLiteral("tripleoscillator"));
		QVERIFY(copiedPage.hasMore);
	}

	void valueTypesContainOnlyCopiedFields()
	{
		lmms::AiBridgeTrackConstructionValue track;
		track.trackId = QStringLiteral("track:test:1:1");
		track.volume = 100;
		track.panning = 0;
		lmms::AiBridgeClipConstructionValue clip;
		clip.clipId = QStringLiteral("clip:test:1:1");
		clip.trackId = track.trackId;
		clip.startTicks = 0;
		clip.lengthTicks = 192;
		QCOMPARE(clip.trackId, track.trackId);
		QCOMPARE(clip.lengthTicks, 192);
	}
};

QTEST_MAIN(AiBridgeConstructionTypesTest)
#include "AiBridgeConstructionTypesTest.moc"
