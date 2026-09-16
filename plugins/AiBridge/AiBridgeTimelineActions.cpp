#include "AiBridgeTimelineActions.h"

#include "Clip.h"
#include "Engine.h"
#include "Song.h"

#include <QSet>

#include <limits>

namespace lmms
{

namespace
{
AiBridgeTimelineActionResult fail(const QString &code, const QString &message)
{
	return {false, code, message};
}

AiBridgeTimelineActionResult success(const QString &message)
{
	AiBridgeTimelineActionResult result;
	result.success = true;
	result.code = QStringLiteral("ok");
	result.message = message;
	return result;
}

Clip *resolveClip(const QString &id, AiBridgeProjectContext &context, AiBridgeTimelineActionResult &result)
{
	if (id.isEmpty())
	{
		result = fail(QStringLiteral("invalid_parameter"), QStringLiteral("clip_id is required"));
		return nullptr;
	}
	auto *clip = context.resolveClip(id);
	if (!clip)
	{
		result = fail(QStringLiteral("stale_clip"), QStringLiteral("clip is not live"));
	}
	return clip;
}
}

AiBridgeValidationResult AiBridgeTimelineActions::validateEdit(const AiBridgeTimelineEdit &edit)
{
	if (edit.clipId.isEmpty()) return {QStringLiteral("invalid_parameter"), QStringLiteral("clip_id is required")};
	if (!edit.startTick && !edit.lengthTicks && !edit.autoResize)
		return {QStringLiteral("invalid_parameter"), QStringLiteral("at least one edit is required")};
	if (edit.startTick && (*edit.startTick < 0 || *edit.startTick > std::numeric_limits<qint32>::max()))
		return {QStringLiteral("invalid_clip_position"), QStringLiteral("start_tick is outside the LMMS range")};
	if (edit.lengthTicks && (*edit.lengthTicks < 1 || *edit.lengthTicks > std::numeric_limits<qint32>::max()))
		return {QStringLiteral("invalid_clip_length"), QStringLiteral("length_ticks is outside the LMMS range")};
	if (edit.startTick && edit.lengthTicks && *edit.startTick > std::numeric_limits<qint32>::max() - *edit.lengthTicks)
		return {QStringLiteral("invalid_clip_length"), QStringLiteral("clip end is outside the LMMS range")};
	if (edit.autoResize && *edit.autoResize && edit.lengthTicks)
		return {QStringLiteral("invalid_clip_length"), QStringLiteral("explicit length cannot be combined with auto_resize")};
	return {};
}

AiBridgeTimelineActionResult AiBridgeTimelineActions::setClip(const AiBridgeTimelineEdit &edit)
{
	const auto validation = validateEdit(edit);
	if (!validation.success()) return fail(validation.code, validation.message);
	AiBridgeProjectContext context;
	AiBridgeTimelineActionResult result;
	Clip *clip = resolveClip(edit.clipId, context, result);
	if (!clip) return result;
	if (edit.lengthTicks && !clip->manuallyResizable())
		return fail(QStringLiteral("unsupported_clip_resize"), QStringLiteral("this clip type is not resizable"));
	const auto currentStart = clip->startPosition().getTicks();
	if (edit.lengthTicks && edit.startTick
		&& *edit.startTick > std::numeric_limits<qint32>::max() - *edit.lengthTicks)
		return fail(QStringLiteral("invalid_clip_length"), QStringLiteral("clip end is outside the LMMS range"));
	const bool changed = (edit.startTick && *edit.startTick != currentStart)
		|| (edit.lengthTicks && *edit.lengthTicks != clip->length().getTicks())
		|| (edit.autoResize && *edit.autoResize != clip->getAutoResize());
	if (changed)
	{
		clip->addJournalCheckPoint();
		if (edit.startTick) clip->movePosition(TimePos(static_cast<tick_t>(*edit.startTick)));
		if (edit.autoResize) clip->setAutoResize(*edit.autoResize);
		if (edit.lengthTicks) clip->changeLength(TimePos(static_cast<tick_t>(*edit.lengthTicks)));
		Engine::getSong()->setModified();
	}
	result = success(changed ? QStringLiteral("clip updated") : QStringLiteral("clip unchanged"));
	result.clipId = edit.clipId;
	result.trackId = clip->getTrack() ? context.idForTrack(clip->getTrack()) : QString();
	result.appliedClipIds.append(edit.clipId);
	return result;
}

AiBridgeTimelineActionResult AiBridgeTimelineActions::duplicateClip(const QString &clipId, qint64 destinationTick)
{
	if (destinationTick < 0 || destinationTick > std::numeric_limits<qint32>::max())
		return fail(QStringLiteral("invalid_clip_position"), QStringLiteral("destination_tick is outside the LMMS range"));
	AiBridgeProjectContext context;
	AiBridgeTimelineActionResult result;
	Clip *clip = resolveClip(clipId, context, result);
	if (!clip) return result;
	if (destinationTick > std::numeric_limits<qint32>::max() - clip->length().getTicks())
		return fail(QStringLiteral("invalid_clip_length"), QStringLiteral("duplicate end is outside the LMMS range"));
	auto *copy = clip->clone();
	if (!copy) return fail(QStringLiteral("clip_unavailable"), QStringLiteral("clip could not be duplicated"));
	copy->movePosition(TimePos(static_cast<tick_t>(destinationTick)));
	Engine::getSong()->setModified();
	result = success(QStringLiteral("clip duplicated"));
	result.clipId = context.idForClip(copy);
	result.trackId = copy->getTrack() ? context.idForTrack(copy->getTrack()) : QString();
	result.appliedClipIds.append(result.clipId);
	return result;
}

AiBridgeTimelineActionResult AiBridgeTimelineActions::deleteClip(const QString &clipId)
{
	AiBridgeProjectContext context;
	AiBridgeTimelineActionResult result;
	Clip *clip = resolveClip(clipId, context, result);
	if (!clip) return result;
	delete clip;
	Engine::getSong()->setModified();
	result = success(QStringLiteral("clip deleted"));
	result.clipId = clipId;
	result.appliedClipIds.append(clipId);
	return result;
}

AiBridgeTimelineActionResult AiBridgeTimelineActions::batchEdit(const QVector<AiBridgeTimelineEdit> &edits)
{
	if (edits.isEmpty()) return fail(QStringLiteral("invalid_parameter"), QStringLiteral("edits are required"));
	QSet<QString> unique;
	for (const auto &edit : edits)
	{
		const auto validation = validateEdit(edit);
		if (!validation.success()) return fail(validation.code, validation.message);
		if (unique.contains(edit.clipId)) return fail(QStringLiteral("duplicate_clip"), QStringLiteral("a clip may appear only once"));
		unique.insert(edit.clipId);
	}
	AiBridgeProjectContext context;
	for (const auto &edit : edits)
	{
		auto *clip = context.resolveClip(edit.clipId);
		if (!clip) return fail(QStringLiteral("stale_clip"), QStringLiteral("clip is not live"));
		if (edit.lengthTicks && !clip->manuallyResizable())
			return fail(QStringLiteral("unsupported_clip_resize"), QStringLiteral("this clip type is not resizable"));
	}
	AiBridgeTimelineActionResult result = success(QStringLiteral("clips updated"));
	for (const auto &edit : edits)
	{
		const auto applied = setClip(edit);
		if (!applied.success)
		{
			result.success = false;
			result.code = QStringLiteral("partial_failure");
			result.message = applied.message;
			result.unappliedClipIds = edits.mid(result.appliedClipIds.size()).isEmpty()
				? QVector<QString>() : [&]() { QVector<QString> ids; for (const auto &remaining : edits.mid(result.appliedClipIds.size())) ids.append(remaining.clipId); return ids; }();
			return result;
		}
	}
	return result;
}

} // namespace lmms
