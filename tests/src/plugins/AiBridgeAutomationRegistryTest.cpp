#include <QtTest>

#include "AiBridgeAutomationRegistry.h"

#include <QObject>

class AiBridgeAutomationRegistryTest : public QObject
{
	Q_OBJECT

private slots:
	void stableIdsAndParentRetirement()
	{
		lmms::AiBridgeAutomationRegistry registry(QStringLiteral("session"), 7);
		QObject track;
		QObject clip;
		const auto trackId = registry.automationTrackId(&track);
		const auto clipId = registry.automationClipId(&clip, trackId);
		QCOMPARE(trackId, QStringLiteral("automation-track:session:7:1"));
		QCOMPARE(clipId, QStringLiteral("automation-clip:session:7:1"));
		QVERIFY(!trackId.contains(QStringLiteral("index")));
		QCOMPARE(registry.automationTrackId(&track), trackId);
		QCOMPARE(registry.automationClipId(&clip, trackId), clipId);
		QVERIFY(registry.isLive(trackId));
		QVERIFY(registry.isLive(clipId));

		registry.retireTrack(trackId);
		QVERIFY(registry.isRetired(trackId));
		QVERIFY(registry.isRetired(clipId));
		QVERIFY(!registry.isLive(trackId));
		QVERIFY(!registry.isLive(clipId));
	}

	void deletionAndRecreationDoNotReuseIds()
	{
		lmms::AiBridgeAutomationRegistry registry(QStringLiteral("session"), 1);
		auto *first = new QObject;
		const auto firstId = registry.automationTrackId(first);
		delete first;
		QVERIFY(registry.isRetired(firstId));
		auto *replacement = new QObject;
		const auto replacementId = registry.automationTrackId(replacement);
		QVERIFY(firstId != replacementId);
		QVERIFY(registry.isLive(replacementId));
		delete replacement;
	}

	void projectAndSessionRetirement()
	{
		lmms::AiBridgeAutomationRegistry registry(QStringLiteral("session"), 3);
		QObject track;
		const auto id = registry.automationTrackId(&track);
		registry.retireProject();
		QVERIFY(registry.isRetired(id));
		QVERIFY(registry.resolveTrackToken(id) == nullptr);

		lmms::AiBridgeAutomationRegistry next(QStringLiteral("new-session"), 1);
		QObject nextTrack;
		const auto nextId = next.automationTrackId(&nextTrack);
		next.retireSession();
		QVERIFY(next.isRetired(nextId));
		QVERIFY(next.resolveTrackToken(nextId) == nullptr);
		QVERIFY(id != nextId);
	}
};

QTEST_MAIN(AiBridgeAutomationRegistryTest)
#include "AiBridgeAutomationRegistryTest.moc"
