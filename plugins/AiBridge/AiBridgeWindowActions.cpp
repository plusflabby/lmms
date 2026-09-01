#include "AiBridgeWindowActions.h"

#include <QApplication>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QThread>

namespace lmms
{

AiBridgeActionResult AiBridgeWindowActions::focusWindow(AiBridgeWindowContext& context, const QString& id)
{
	if (QApplication::instance() == nullptr || QThread::currentThread() != QApplication::instance()->thread())
		return AiBridgeActionResult::failure(QStringLiteral("wrong_thread"), QStringLiteral("window action requires Qt application thread"));
	auto* window = context.resolveWindow(id);
	if (window == nullptr) return AiBridgeActionResult::failure(QStringLiteral("unknown_window"), QStringLiteral("window is not live"));
	if (!window->isVisible()) return AiBridgeActionResult::failure(QStringLiteral("window_not_visible"), QStringLiteral("window is hidden"));
	if (auto* area = window->mdiArea(); area != nullptr) area->setActiveSubWindow(window);
	window->setFocus(Qt::OtherFocusReason);
	return AiBridgeActionResult::ok(QStringLiteral("window focused"));
}

AiBridgeActionResult AiBridgeWindowActions::setWindowGeometry(AiBridgeWindowContext& context, const QString& id,
	const AiBridgeWindowRect& geometry)
{
	if (QApplication::instance() == nullptr || QThread::currentThread() != QApplication::instance()->thread())
		return AiBridgeActionResult::failure(QStringLiteral("wrong_thread"), QStringLiteral("window action requires Qt application thread"));
	if (geometry.width <= 0 || geometry.height <= 0) return AiBridgeActionResult::failure(QStringLiteral("invalid_geometry"), QStringLiteral("geometry size must be positive"));
	auto* window = context.resolveWindow(id);
	if (window == nullptr) return AiBridgeActionResult::failure(QStringLiteral("unknown_window"), QStringLiteral("window is not live"));
	if (window->isMaximized() || window->isMinimized()) return AiBridgeActionResult::failure(QStringLiteral("window_state_conflict"), QStringLiteral("cannot change maximized or minimized window"));
	window->setGeometry(QRect{geometry.x, geometry.y, geometry.width, geometry.height});
	return AiBridgeActionResult::ok(QStringLiteral("window geometry updated"));
}

AiBridgeActionResult AiBridgeWindowActions::arrangeWindows(AiBridgeWindowContext& context, const QString& strategy)
{
	if (QApplication::instance() == nullptr || QThread::currentThread() != QApplication::instance()->thread())
		return AiBridgeActionResult::failure(QStringLiteral("wrong_thread"), QStringLiteral("window action requires Qt application thread"));
	if (strategy != QStringLiteral("tile") && strategy != QStringLiteral("cascade") && strategy != QStringLiteral("avoid_overlap") && strategy != QStringLiteral("music_editing"))
		return AiBridgeActionResult::failure(QStringLiteral("invalid_strategy"), QStringLiteral("unknown arrangement strategy"));
	const auto snapshot = context.snapshot();
	if (!snapshot.success) return AiBridgeActionResult::failure(snapshot.code, snapshot.message);
	int index = 0;
	for (const auto& item : snapshot.windows)
	{
		if (!item.visible || item.detached || !item.capabilities.targetable || item.maximized || item.minimized) continue;
		auto* window = context.resolveWindow(item.id);
		if (window == nullptr) continue;
		const int width = qMax(160, snapshot.geometry.width / 2);
		const int height = qMax(120, snapshot.geometry.height / 2);
		const int x = strategy == QStringLiteral("cascade") ? (index * 32) % qMax(1, snapshot.geometry.width - width) : (index % 2) * width;
		const int y = strategy == QStringLiteral("cascade") ? (index * 32) % qMax(1, snapshot.geometry.height - height) : (index / 2) * height;
		window->setGeometry(QRect{x, y, width, height});
		++index;
	}
	return AiBridgeActionResult::ok(QStringLiteral("windows arranged"));
}

} // namespace lmms
