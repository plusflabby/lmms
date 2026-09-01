#include "AiBridgeWindowContext.h"

#include <array>
#include <algorithm>

#include <QRandomGenerator>
#include <QCoreApplication>
#include <QThread>

#include "GuiApplication.h"
#include "MainWindow.h"
#include "SubWindow.h"
#include "AutomationEditor.h"
#include "ControllerRackView.h"
#include "MicrotunerConfig.h"
#include "MixerView.h"
#include "PatternEditor.h"
#include "PianoRoll.h"
#include "ProjectNotes.h"
#include "SongEditor.h"

namespace lmms
{

namespace
{

constexpr int MaxRegisteredWindows = 256;

QString createSessionToken()
{
	return QString::number(QRandomGenerator::global()->generate64(), 16);
}

bool containsKnownWidget(const QMdiSubWindow* wrapper, const QWidget* knownWidget)
{
	if (wrapper == nullptr || knownWidget == nullptr || wrapper->widget() == nullptr)
	{
		return false;
	}

	const auto* managedWidget = wrapper->widget();
	return managedWidget == knownWidget || managedWidget->isAncestorOf(knownWidget);
}

} // namespace

AiBridgeWindowRegistry::AiBridgeWindowRegistry() : m_sessionToken{createSessionToken()}
{
}

QString AiBridgeWindowRegistry::registerWindow(const QString& id, QMdiSubWindow* wrapper)
{
	if (id.isEmpty() || wrapper == nullptr)
	{
		return {};
	}

	Entry entry;
	entry.wrapper = wrapper;
	entry.current = true;
	entry.known = !id.startsWith(QStringLiteral("dynamic:"));
	entry.order = m_nextOrder++;
	m_entries.insert(id, entry);
	trimToBound();
	return id;
}

QString AiBridgeWindowRegistry::registerDynamicWindow(QMdiSubWindow* wrapper)
{
	if (wrapper == nullptr)
	{
		return {};
	}

	for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
	{
		if (it->wrapper == wrapper)
		{
			return it.key();
		}
	}

	const auto id = QStringLiteral("dynamic:%1:%2").arg(m_sessionToken).arg(m_nextDynamicId++);
	return registerWindow(id, wrapper);
}

AiBridgeWindowLookup AiBridgeWindowRegistry::lookup(const QString& id) const
{
	const auto it = m_entries.constFind(id);
	if (it == m_entries.cend())
	{
		return AiBridgeWindowLookup::unknown;
	}

	return it->current && !it->wrapper.isNull() ? AiBridgeWindowLookup::live
																				: AiBridgeWindowLookup::stale;
}

QString AiBridgeWindowRegistry::idFor(QMdiSubWindow* wrapper) const
{
	for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
	{
		if (it->current && it->wrapper == wrapper)
		{
			return it.key();
		}
	}
	return {};
}

void AiBridgeWindowRegistry::reconcile(const QList<QMdiSubWindow*>& wrappers,
	const QHash<QMdiSubWindow*, QString>& knownIds)
{
	for (auto& entry : m_entries)
	{
		entry.current = false;
	}

	for (auto* wrapper : wrappers)
	{
		if (wrapper == nullptr)
		{
			continue;
		}

		const auto knownId = knownIds.value(wrapper);
		if (!knownId.isEmpty())
		{
			for (auto it = m_entries.begin(); it != m_entries.end();)
			{
				if (it->wrapper == wrapper && it.key() != knownId)
				{
					it = m_entries.erase(it);
				}
				else
				{
					++it;
				}
			}

			Entry entry;
			entry.wrapper = wrapper;
			entry.current = true;
			entry.known = true;
			entry.order = m_nextOrder++;
			m_entries.insert(knownId, entry);
			continue;
		}

		QString existingId;
		for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
		{
			if (it->wrapper == wrapper)
			{
				existingId = it.key();
				break;
			}
		}

		if (existingId.isEmpty())
		{
			existingId = QStringLiteral("dynamic:%1:%2").arg(m_sessionToken).arg(m_nextDynamicId++);
		}

		Entry entry;
		entry.wrapper = wrapper;
		entry.current = true;
		entry.known = existingId.startsWith(QStringLiteral("dynamic:")) == false;
		entry.order = m_nextOrder++;
		m_entries.insert(existingId, entry);
	}

	trimToBound();
}

void AiBridgeWindowRegistry::trimToBound()
{
	while (m_entries.size() > MaxRegisteredWindows)
	{
		auto candidate = m_entries.cend();
		for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
		{
			if ((!it->current || it->wrapper.isNull()) && !it->known)
			{
				candidate = it;
				break;
			}
			if (!it->known && (candidate == m_entries.cend() || it->order < candidate->order))
			{
				candidate = it;
			}
		}
		if (candidate == m_entries.cend())
		{
			return;
		}
		m_entries.remove(candidate.key());
	}
}

bool aiBridgeWindowRectsOverlap(const QRect& first, const QRect& second)
{
	return first.intersects(second);
}

AiBridgeWindowRegistry AiBridgeWindowContext::s_registry;

void AiBridgeWindowContext::reconcileRegistry() const
{
	QList<QMdiSubWindow*> wrappers;
	QHash<QMdiSubWindow*, QString> knownIds;

	auto* application = gui::getGUI();
	if (application == nullptr || application->mainWindow() == nullptr ||
		application->mainWindow()->workspace() == nullptr)
	{
		s_registry.reconcile(wrappers, knownIds);
		return;
	}

	auto* workspace = application->mainWindow()->workspace();
	wrappers = workspace->subWindowList();

	struct KnownWindow
	{
		QString id;
		QWidget* widget;
	};

	const std::array knownWindows{
		KnownWindow{QStringLiteral("song_editor"), application->songEditor()},
		KnownWindow{QStringLiteral("piano_roll"), application->pianoRoll()},
		KnownWindow{QStringLiteral("mixer"), application->mixerView()},
		KnownWindow{QStringLiteral("pattern_editor"), application->patternEditor()},
		KnownWindow{QStringLiteral("automation_editor"), application->automationEditor()},
		KnownWindow{QStringLiteral("project_notes"), application->getProjectNotes()},
		KnownWindow{QStringLiteral("controller_rack"), application->getControllerRackView()},
		KnownWindow{QStringLiteral("microtuner"), application->getMicrotunerConfig()}};

	for (auto* wrapper : wrappers)
	{
		for (const auto& known : knownWindows)
		{
			if (containsKnownWidget(wrapper, known.widget))
			{
				knownIds.insert(wrapper, known.id);
				break;
			}
		}
	}

	s_registry.reconcile(wrappers, knownIds);
}

AiBridgeWorkspaceSnapshot AiBridgeWindowContext::snapshot() const
{
	AiBridgeWorkspaceSnapshot result;
	if (QCoreApplication::instance() == nullptr ||
		QThread::currentThread() != QCoreApplication::instance()->thread())
	{
		result.code = QStringLiteral("wrong_thread");
		result.message = QStringLiteral("LMMS window snapshots require the Qt application thread");
		return result;
	}

	auto* application = gui::getGUI();
	if (application == nullptr || application->mainWindow() == nullptr ||
		application->mainWindow()->workspace() == nullptr)
	{
		result.code = QStringLiteral("gui_unavailable");
		result.message = QStringLiteral("LMMS GUI workspace is unavailable");
		return result;
	}

	auto* workspace = application->mainWindow()->workspace();
	result.success = true;
	result.code = QStringLiteral("ok");
	result.message = QStringLiteral("LMMS workspace snapshot captured");
	result.geometry = {0, 0, workspace->width(), workspace->height()};
	result.coordinateSpace = QStringLiteral("mdi_content");
	reconcileRegistry();

	for (auto* wrapper : workspace->subWindowList())
	{
		if (wrapper == nullptr)
		{
			continue;
		}
		AiBridgeWindowSnapshot window;
		window.id = s_registry.idFor(wrapper);
		window.kind = window.id.startsWith(QStringLiteral("dynamic:")) ?
			QStringLiteral("dynamic") : window.id;
		window.title = wrapper->windowTitle();
		window.detached = false;
		QWidget* displayed = wrapper;
		if (auto* subWindow = qobject_cast<gui::SubWindow*>(wrapper); subWindow != nullptr)
		{
			window.detached = subWindow->isDetached();
			if (window.detached && subWindow->widget() != nullptr)
			{
				displayed = subWindow->widget()->window();
			}
			window.normalGeometry = AiBridgeWindowRect{
				subWindow->getTrueNormalGeometry().x(), subWindow->getTrueNormalGeometry().y(),
				subWindow->getTrueNormalGeometry().width(), subWindow->getTrueNormalGeometry().height()};
		}
		const auto displayedGeometry = window.detached ? displayed->frameGeometry() : wrapper->geometry();
		window.geometry = {displayedGeometry.x(), displayedGeometry.y(), displayedGeometry.width(), displayedGeometry.height()};
		window.coordinateSpace = window.detached ? QStringLiteral("desktop_global") : QStringLiteral("mdi_content");
		window.visible = displayed->isVisible();
		window.active = workspace->activeSubWindow() == wrapper;
		window.maximized = wrapper->isMaximized();
		window.minimized = wrapper->isMinimized();
		window.capabilities = {window.visible && !window.detached, !window.detached,
			!window.detached && !wrapper->isMinimized(), window.visible, wrapper->minimumSize() == wrapper->maximumSize()};
		result.windows.append(window);
	}
	std::sort(result.windows.begin(), result.windows.end(), [](const auto& first, const auto& second) {
		return first.id < second.id;
	});
	for (int i = 0; i < result.windows.size(); ++i)
	{
		if (!result.windows.at(i).visible || result.windows.at(i).detached)
		{
			continue;
		}
		const QRect first{result.windows.at(i).geometry.x, result.windows.at(i).geometry.y,
			result.windows.at(i).geometry.width, result.windows.at(i).geometry.height};
		for (int j = 0; j < result.windows.size(); ++j)
		{
			if (i == j || !result.windows.at(j).visible || result.windows.at(j).detached)
			{
				continue;
			}
			const auto& other = result.windows.at(j);
			if (aiBridgeWindowRectsOverlap(first, QRect{other.geometry.x, other.geometry.y,
				other.geometry.width, other.geometry.height}))
			{
				result.windows[i].overlaps.append(other.id);
			}
		}
	}
	return result;
}

AiBridgeWindowLookup AiBridgeWindowContext::targetStatus(const QString& id) const
{
	if (QCoreApplication::instance() == nullptr ||
		QThread::currentThread() != QCoreApplication::instance()->thread())
	{
		return AiBridgeWindowLookup::wrong_thread;
	}
	reconcileRegistry();
	return s_registry.lookup(id);
}

QMdiSubWindow* AiBridgeWindowContext::resolveWindow(const QString& id) const
{
	if (targetStatus(id) != AiBridgeWindowLookup::live)
	{
		return nullptr;
	}
	auto* application = gui::getGUI();
	if (application == nullptr || application->mainWindow() == nullptr || application->mainWindow()->workspace() == nullptr)
	{
		return nullptr;
	}
	for (auto* wrapper : application->mainWindow()->workspace()->subWindowList())
	{
		if (s_registry.idFor(wrapper) == id)
		{
			return wrapper;
		}
	}
	return nullptr;
}

AiBridgeWindowSnapshot AiBridgeWindowContext::findSnapshot(const QString& id) const
{
	const auto windows = snapshot().windows;
	for (const auto& window : windows)
	{
		if (window.id == id)
		{
			return window;
		}
	}
	return {};
}

} // namespace lmms
