#include "AiBridgeMidiPlanner.h"

#include <algorithm>
#include <cmath>

namespace lmms
{
namespace
{
qint64 floorToGrid(qint64 tick, qint64 grid)
{
	if (tick >= 0) return tick / grid * grid;
	return -(((-tick + grid - 1) / grid) * grid);
}

qint64 roundHalfAwayFromZero(double value)
{
	return value >= 0.0 ? static_cast<qint64>(std::floor(value + 0.5))
		: static_cast<qint64>(std::ceil(value - 0.5));
}

qint64 quantizeTarget(qint64 source, const AiBridgeMidiQuantizeOptions &options)
{
	const auto earlier = floorToGrid(source, options.gridTicks);
	const auto later = earlier == source ? source : earlier + options.gridTicks;
	if (options.direction == AiBridgeMidiQuantizeDirection::Earlier) return earlier;
	if (options.direction == AiBridgeMidiQuantizeDirection::Later) return later;
	return source - earlier <= later - source ? earlier : later;
}

quint32 nextRandom(quint32 &state)
{
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

qint64 randomOffset(quint32 &state, qint64 range, AiBridgeMidiHumanizeDistribution distribution)
{
	if (range == 0) return 0;
	const auto uniform = [&state, range]() { return static_cast<qint64>(nextRandom(state) % (2 * range + 1)) - range; };
	if (distribution == AiBridgeMidiHumanizeDistribution::Uniform) return uniform();
	return roundHalfAwayFromZero((uniform() + uniform()) / 2.0);
}
}

AiBridgeMidiQuantizePlan planQuantize(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices, const AiBridgeMidiQuantizeOptions &options)
{
	AiBridgeMidiQuantizePlan result;
	result.notes = canonicalizeMidiNotes(snapshot.notes);
	if (options.gridTicks < 1 || options.strength < 0.0 || options.strength > 1.0
		|| (options.maxShiftTicks && *options.maxShiftTicks < 0))
	{
		result.code = QStringLiteral("invalid_request");
		return result;
	}
	for (const int index : canonicalIndices)
	{
		if (index < 0 || index >= result.notes.size())
		{
			result.code = QStringLiteral("invalid_selection");
			return result;
		}
	}
	for (const int index : canonicalIndices)
	{
		auto &note = result.notes[index];
		const auto source = note.positionTicks;
		const auto target = quantizeTarget(source, options);
		if (options.maxShiftTicks && qAbs(target - source) > *options.maxShiftTicks)
		{
			++result.skippedCount;
			continue;
		}
		auto planned = source + roundHalfAwayFromZero((target - source) * options.strength);
		if (planned < 0)
		{
			if (options.boundaryPolicy == AiBridgeMidiBoundaryPolicy::Reject)
			{
				result.notes = canonicalizeMidiNotes(snapshot.notes);
				result.code = QStringLiteral("boundary_rejected");
				return result;
			}
			planned = 0;
		}
		if (planned != source)
		{
			note.positionTicks = planned;
			++result.changedCount;
		}
	}
	result.success = true;
	result.stepCount = result.changedCount;
	if (result.changedCount == 0) result.code = QStringLiteral("no_effect");
	return result;
}

AiBridgeMidiHumanizePlan planHumanize(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices, const AiBridgeMidiHumanizeOptions &options)
{
	AiBridgeMidiHumanizePlan result;
	result.notes = canonicalizeMidiNotes(snapshot.notes);
	if (options.timingRangeTicks < 0 || options.volumeRange < 0 || options.lengthRangeTicks < 0)
	{
		result.code = QStringLiteral("invalid_request");
		return result;
	}
	for (const int index : canonicalIndices)
	{
		if (index < 0 || index >= result.notes.size())
		{
			result.code = QStringLiteral("invalid_selection");
			return result;
		}
	}
	quint32 state = options.seed == 0 ? 0x6D2B79F5u : options.seed;
	for (const int index : canonicalIndices)
	{
		auto &note = result.notes[index];
		const auto old = note;
		const auto position = note.positionTicks + randomOffset(state, options.timingRangeTicks, options.distribution);
		const auto volume = note.volume + randomOffset(state, options.volumeRange, options.distribution);
		const auto length = note.lengthTicks + randomOffset(state, options.lengthRangeTicks, options.distribution);
		if (position < 0 || volume < 0 || volume > 200 || length < 1)
		{
			if (options.boundaryPolicy == AiBridgeMidiBoundaryPolicy::Reject)
			{
				result.notes = canonicalizeMidiNotes(snapshot.notes);
				result.code = QStringLiteral("boundary_rejected");
				return result;
			}
			if (position < 0) { note.positionTicks = 0; ++result.clampCount; } else note.positionTicks = position;
			if (volume < 0 || volume > 200) { note.volume = qBound(0, static_cast<int>(volume), 200); ++result.clampCount; } else note.volume = volume;
			if (length < 1) { note.lengthTicks = 1; ++result.clampCount; } else note.lengthTicks = length;
		}
		else { note.positionTicks = position; note.volume = volume; note.lengthTicks = length; }
		if (note.positionTicks != old.positionTicks || note.volume != old.volume || note.lengthTicks != old.lengthTicks) ++result.changedCount;
	}
	result.success = true;
	if (result.changedCount == 0) result.code = QStringLiteral("no_effect");
	return result;
}

AiBridgeMidiHumanizePlan planGroove(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices, const AiBridgeMidiGrooveOptions &options)
{
	AiBridgeMidiHumanizePlan result;
	result.notes = canonicalizeMidiNotes(snapshot.notes);
	if (options.strength < 0.0 || options.strength > 1.0
		|| (options.mode == AiBridgeMidiGrooveMode::Swing && (options.subdivisionTicks < 1 || options.swingPercent < 50.0 || options.swingPercent > 75.0))
		|| (options.mode == AiBridgeMidiGrooveMode::Template && (options.stepTicks < 1 || options.timingOffsets.isEmpty())))
	{
		result.code = QStringLiteral("invalid_request"); return result;
	}
	for (const int index : canonicalIndices) if (index < 0 || index >= result.notes.size()) { result.code = QStringLiteral("invalid_selection"); return result; }
	for (const int index : canonicalIndices)
	{
		auto &note = result.notes[index]; const auto old = note;
		qint64 timing = 0; int volume = 0;
		if (options.mode == AiBridgeMidiGrooveMode::Swing)
		{
			const auto pair = 2 * options.subdivisionTicks;
			if (note.positionTicks % pair == options.subdivisionTicks)
				timing = roundHalfAwayFromZero((roundHalfAwayFromZero(pair * options.swingPercent / 100.0) - options.subdivisionTicks) * options.strength);
		}
		else
		{
			const auto step = (note.positionTicks / options.stepTicks) % options.timingOffsets.size();
			timing = roundHalfAwayFromZero(options.timingOffsets[step] * options.strength);
			if (!options.volumeOffsets.isEmpty()) volume = static_cast<int>(roundHalfAwayFromZero(options.volumeOffsets[step % options.volumeOffsets.size()] * options.strength));
		}
		const auto position = note.positionTicks + timing; const auto newVolume = note.volume + volume;
		if (position < 0 || newVolume < 0 || newVolume > 200)
		{
			if (options.boundaryPolicy == AiBridgeMidiBoundaryPolicy::Reject) { result.notes = canonicalizeMidiNotes(snapshot.notes); result.code = QStringLiteral("boundary_rejected"); return result; }
			if (position < 0) { note.positionTicks = 0; ++result.clampCount; } else note.positionTicks = position;
			if (newVolume < 0 || newVolume > 200) { note.volume = qBound(0, static_cast<int>(newVolume), 200); ++result.clampCount; } else note.volume = newVolume;
		}
		else { note.positionTicks = position; note.volume = newVolume; }
		if (note.positionTicks != old.positionTicks || note.volume != old.volume) ++result.changedCount;
	}
	result.success = true; if (result.changedCount == 0) result.code = QStringLiteral("no_effect"); return result;
}

AiBridgeMidiHumanizePlan planVelocityShape(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices, const AiBridgeMidiVelocityOptions &options)
{
	AiBridgeMidiHumanizePlan result;
	result.notes = canonicalizeMidiNotes(snapshot.notes);
	if ((options.mode != AiBridgeMidiValueMode::Set && options.mode != AiBridgeMidiValueMode::Offset
		&& options.mode != AiBridgeMidiValueMode::Scale && options.mode != AiBridgeMidiValueMode::Ramp)
		|| !std::isfinite(options.value))
	{
		result.code = QStringLiteral("invalid_request");
		return result;
	}
	auto selected = canonicalIndices;
	std::sort(selected.begin(), selected.end());
	for (const int index : selected)
	{
		if (index < 0 || index >= result.notes.size())
		{
			result.code = QStringLiteral("invalid_selection");
			return result;
		}
	}
	const auto original = result.notes;
	for (int selectedIndex = 0; selectedIndex < selected.size(); ++selectedIndex)
	{
		const auto noteIndex = selected[selectedIndex];
		auto &note = result.notes[noteIndex];
		double planned = 0.0;
		switch (options.mode)
		{
		case AiBridgeMidiValueMode::Set: planned = options.value; break;
		case AiBridgeMidiValueMode::Offset: planned = note.volume + options.value; break;
		case AiBridgeMidiValueMode::Scale: planned = note.volume * options.value; break;
		case AiBridgeMidiValueMode::Ramp:
			planned = selected.size() == 1 ? options.startValue : options.startValue
				+ (options.endValue - options.startValue) * selectedIndex / static_cast<double>(selected.size() - 1);
			break;
		default: break;
		}
		const auto rounded = roundHalfAwayFromZero(planned);
		if (rounded < 0 || rounded > 200)
		{
			if (options.boundaryPolicy == AiBridgeMidiBoundaryPolicy::Reject)
			{
				result.notes = canonicalizeMidiNotes(snapshot.notes);
				result.code = QStringLiteral("boundary_rejected");
				return result;
			}
			note.volume = rounded < 0 ? 0 : 200;
			++result.clampCount;
		}
		else note.volume = static_cast<int>(rounded);
		if (note.volume != original[noteIndex].volume) ++result.changedCount;
	}
	result.success = true;
	if (result.changedCount == 0) result.code = QStringLiteral("no_effect");
	return result;
}

AiBridgeMidiHumanizePlan planLengthShape(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices, const AiBridgeMidiLengthOptions &options)
{
	AiBridgeMidiHumanizePlan result;
	result.notes = canonicalizeMidiNotes(snapshot.notes);
	if ((options.mode != AiBridgeMidiValueMode::Set && options.mode != AiBridgeMidiValueMode::Offset
		&& options.mode != AiBridgeMidiValueMode::Scale && options.mode != AiBridgeMidiValueMode::GateToNext)
		|| !std::isfinite(options.value) || !std::isfinite(options.gatePercent) || options.gatePercent < 0.0)
	{
		result.code = QStringLiteral("invalid_request");
		return result;
	}
	auto selected = canonicalIndices;
	std::sort(selected.begin(), selected.end());
	for (const int index : selected)
	{
		if (index < 0 || index >= result.notes.size())
		{
			result.code = QStringLiteral("invalid_selection");
			return result;
		}
	}
	const auto original = result.notes;
	for (const int index : selected)
	{
		auto &note = result.notes[index];
		qint64 planned = note.lengthTicks;
		switch (options.mode)
		{
		case AiBridgeMidiValueMode::Set: planned = roundHalfAwayFromZero(options.value); break;
		case AiBridgeMidiValueMode::Offset: planned = note.lengthTicks + roundHalfAwayFromZero(options.value); break;
		case AiBridgeMidiValueMode::Scale: planned = roundHalfAwayFromZero(note.lengthTicks * options.value); break;
		case AiBridgeMidiValueMode::GateToNext:
		{
			qint64 nextOnset = -1;
			for (const auto &candidate : original)
			{
				if (candidate.positionTicks > note.positionTicks)
				{
					nextOnset = candidate.positionTicks;
					break;
				}
			}
			if (nextOnset < 0)
			{
				if (!options.fallbackLengthTicks) continue;
				planned = *options.fallbackLengthTicks;
			}
			else planned = roundHalfAwayFromZero((nextOnset - note.positionTicks) * options.gatePercent / 100.0);
			break;
		}
		default: break;
		}
		if (planned < 1)
		{
			if (options.boundaryPolicy == AiBridgeMidiBoundaryPolicy::Reject)
			{
				result.notes = original;
				result.code = QStringLiteral("boundary_rejected");
				return result;
			}
			planned = 1;
			++result.clampCount;
		}
		if (planned != note.lengthTicks)
		{
			note.lengthTicks = planned;
			++result.changedCount;
		}
	}
	result.success = true;
	if (result.changedCount == 0) result.code = QStringLiteral("no_effect");
	return result;
}

AiBridgeMidiCleanupPlan planCleanup(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices,
	const AiBridgeMidiCleanupOptions &options)
{
	AiBridgeMidiCleanupPlan result;
	result.notes = canonicalizeMidiNotes(snapshot.notes);
	if (options.minimumLengthTicks.has_value() != options.shortNoteAction.has_value()
		|| (options.minimumLengthTicks && *options.minimumLengthTicks < 1))
	{
		result.code = QStringLiteral("invalid_request");
		return result;
	}
	const auto original = result.notes;
	QVector<bool> selected(result.notes.size(), false);
	for (const int index : canonicalIndices)
	{
		if (index < 0 || index >= selected.size())
		{
			result.code = QStringLiteral("invalid_selection");
			return result;
		}
		selected[index] = true;
	}
	QVector<bool> removed(result.notes.size(), false);
	if (options.removeExactDuplicates)
	{
		for (int begin = 0; begin < result.notes.size(); )
		{
			int end = begin + 1;
			while (end < result.notes.size() && result.notes[begin].positionTicks == result.notes[end].positionTicks
				&& result.notes[begin].lengthTicks == result.notes[end].lengthTicks
				&& result.notes[begin].key == result.notes[end].key
				&& result.notes[begin].volume == result.notes[end].volume
				&& result.notes[begin].panning == result.notes[end].panning) ++end;
			bool hasUnselected = false;
			for (int index = begin; index < end; ++index) hasUnselected = hasUnselected || !selected[index];
			bool keptSelected = false;
			for (int index = begin; index < end; ++index)
			{
				if (!selected[index]) continue;
				if (hasUnselected || keptSelected) { removed[index] = true; ++result.removedCount; }
				else keptSelected = true;
			}
			begin = end;
		}
	}
	if (options.samePitchOverlapMode != AiBridgeMidiOverlapMode::None)
	{
		for (int index = 0; index < result.notes.size(); ++index)
		{
			if (removed[index]) continue;
			for (int later = index + 1; later < result.notes.size(); ++later)
			{
				if (removed[later]) continue;
				if (result.notes[later].key != result.notes[index].key || result.notes[later].positionTicks == result.notes[index].positionTicks) continue;
				if (result.notes[index].positionTicks + result.notes[index].lengthTicks <= result.notes[later].positionTicks) break;
				if (options.samePitchOverlapMode == AiBridgeMidiOverlapMode::Reject && (selected[index] || selected[later]))
				{
					result.notes = original;
					result.changedCount = 0;
					result.removedCount = 0;
					result.code = QStringLiteral("overlap_rejected");
					return result;
				}
				if (options.samePitchOverlapMode == AiBridgeMidiOverlapMode::TrimEarlier && selected[index])
				{
					result.notes[index].lengthTicks = result.notes[later].positionTicks - result.notes[index].positionTicks;
					++result.changedCount;
				}
				break;
			}
		}
	}
	if (options.minimumLengthTicks)
	{
		for (int index = 0; index < result.notes.size(); ++index)
		{
			if (removed[index] || !selected[index] || result.notes[index].lengthTicks >= *options.minimumLengthTicks) continue;
			if (*options.shortNoteAction == AiBridgeMidiShortNoteAction::Remove) { removed[index] = true; ++result.removedCount; }
			else { result.notes[index].lengthTicks = *options.minimumLengthTicks; ++result.changedCount; }
		}
	}
	QVector<AiBridgeMidiNoteSnapshot> retained;
	retained.reserve(result.notes.size() - result.removedCount);
	for (int index = 0; index < result.notes.size(); ++index) if (!removed[index]) retained.append(result.notes[index]);
	result.notes = canonicalizeMidiNotes(retained);
	result.changedCount += result.removedCount;
	result.success = true;
	if (result.changedCount == 0) result.code = QStringLiteral("no_effect");
	return result;
}

AiBridgeMidiHumanizePlan planScaleConstraint(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices, const AiBridgeMidiScaleConstraintOptions &options)
{
	AiBridgeMidiHumanizePlan result;
	result.notes = canonicalizeMidiNotes(snapshot.notes);
	QVector<bool> allowed(12, false);
	if (options.allowedPitchClasses.isEmpty()) { result.code = QStringLiteral("invalid_pitch_class"); return result; }
	for (const int pitchClass : options.allowedPitchClasses)
	{
		if (pitchClass < 0 || pitchClass > 11 || allowed[pitchClass]) { result.code = QStringLiteral("invalid_pitch_class"); return result; }
		allowed[pitchClass] = true;
	}
	for (const int index : canonicalIndices) if (index < 0 || index >= result.notes.size()) { result.code = QStringLiteral("invalid_selection"); return result; }
	for (const int index : canonicalIndices)
	{
		auto &note = result.notes[index];
		if (allowed[note.key % 12]) continue;
		int down = note.key - 1; while (down >= 0 && !allowed[down % 12]) --down;
		int up = note.key + 1; while (up <= 127 && !allowed[up % 12]) ++up;
		int target = note.key;
		if (options.direction == AiBridgeMidiScaleDirection::Down) { if (down >= 0) target = down; }
		else if (options.direction == AiBridgeMidiScaleDirection::Up) { if (up <= 127) target = up; }
		else if (down < 0) { if (up <= 127) target = up; }
		else if (up > 127) target = down;
		else if (note.key - down < up - note.key || (note.key - down == up - note.key && options.nearestTieBreak == AiBridgeMidiNearestTieBreak::Down)) target = down;
		else target = up;
		if (target != note.key) { note.key = target; ++result.changedCount; }
	}
	result.success = true;
	if (result.changedCount == 0) result.code = QStringLiteral("no_effect");
	return result;
}

} // namespace lmms
