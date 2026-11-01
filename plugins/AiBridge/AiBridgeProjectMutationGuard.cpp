#include "AiBridgeProjectMutationGuard.h"

#include <QSet>

#include <cmath>

namespace lmms
{

namespace
{
bool validPositiveNumber(const QJsonValue &value)
{
	if (!value.isDouble()) return false;
	const auto number = value.toDouble();
	return std::isfinite(number) && std::floor(number) == number && number > 0;
}

bool validDecimalString(const QJsonValue &value)
{
	if (!value.isString()) return false;
	const auto text = value.toString();
	if (text.isEmpty() || text.size() > 32) return false;
	for (const auto character : text)
		if (character < QLatin1Char('0') || character > QLatin1Char('9')) return false;
	return true;
}

bool hasOnlyKeys(const QJsonObject &object, const QSet<QString> &allowed)
{
	for (auto it = object.constBegin(); it != object.constEnd(); ++it)
		if (!allowed.contains(it.key())) return false;
	return true;
}

AiBridgeProjectGuardCheck invalidIdentity()
{
	return {false, QStringLiteral("invalid_parameter")};
}
} // namespace

bool AiBridgeProjectMutationGuard::isSha256(const QString &value)
{
	if (value.size() != 64) return false;
	for (const auto character : value)
		if (!QStringLiteral("0123456789abcdef").contains(character)) return false;
	return true;
}

AiBridgeProjectGuardCheck AiBridgeProjectMutationGuard::validateIdentity(const QJsonObject &identity)
{
	const QSet<QString> identityKeys{
		QStringLiteral("schema_version"), QStringLiteral("availability"),
		QStringLiteral("server_pid"), QStringLiteral("client_pid"),
		QStringLiteral("server_creation_time"), QStringLiteral("client_creation_time"),
		QStringLiteral("process_session_id"), QStringLiteral("project_load_generation"),
		QStringLiteral("identity_revision"), QStringLiteral("document_state"),
		QStringLiteral("dirty"), QStringLiteral("document_state_hash"),
		QStringLiteral("loaded_project")};
	if (!identity.value(QStringLiteral("availability")).isString()) return invalidIdentity();
	if (identity.value(QStringLiteral("availability")).toString() != QStringLiteral("available"))
		return {false, QStringLiteral("guard_unsupported")};
	if (!hasOnlyKeys(identity, identityKeys) || identity.size() != identityKeys.size()) return invalidIdentity();
	if (!identity.value(QStringLiteral("schema_version")).isDouble()
		|| identity.value(QStringLiteral("schema_version")).toInt() != 1
		|| !validPositiveNumber(identity.value(QStringLiteral("server_pid")))
		|| !validPositiveNumber(identity.value(QStringLiteral("client_pid")))
		|| !validDecimalString(identity.value(QStringLiteral("server_creation_time")))
		|| !validDecimalString(identity.value(QStringLiteral("client_creation_time")))
		|| !identity.value(QStringLiteral("process_session_id")).isString()
		|| identity.value(QStringLiteral("process_session_id")).toString().isEmpty()
		|| identity.value(QStringLiteral("process_session_id")).toString().toUtf8().size() > 128
		|| !validDecimalString(identity.value(QStringLiteral("project_load_generation")))
		|| !validDecimalString(identity.value(QStringLiteral("identity_revision")))
		|| !identity.value(QStringLiteral("document_state")).isString()
		|| !QSet<QString>{QStringLiteral("saved"), QStringLiteral("modified"), QStringLiteral("unsaved"), QStringLiteral("recovered")}.contains(identity.value(QStringLiteral("document_state")).toString())
		|| !identity.value(QStringLiteral("dirty")).isBool()
		|| !isSha256(identity.value(QStringLiteral("document_state_hash")).toString())
		|| !identity.value(QStringLiteral("loaded_project")).isObject())
		return invalidIdentity();

	const auto loaded = identity.value(QStringLiteral("loaded_project")).toObject();
	const QSet<QString> loadedKeys{QStringLiteral("canonical_path"), QStringLiteral("sha256"),
		QStringLiteral("file_id"), QStringLiteral("windows_file_id")};
	if (!hasOnlyKeys(loaded, loadedKeys)) return invalidIdentity();
	if (!loaded.isEmpty())
	{
		if (!loaded.value(QStringLiteral("canonical_path")).isString()
			|| loaded.value(QStringLiteral("canonical_path")).toString().isEmpty()
			|| loaded.value(QStringLiteral("canonical_path")).toString().toUtf8().size() > 32768
			|| !isSha256(loaded.value(QStringLiteral("sha256")).toString())
			|| !loaded.value(QStringLiteral("file_id")).isString()
			|| loaded.value(QStringLiteral("file_id")).toString().isEmpty()
			|| loaded.value(QStringLiteral("file_id")).toString().size() > 256) return invalidIdentity();
		if (loaded.contains(QStringLiteral("windows_file_id"))
			&& (!loaded.value(QStringLiteral("windows_file_id")).isString()
				|| loaded.value(QStringLiteral("windows_file_id")).toString().isEmpty()
				|| loaded.value(QStringLiteral("windows_file_id")).toString().size() > 256
				|| loaded.value(QStringLiteral("windows_file_id")) != loaded.value(QStringLiteral("file_id")))) return invalidIdentity();
	}
	else if (identity.value(QStringLiteral("document_state")).toString() != QStringLiteral("unsaved")
		&& identity.value(QStringLiteral("document_state")).toString() != QStringLiteral("recovered"))
	{
		return invalidIdentity();
	}
	return {true, QStringLiteral("ok")};
}

AiBridgeProjectGuardCheck AiBridgeProjectMutationGuard::compareIdentity(const QJsonObject &expected,
	const QJsonObject &actual)
{
	const auto expectedCheck = validateIdentity(expected);
	if (!expectedCheck.success) return expectedCheck;
	const auto actualCheck = validateIdentity(actual);
	if (!actualCheck.success) return actualCheck;
	return expected == actual ? AiBridgeProjectGuardCheck{true, QStringLiteral("ok")}
		: AiBridgeProjectGuardCheck{false, QStringLiteral("stale_project_identity")};
}

AiBridgeProjectGuardCheck AiBridgeProjectMutationGuard::compareHash(const QString &expected,
	const QString &actual, const QString &staleCode)
{
	if (!isSha256(expected)) return {false, QStringLiteral("invalid_parameter")};
	if (!isSha256(actual) || expected != actual) return {false, staleCode};
	return {true, QStringLiteral("ok")};
}

QJsonObject AiBridgeProjectMutationGuard::pathFreeIdentity(const QJsonObject &identity)
{
	QJsonObject result;
	for (const auto &key : {QStringLiteral("schema_version"), QStringLiteral("availability"),
		QStringLiteral("server_pid"), QStringLiteral("client_pid"),
		QStringLiteral("server_creation_time"), QStringLiteral("client_creation_time"),
		QStringLiteral("process_session_id"), QStringLiteral("project_load_generation"),
		QStringLiteral("identity_revision"), QStringLiteral("document_state"),
		QStringLiteral("dirty"), QStringLiteral("document_state_hash")})
		if (identity.contains(key)) result.insert(key, identity.value(key));
	const auto loaded = identity.value(QStringLiteral("loaded_project")).toObject();
	QJsonObject safeLoaded;
	for (const auto &key : {QStringLiteral("sha256"), QStringLiteral("file_id"), QStringLiteral("windows_file_id")})
		if (loaded.contains(key)) safeLoaded.insert(key, loaded.value(key));
	result.insert(QStringLiteral("loaded_project"), safeLoaded);
	return result;
}

} // namespace lmms
