#include "AiBridgeAudioTypes.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <algorithm>
#include <cmath>
namespace lmms {
QString audioStateHash(std::vector<AiBridgeAudioClipState> clips)
{
	std::sort(clips.begin(),clips.end(),[](const auto& a,const auto& b){return a.id<b.id;});
	QJsonArray rows; QString previous;
	for(const auto& s:clips)
	{
		if(s.id.isEmpty() || s.id==previous || !std::isfinite(s.trackVolume) || !std::isfinite(s.trackPanning)
			|| !std::isfinite(s.playbackFrequency) || !std::isfinite(s.amplification)) return {};
		previous=s.id;
		rows.append(QJsonObject{{"id",s.id},{"track_id",s.trackId},{"source_status",s.sourceStatus},
			{"source_reference_hash",QString::fromLatin1(s.sourceReferenceHash.toHex())},
			{"source_identity",QString::fromLatin1(s.sourceIdentity.toHex())},{"source_digest",QString::fromLatin1(s.sourceDigest.toHex())},
			{"position",QString::number(s.position)},{"length",QString::number(s.length)},{"offset",QString::number(s.offset)},
			{"muted",s.muted},{"solo",s.solo},{"native_name",s.nativeName},{"reversed",s.reversed},{"auto_resize",s.autoResize},{"color",s.color},
			{"source_frames",QString::number(s.sourceFrames)},{"sample_rate",s.sampleRate},
			{"source_channels",s.sourceChannels ? QJsonValue(*s.sourceChannels) : QJsonValue(QJsonValue::Null)},
			{"track_volume",s.trackVolume},{"track_panning",s.trackPanning},{"fx_channel",s.fxChannel},
			{"track_muted",s.trackMuted},{"playback_frequency",s.playbackFrequency},{"amplification",s.amplification}});
	}
	return QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("aibridge-audio-state-v1:")
		+QJsonDocument(rows).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
}
