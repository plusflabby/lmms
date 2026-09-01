#include <QtTest>

#include <QPushButton>

#include "AiBridge.h"
#include "Engine.h"
#include "PluginView.h"

class AiBridgeViewTest : public QObject
{
	Q_OBJECT

private slots:
	void viewRemainsInsideItsLmmsSubWindow()
	{
		lmms::Engine::init(true);
		lmms::AiBridge bridge;
		auto *view = bridge.instantiateView(nullptr);
		QVERIFY(view != nullptr);

		const auto buttons = view->findChildren<QPushButton *>();
		QCOMPARE(buttons.size(), 3);
		for (auto *button : buttons)
		{
			QVERIFY(button->parentWidget() == view);
			QVERIFY(!button->isWindow());
		}
		delete view;
		lmms::Engine::destroy();
	}
};

QTEST_MAIN(AiBridgeViewTest)
#include "AiBridgeViewTest.moc"
