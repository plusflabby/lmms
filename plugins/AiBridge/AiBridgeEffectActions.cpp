#include "AiBridgeEffectActions.h"
#include "Effect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "Mixer.h"
#include "AiBridgeEffectCatalog.h"
#include "AiBridgeEffectAdapters.h"
#include "AiBridgeMixerRegistry.h"
#include <QApplication>
#include <QThread>
#include <algorithm>
#include <cmath>
#include "Song.h"
namespace lmms { namespace {
AiBridgeEffectRegistry& registry() { return aiBridgeEffectRegistry(); }
AiBridgeMixerRegistry& mixerRegistry() { return aiBridgeMixerRegistry(); }
AiBridgeEffectActionResult fail(const QString& c,const QString& m){return {false,c,m};}
bool qt(){return qApp && QThread::currentThread()==qApp->thread();}
AiBridgeEffectActionResult state(Effect* e,const QString& id,const QString& msg){auto r=fail("ok",msg);r.success=true;r.effectInstanceId=id;r.enabled=e->isEnabled();r.wetDry=e->wetLevel();r.position=e->effectChain()->indexOf(e);r.effectId=QString::fromUtf8(e->descriptor()->name);return r;}
QVector<AiBridgeMixerEffectSummary> copiedEffects(EffectChain* chain, const QString& channelId)
{
	QVector<AiBridgeMixerEffectSummary> result;
	for (int position = 0; position < chain->effectCount(); ++position)
	{
		auto* effect = chain->effectAt(position);
		AiBridgeMixerEffectSummary summary;
		summary.effectId = QString::fromUtf8(effect->descriptor()->name);
		summary.name = QString::fromUtf8(effect->descriptor()->displayName);
		summary.position = position;
		summary.effectInstanceId = registry().effectId(effect, channelId);
		result.append(std::move(summary));
	}
	return result;
}
void attachChain(AiBridgeEffectActionResult& result, Effect* effect, const QString& channelId)
{
	result.mixerChannelId = channelId;
	result.effects = copiedEffects(effect->effectChain(), channelId);
}

QString channelIdForChain(EffectChain* chain)
{
	if (chain == nullptr || Engine::mixer() == nullptr) return {};
	const auto& channels = Engine::mixer()->mixerChannels();
	for (std::size_t index = 0; index < channels.size(); ++index)
		if (&channels[index]->m_fxChain == chain)
			return mixerRegistry().channelId(channels[index]->lifetimeToken(), index == 0);
	return {};
}
}
bool AiBridgeEffectActions::validWetDry(float value) { return std::isfinite(value) && value >= -1.0f && value <= 1.0f; }
AiBridgeEffectActionResult AiBridgeEffectActions::add(const QString& channelId,const QString& effectId,int position)
{
	if(!qt()) return fail("wrong_thread","effect mutation requires Qt application thread");
	AiBridgeMixerContext mc(mixerRegistry()); auto* channel=mc.liveChannel(channelId); if(!channel)return fail("stale_mixer_channel","channel is not live");
	if(position<0 || position>channel->m_fxChain.effectCount())return fail("invalid_parameter","position is out of range");
	if(!AiBridgeEffectCatalog::isAllowed(effectId))return fail("effect_not_allowed","effect is not in the native allowlist");
	Effect* e=Effect::instantiate(effectId,&channel->m_fxChain,nullptr); if(!e)return fail("effect_unavailable","native effect is unavailable");
	channel->m_fxChain.appendEffect(e); while(channel->m_fxChain.indexOf(e)>position)channel->m_fxChain.moveUp(e);
	const auto id=registry().effectId(e,channelId); Engine::getSong()->setModified(); auto result=state(e,id,"effect added"); attachChain(result,e,channelId); return result;
}
AiBridgeEffectActionResult AiBridgeEffectActions::remove(const QString& id){if(!qt())return fail("wrong_thread","effect mutation requires Qt application thread");auto* e=registry().resolveEffect(id);if(!e)return fail("stale_effect","effect is not live");auto* chain=e->effectChain();chain->removeEffect(e);registry().retire(id);Engine::getSong()->setModified();auto r=fail("ok","effect removed");r.success=true;r.effectInstanceId=id;return r;}
AiBridgeEffectActionResult AiBridgeEffectActions::move(const QString& id,int position){if(!qt())return fail("wrong_thread","effect mutation requires Qt application thread");if(position<0)return fail("invalid_parameter","position is invalid");auto* e=registry().resolveEffect(id);if(!e)return fail("stale_effect","effect is not live");auto* chain=e->effectChain();if(position>=chain->effectCount())return fail("invalid_parameter","position is out of range");while(chain->indexOf(e)>position)chain->moveUp(e);while(chain->indexOf(e)<position)chain->moveDown(e);Engine::getSong()->setModified();auto r=state(e,id,"effect moved");attachChain(r,e,channelIdForChain(chain));return r;}
AiBridgeEffectActionResult AiBridgeEffectActions::setState(const QString& id,bool enabled,float wetDry){if(!qt())return fail("wrong_thread","effect mutation requires Qt application thread");if(!validWetDry(wetDry))return fail("invalid_parameter","wet_dry is outside [-1,1]");auto* e=registry().resolveEffect(id);if(!e)return fail("stale_effect","effect is not live");e->setEnabled(enabled);e->setWetLevel(wetDry);Engine::getSong()->setModified();auto r=state(e,id,"effect state updated");attachChain(r,e,channelIdForChain(e->effectChain()));return r;}

AiBridgeEffectParameterActionResult AiBridgeEffectActions::getParameters(const QString& id, int offset, int limit)
{
	AiBridgeEffectParameterActionResult result;
	if (!qt()) { result.code = "wrong_thread"; result.message = "parameter read requires Qt application thread"; return result; }
	if (!validPage(offset, limit, AiBridgeMixerLimits::maxParametersPerEffect)) { result.code = "invalid_parameter"; result.message = "invalid parameter page"; return result; }
	auto* effect = registry().resolveEffect(id);
	if (!effect) { result.code = "stale_effect"; result.message = "effect is not live"; return result; }
	const auto effectId = QString::fromUtf8(effect->descriptor()->name);
	const auto all = AiBridgeEffectAdapters::parameters(effectId, effect->controls());
	result.success = true; result.code = "ok"; result.message = "parameters read"; result.effectInstanceId = id;
	result.page.page = {static_cast<int>(all.size()), offset, limit, offset + limit < all.size()};
	result.page.items = all.mid(offset, limit);
	return result;
}

AiBridgeEffectParameterActionResult AiBridgeEffectActions::setParameters(const QString& id, const QVector<AiBridgeEffectParameterChange>& changes)
{
	AiBridgeEffectParameterActionResult result;
	if (!qt()) { result.code = "wrong_thread"; result.message = "parameter mutation requires Qt application thread"; return result; }
	if (changes.isEmpty() || changes.size() > AiBridgeMixerLimits::maxParameterChanges) { result.code = "invalid_parameter"; result.message = "invalid parameter change count"; return result; }
	auto* effect = registry().resolveEffect(id);
	if (!effect) { result.code = "stale_effect"; result.message = "effect is not live"; return result; }
	const auto effectId = QString::fromUtf8(effect->descriptor()->name);
	const auto before = AiBridgeEffectAdapters::parameters(effectId, effect->controls());
	QString validationError;
	if (!AiBridgeEffectAdapters::validateBatch(before, changes, &validationError)) { result.code = validationError; result.message = "parameter batch rejected"; return result; }
	for (const auto& change : changes) AiBridgeEffectAdapters::setParameter(effectId, effect->controls(), change.id, change.value);
	Engine::getSong()->setModified();
	result.success = true; result.code = "ok"; result.message = "parameters updated"; result.effectInstanceId = id;
	const auto after = AiBridgeEffectAdapters::parameters(effectId, effect->controls());
	result.page.page = {static_cast<int>(after.size()), 0, static_cast<int>(after.size()), false}; result.page.items = after;
	return result;
}
}
