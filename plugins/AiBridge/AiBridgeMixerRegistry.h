#ifndef LMMS_AIBRIDGE_MIXER_REGISTRY_H
#define LMMS_AIBRIDGE_MIXER_REGISTRY_H

#include <QHash>
#include <QPointer>
#include <QString>

class QObject;

namespace lmms
{

class MixerRoute;

class AiBridgeMixerRegistry
{
public:
	AiBridgeMixerRegistry(QString sessionId, int projectGeneration);

	QString masterId() const;
	QString channelId(QObject* lifetimeToken, bool master = false);
	QString routeId(MixerRoute* route);
	MixerRoute* liveRoute(const QString& id) const;
	bool isLive(const QString& id) const;
	bool isRetired(const QString& id) const;
	void retireChannel(const QString& id);
	void retireProject();
	void retireSession();
	void retireRoute(const QString& id);

private:
	struct Record
	{
		QString id;
		QPointer<QObject> lifetimeToken;
		bool retired = false;
	};

	QString m_sessionId;
	int m_projectGeneration;
	QString m_masterId;
	quint64 m_nextToken = 1;
	QHash<QString, Record> m_records;
	QHash<QString, QPointer<MixerRoute>> m_routes;
	quint64 m_nextRouteToken = 1;
};

AiBridgeMixerRegistry &aiBridgeMixerRegistry();

} // namespace lmms

#endif
