#ifndef LMMS_AI_BRIDGE_ENDPOINT_H
#define LMMS_AI_BRIDGE_ENDPOINT_H

#include <QByteArray>
#include <QString>

namespace lmms {
// A test instance can use an isolated endpoint without accepting a path or
// silently falling back to the production pipe when its suffix is invalid.
inline QString aiBridgeEndpoint(bool hasTestSuffix, const QByteArray& suffix)
{
	const auto base = QStringLiteral("lmms-aibridge-v2");
	if (!hasTestSuffix) return base;
	if (suffix.size() < 16 || suffix.size() > 64) return {};
	for (const auto c : suffix)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '_' || c == '-')) return {};
	return base + QLatin1Char('-') + QString::fromLatin1(suffix);
}
}
#endif
