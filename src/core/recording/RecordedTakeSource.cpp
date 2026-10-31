#include "RecordedTakeSource.h"
#include <QUuid>
#include <limits>
namespace lmms::recording {
namespace {
bool validUuid(const QString& value)
{
	const QUuid uuid(value);
	return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == value;
}

bool unsignedDecimal(const QString& text, std::uint64_t& value)
{
	if (text.isEmpty()) return false;
	for (const auto c : text) if (c < QChar('0') || c > QChar('9')) return false;
	bool ok = false;
	value = text.toULongLong(&ok);
	return ok && QString::number(value) == text;
}

bool digestText(const QString& value)
{
	if (value.size() != 64) return false;
	for (const auto c : value)
	{
		const bool isHex = (c >= QChar('0') && c <= QChar('9')) || (c >= QChar('a') && c <= QChar('f'));
		if (!isHex) return false;
	}
	return true;
}
}

std::optional<RecordedTakeReference> RecordedTakeSource::fromFacts(const RawTakeFacts& facts)
{
	RecordedTakeReference reference;
	reference.projectId = facts.projectId;
	reference.takeId = facts.takeId;
	reference.sampleRate = facts.sampleRate;
	reference.rawFrames = facts.rawFrames;
	reference.sourceIdentity = facts.sourceIdentity;
	reference.sourceDigest = facts.sourceDigest;
	if (!validate(reference).isEmpty()) return {};
	return reference;
}

QString RecordedTakeSource::validate(const RecordedTakeReference& reference)
{
	if (!validUuid(reference.projectId) || !validUuid(reference.takeId)) return QStringLiteral("invalid_identity");
	if (reference.sampleRate != 44100 && reference.sampleRate != 48000) return QStringLiteral("unsupported_rate");
	if (!reference.rawFrames || reference.rawFrames > std::uint64_t(reference.sampleRate) * 120)
		return QStringLiteral("source_limit");
	if (!reference.sourceIdentity.index || reference.sourceDigest.size() != 32) return QStringLiteral("source_identity_changed");
	const bool trackEmpty = reference.trackId.isEmpty();
	const bool clipEmpty = reference.clipId.isEmpty();
	if (trackEmpty != clipEmpty) return QStringLiteral("invalid_attachment_identity");
	if (trackEmpty)
	{
		if (reference.placementRevision) return QStringLiteral("invalid_placement_revision");
	}
	else if (!validUuid(reference.trackId) || !validUuid(reference.clipId) || !reference.placementRevision)
		return QStringLiteral("invalid_attachment_identity");
	return {};
}

bool RecordedTakeSource::matches(const RecordedTakeReference& reference, const RawTakeFacts& facts)
{
	return validate(reference).isEmpty() && reference.projectId == facts.projectId && reference.takeId == facts.takeId
		&& reference.sampleRate == facts.sampleRate && reference.rawFrames == facts.rawFrames
		&& reference.sourceIdentity == facts.sourceIdentity && reference.sourceDigest == facts.sourceDigest;
}

QDomElement RecordedTakeSource::writeReference(QDomDocument& document, const RecordedTakeReference& reference)
{
	if (!validate(reference).isEmpty()) return {};
	auto element = document.createElement(QStringLiteral("recorded_take"));
	element.setAttribute(QStringLiteral("schema"), QStringLiteral("1"));
	element.setAttribute(QStringLiteral("project_id"), reference.projectId);
	element.setAttribute(QStringLiteral("take_id"), reference.takeId);
	element.setAttribute(QStringLiteral("track_id"), reference.trackId);
	element.setAttribute(QStringLiteral("clip_id"), reference.clipId);
	element.setAttribute(QStringLiteral("sample_rate"), QString::number(reference.sampleRate));
	element.setAttribute(QStringLiteral("frames"), QString::number(reference.rawFrames));
	element.setAttribute(QStringLiteral("source_volume"), QString::number(reference.sourceIdentity.volume));
	element.setAttribute(QStringLiteral("source_index"), QString::number(reference.sourceIdentity.index));
	element.setAttribute(QStringLiteral("source_sha256"), QString::fromLatin1(reference.sourceDigest.toHex()));
	element.setAttribute(QStringLiteral("placement_revision"), QString::number(reference.placementRevision));
	return element;
}

std::optional<RecordedTakeReference> RecordedTakeSource::readReference(const QDomElement& element, QString& code)
{
	code = QStringLiteral("invalid_recorded_source");
	if (element.isNull() || element.tagName() != QStringLiteral("recorded_take") || element.hasChildNodes()
		|| element.attributes().count() != 11 || element.attribute(QStringLiteral("schema")) != QStringLiteral("1")) return {};
	for (int i = 0; i < element.attributes().count(); ++i)
	{
		const auto name = element.attributes().item(i).nodeName();
		if (name != QStringLiteral("schema") && name != QStringLiteral("project_id") && name != QStringLiteral("take_id")
			&& name != QStringLiteral("track_id") && name != QStringLiteral("clip_id")
			&& name != QStringLiteral("sample_rate") && name != QStringLiteral("frames")
			&& name != QStringLiteral("source_volume") && name != QStringLiteral("source_index")
			&& name != QStringLiteral("source_sha256") && name != QStringLiteral("placement_revision")) return {};
	}
	std::uint64_t rate = 0, frames = 0, volume = 0, index = 0, revision = 0;
	if (!unsignedDecimal(element.attribute(QStringLiteral("sample_rate")), rate) || rate > std::numeric_limits<std::uint32_t>::max()
		|| !unsignedDecimal(element.attribute(QStringLiteral("frames")), frames)
		|| !unsignedDecimal(element.attribute(QStringLiteral("source_volume")), volume) || volume > std::numeric_limits<std::uint32_t>::max()
		|| !unsignedDecimal(element.attribute(QStringLiteral("source_index")), index)
		|| !unsignedDecimal(element.attribute(QStringLiteral("placement_revision")), revision)
		|| !digestText(element.attribute(QStringLiteral("source_sha256")))) return {};
	RecordedTakeReference reference;
	reference.projectId = element.attribute(QStringLiteral("project_id"));
	reference.takeId = element.attribute(QStringLiteral("take_id"));
	reference.trackId = element.attribute(QStringLiteral("track_id"));
	reference.clipId = element.attribute(QStringLiteral("clip_id"));
	reference.sampleRate = std::uint32_t(rate);
	reference.rawFrames = frames;
	reference.sourceIdentity = {std::uint32_t(volume), index};
	reference.sourceDigest = QByteArray::fromHex(element.attribute(QStringLiteral("source_sha256")).toLatin1());
	reference.placementRevision = revision;
	code = validate(reference);
	if (!code.isEmpty()) return {};
	code = QStringLiteral("ok");
	return reference;
}
}
