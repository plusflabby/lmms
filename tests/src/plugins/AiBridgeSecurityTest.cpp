#include <QtTest>
#include "AiBridgeEffectCatalog.h"

class AiBridgeSecurityTest : public QObject
{
	Q_OBJECT
private slots:
	void onlyCompiledNativeKeysAreAllowed()
	{
		QVERIFY(lmms::AiBridgeEffectCatalog::isAllowed(QStringLiteral("amplifier")));
		QVERIFY(!lmms::AiBridgeEffectCatalog::isAllowed(QStringLiteral("vst")));
		QVERIFY(!lmms::AiBridgeEffectCatalog::isAllowed(QStringLiteral("carla")));
		QVERIFY(!lmms::AiBridgeEffectCatalog::isAllowed(QStringLiteral("arbitrary.dll")));
	}
};
QTEST_MAIN(AiBridgeSecurityTest)
#include "AiBridgeSecurityTest.moc"
