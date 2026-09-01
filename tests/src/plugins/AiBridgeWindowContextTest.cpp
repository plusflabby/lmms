/*
 * AiBridgeWindowContextTest.cpp
 *
 * These tests define the Phase 3A value and lifetime boundary.
 */

#include <type_traits>

#include <QApplication>
#include <QMdiArea>
#include <QtTest>

#include "AiBridgeWindowContext.h"
#include "SubWindow.h"

class AiBridgeWindowContextTest : public QObject
{
	Q_OBJECT

private slots:
	void touchingRectanglesDoNotOverlap()
	{
		QVERIFY(!lmms::aiBridgeWindowRectsOverlap(QRect{0, 0, 10, 10}, QRect{10, 0, 10, 10}));
	}

	void positiveAreaRectanglesOverlap()
	{
		QVERIFY(lmms::aiBridgeWindowRectsOverlap(QRect{0, 0, 10, 10}, QRect{9, 0, 10, 10}));
	}

	void identicalRectanglesOverlap()
	{
		QVERIFY(lmms::aiBridgeWindowRectsOverlap(QRect{4, 8, 20, 12}, QRect{4, 8, 20, 12}));
	}

	void copiedSnapshotContainsOnlyValueData()
	{
		static_assert(std::is_copy_constructible_v<lmms::AiBridgeWindowSnapshot>);
		static_assert(std::is_copy_assignable_v<lmms::AiBridgeWindowSnapshot>);
		static_assert(std::is_same_v<decltype(lmms::AiBridgeWindowSnapshot{}.geometry), lmms::AiBridgeWindowRect>);
		static_assert(std::is_same_v<decltype(lmms::AiBridgeWindowSnapshot{}.normalGeometry),
			std::optional<lmms::AiBridgeWindowRect>>);

		lmms::AiBridgeWindowSnapshot original;
		original.id = QStringLiteral("dynamic:test:1");
		original.kind = QStringLiteral("dynamic");
		original.title = QStringLiteral("Test window");
		original.geometry = {10, 20, 300, 200};
		original.normalGeometry = lmms::AiBridgeWindowRect{11, 21, 301, 201};
		original.coordinateSpace = QStringLiteral("mdi_content");
		original.visible = true;
		original.capabilities = {true, true, false, true, false};
		original.overlaps = QStringList{QStringLiteral("song_editor")};

		const auto copy = original;
		original.id.clear();
		original.geometry = {};
		original.overlaps.clear();

		QCOMPARE(copy.id, QStringLiteral("dynamic:test:1"));
		QCOMPARE(copy.geometry.x, 10);
		QCOMPARE(copy.geometry.width, 300);
		QCOMPARE(copy.normalGeometry->height, 201);
		QCOMPARE(copy.overlaps, QStringList{QStringLiteral("song_editor")});
	}

	void destroyedRegisteredWrapperBecomesStaleWithoutDereference()
	{
		QMdiArea mdiArea;
		auto *wrapper = new lmms::gui::SubWindow(mdiArea.viewport());
		mdiArea.addSubWindow(wrapper);

		lmms::AiBridgeWindowRegistry registry;
		const auto id = registry.registerDynamicWindow(wrapper);
		QVERIFY(!id.isEmpty());
		QVERIFY(id.startsWith(QStringLiteral("dynamic:")));
		QVERIFY(registry.lookup(id) == lmms::AiBridgeWindowLookup::live);

		delete wrapper;

		QVERIFY(registry.lookup(id) == lmms::AiBridgeWindowLookup::stale);
	}
};

QTEST_MAIN(AiBridgeWindowContextTest)
#include "AiBridgeWindowContextTest.moc"
