#include <QtTest>

#include "AiBridgeProtocol.h"

class AiBridgePhase3HFrameSizeTest : public QObject
{
	Q_OBJECT

private slots:
	void neverTruncatesOversizedMidiReadOrPreviewResponses()
	{
		for (const auto action : {"get_midi_notes", "quantize_notes", "humanize_notes"})
		{
			const auto frame = lmms::AiBridgeProtocol::serializeResponse({2, action, true, "ok", "done", QJsonObject{{"clips", QJsonArray{QString(70000, QLatin1Char('x'))}}}});
			QVERIFY(frame.size() <= lmms::AiBridgeProtocol::MaxFrameBytes);
			QVERIFY(frame.contains("\"code\":\"result_too_large\""));
			QVERIFY(!frame.contains("\"clips\""));
			QVERIFY(frame.endsWith('\n'));
		}
	}
};

QTEST_MAIN(AiBridgePhase3HFrameSizeTest)
#include "AiBridgePhase3HFrameSizeTest.moc"
