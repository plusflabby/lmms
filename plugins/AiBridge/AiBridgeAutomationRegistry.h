#ifndef LMMS_AIBRIDGE_AUTOMATION_REGISTRY_H
#define LMMS_AIBRIDGE_AUTOMATION_REGISTRY_H

#include <QHash>
#include <QPointer>
#include <QString>

class QObject;

namespace lmms
{

class AiBridgeAutomationRegistry
{
public:
	AiBridgeAutomationRegistry(QString sessionId, int projectGeneration);

	QString automationTrackId(QObject *trackToken);
	QString automationClipId(QObject *clipToken, const QString &trackId);
	QObject *resolveTrackToken(const QString &id) const;
	QObject *resolveClipToken(const QString &id) const;
	bool isLive(const QString &id) const;
	bool isRetired(const QString &id) const;
	void retireTrack(const QString &id);
	void retireClip(const QString &id);
	void retireProject();
	void retireSession();

private:
	struct Record
	{
		QString id;
		QPointer<QObject> token;
		QString ownerTrackId;
		bool track = false;
		bool retired = false;
	};

	QString m_sessionId;
	int m_projectGeneration;
	quint64 m_nextTrackToken = 1;
	quint64 m_nextClipToken = 1;
	QHash<QString, Record> m_records;
};

AiBridgeAutomationRegistry &aiBridgeAutomationRegistry();

} // namespace lmms

#endif
