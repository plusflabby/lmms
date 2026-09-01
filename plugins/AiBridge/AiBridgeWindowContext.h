#ifndef LMMS_AIBRIDGE_WINDOW_CONTEXT_H
#define LMMS_AIBRIDGE_WINDOW_CONTEXT_H

#include <optional>

#include <QHash>
#include <QList>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class QMdiSubWindow;
class QWidget;

namespace lmms
{

namespace gui
{
class SubWindow;
}

struct AiBridgeWindowRect
{
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct AiBridgeWindowCapabilities
{
	bool targetable = false;
	bool movable = false;
	bool resizable = false;
	bool focusable = false;
	bool fixedSize = false;
};

struct AiBridgeWindowSnapshot
{
	QString id;
	QString kind;
	QString title;
	AiBridgeWindowRect geometry;
	std::optional<AiBridgeWindowRect> normalGeometry;
	QString coordinateSpace;
	bool visible = false;
	bool active = false;
	bool detached = false;
	bool maximized = false;
	bool minimized = false;
	AiBridgeWindowCapabilities capabilities;
	QStringList overlaps;
};

struct AiBridgeWorkspaceSnapshot
{
	bool success = false;
	QString code;
	QString message;
	AiBridgeWindowRect geometry;
	QString coordinateSpace;
	QList<AiBridgeWindowSnapshot> windows;
};

enum class AiBridgeWindowLookup
{
	unknown,
	stale,
	wrong_thread,
	live
};

/**
 * Internal, lifetime-safe identity registry for MDI wrappers.
 *
 * The registry exposes only copied IDs and lookup status. The QObject
 * association remains private and is invalidated automatically when a wrapper
 * is destroyed.
 */
class AiBridgeWindowRegistry
{
public:
	AiBridgeWindowRegistry();

	QString registerDynamicWindow(QMdiSubWindow* wrapper);
	AiBridgeWindowLookup lookup(const QString& id) const;

private:
	struct Entry
	{
		QPointer<QMdiSubWindow> wrapper;
		bool current = false;
		bool known = false;
		quint64 order = 0;
	};

	QString registerWindow(const QString& id, QMdiSubWindow* wrapper);
	QString idFor(QMdiSubWindow* wrapper) const;
	void reconcile(const QList<QMdiSubWindow*>& wrappers,
		const QHash<QMdiSubWindow*, QString>& knownIds);
	void trimToBound();

	QHash<QString, Entry> m_entries;
	QString m_sessionToken;
	quint64 m_nextDynamicId = 1;
	quint64 m_nextOrder = 1;

	friend class AiBridgeWindowContext;
};

bool aiBridgeWindowRectsOverlap(const QRect& first, const QRect& second);

class AiBridgeWindowContext
{
public:
	AiBridgeWindowContext() = default;

	// Snapshot enumeration is completed by the next window-management
	// checkpoint. This value-only placeholder keeps the boundary available.
	AiBridgeWorkspaceSnapshot snapshot() const;

	AiBridgeWindowLookup targetStatus(const QString& id) const;
	AiBridgeWindowSnapshot findSnapshot(const QString& id) const;

private:
	void reconcileRegistry() const;
	QMdiSubWindow* resolveWindow(const QString& id) const;

	static AiBridgeWindowRegistry s_registry;
	friend class AiBridgeWindowActions;
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_WINDOW_CONTEXT_H
