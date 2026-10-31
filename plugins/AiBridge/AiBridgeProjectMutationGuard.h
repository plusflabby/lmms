#ifndef LMMS_AIBRIDGE_PROJECT_MUTATION_GUARD_H
#define LMMS_AIBRIDGE_PROJECT_MUTATION_GUARD_H

#include <QJsonObject>
#include <QString>

namespace lmms
{

struct AiBridgeProjectGuardCheck
{
	bool success = false;
	QString code;
};

class AiBridgeProjectMutationGuard
{
public:
	static bool isSha256(const QString &value);
	static AiBridgeProjectGuardCheck validateIdentity(const QJsonObject &identity);
	static AiBridgeProjectGuardCheck compareIdentity(const QJsonObject &expected,
		const QJsonObject &actual);
	static AiBridgeProjectGuardCheck compareHash(const QString &expected, const QString &actual,
		const QString &staleCode = QStringLiteral("stale_target"));
	static QJsonObject pathFreeIdentity(const QJsonObject &identity);
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_PROJECT_MUTATION_GUARD_H
