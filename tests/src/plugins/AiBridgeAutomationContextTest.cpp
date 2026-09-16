#include <QtTest>

#include "AiBridgeAutomationContext.h"

class AiBridgeAutomationContextTest : public QObject
{
	Q_OBJECT

private slots:
	void freezesUnavailableNativeMarkers()
	{
		QVERIFY(!lmms::AiBridgeAutomationContext::nativeMarkersSupported);
		QVERIFY(!QStringLiteral("list_timeline_markers").contains(QStringLiteral("automation")));
	}
	void ordersAndPagesTargets()
	{
		lmms::AiBridgeAutomationTargets targets(QStringLiteral("session"), 1);
		lmms::AiBridgeAutomationTargetSnapshot metadata;
		metadata.capability = QStringLiteral("read_write");
		targets.registerTarget(QStringLiteral("owner"), QStringLiteral("z_class"), QStringLiteral("z"), metadata);
		targets.registerTarget(QStringLiteral("owner"), QStringLiteral("a_class"), QStringLiteral("a"), metadata);
		lmms::AiBridgeAutomationRegistry registry(QStringLiteral("session"), 1);
		lmms::AiBridgeAutomationContext context(registry, targets);
		const auto page = context.listTargets(0, 1);
		QCOMPARE(page.size(), 1);
		const auto all = context.listTargets(0, 8);
		QCOMPARE(all.size(), 2);
		QVERIFY(all.at(0).automationTargetId < all.at(1).automationTargetId);
	}

	void unsupportedTargetsRemainExplicit()
	{
		lmms::AiBridgeAutomationTargets targets(QStringLiteral("session"), 1);
		const auto unsupported = targets.registerUnsupported(QStringLiteral("timeline_marker"), QStringLiteral("unsupported"));
		QCOMPARE(unsupported.capability, QStringLiteral("unsupported"));
		lmms::AiBridgeAutomationRegistry registry(QStringLiteral("session"), 1);
		lmms::AiBridgeAutomationContext context(registry, targets);
		const auto page = context.capabilities(0, 8);
		QCOMPARE(page.size(), 0);
	}
};

QTEST_MAIN(AiBridgeAutomationContextTest)
#include "AiBridgeAutomationContextTest.moc"
