#include <QtTest>

#include "AiBridgeEffectCatalog.h"

class AiBridgeEffectCatalogTest : public QObject
{
	Q_OBJECT

private slots:
	void allowlist()
	{
		QVERIFY(lmms::AiBridgeEffectCatalog::isAllowed("amplifier"));
		QVERIFY(lmms::AiBridgeEffectCatalog::isAllowed("reverbsc"));
		QVERIFY(!lmms::AiBridgeEffectCatalog::isAllowed("vst"));
		QVERIFY(!lmms::AiBridgeEffectCatalog::isAllowed("C:/plugin.dll"));
	}
};
QTEST_MAIN(AiBridgeEffectCatalogTest)
#include "AiBridgeEffectCatalogTest.moc"
