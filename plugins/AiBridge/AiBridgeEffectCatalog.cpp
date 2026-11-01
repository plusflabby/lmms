#include "AiBridgeEffectCatalog.h"
#include "PluginFactory.h"
#include <algorithm>
namespace lmms {
namespace { constexpr const char* keys[] = {"amplifier", "compressor", "crossovereq", "delay", "reverbsc"}; }
bool AiBridgeEffectCatalog::isAllowed(const QString& key) { for (const auto* item : keys) if (key == QLatin1String(item)) return true; return false; }
AiBridgeEffectPage AiBridgeEffectCatalog::list(int offset, int limit, const QString& filter)
{
	AiBridgeEffectPage result; if (!validPage(offset, limit, AiBridgeMixerLimits::maxCatalogEntries)) return result;
	for (const auto* key : keys) {
		bool present = false; for (const auto* descriptor : getPluginFactory()->descriptors(Plugin::Type::Effect)) if (descriptor && QString::fromUtf8(descriptor->name) == QLatin1String(key)) present = true;
		if (present && (filter.isEmpty() || QString::fromUtf8(key).contains(filter, Qt::CaseInsensitive))) result.items.append({QString::fromUtf8(key), QString::fromUtf8(key), {}});
	}
	std::sort(result.items.begin(), result.items.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
	result.page = {static_cast<int>(result.items.size()), offset, limit, false}; auto all = result.items; result.items = all.mid(offset, limit); result.page.total = static_cast<int>(all.size()); result.page.hasMore = offset + limit < all.size(); return result;
}
}
