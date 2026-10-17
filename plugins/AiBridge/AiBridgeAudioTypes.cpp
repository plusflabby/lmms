#include "AiBridgeAudioTypes.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <algorithm>
#include <cmath>
namespace lmms {
namespace {
QJsonObject rangeJson(const AudioSourceWindowV1& w)
{
	return {{"in",QString::number(w.inFrame)},{"out",QString::number(w.outFrame)},
		{"origin",QString::number(w.phaseOriginFrame)},{"source_frames",QString::number(w.sourceFrameCount)},
		{"source_rate",w.sourceRate},{"reference_bpm",w.referenceTempoBpm},{"reference_output_rate",w.referenceOutputRate},
		{"ticks_per_beat",w.ticksPerBeat},{"frames_per_tick",w.framesPerTick},{"length_ticks",w.lengthTicks},{"offset_ticks",w.offsetTicks}};
}
}
QString audioStateHash(std::vector<AiBridgeAudioClipState> clips)
{
	std::sort(clips.begin(),clips.end(),[](const auto& a,const auto& b){return a.id<b.id;});
	QJsonArray rows; QString previous;
	for(const auto& s:clips)
	{
		if(s.id.isEmpty() || s.id==previous || !std::isfinite(s.trackVolume) || !std::isfinite(s.trackPanning)
			|| !std::isfinite(s.playbackFrequency) || !std::isfinite(s.amplification)) return {};
		previous=s.id;
		QJsonObject row{{"id",s.id},{"track_id",s.trackId},{"source_status",s.sourceStatus},
			{"source_reference_hash",QString::fromLatin1(s.sourceReferenceHash.toHex())},
			{"source_identity",QString::fromLatin1(s.sourceIdentity.toHex())},{"source_digest",QString::fromLatin1(s.sourceDigest.toHex())},
			{"position",QString::number(s.position)},{"length",QString::number(s.length)},{"offset",QString::number(s.offset)},
			{"muted",s.muted},{"solo",s.solo},{"native_name",s.nativeName},{"reversed",s.reversed},{"auto_resize",s.autoResize},{"color",s.color},
			{"source_frames",QString::number(s.sourceFrames)},{"sample_rate",s.sampleRate},
			{"source_channels",s.sourceChannels ? QJsonValue(*s.sourceChannels) : QJsonValue(QJsonValue::Null)},
			{"track_volume",s.trackVolume},{"track_panning",s.trackPanning},{"fx_channel",s.fxChannel},
			{"track_muted",s.trackMuted},{"playback_frequency",s.playbackFrequency},{"amplification",s.amplification}};
		const int rangeKinds=int(s.rangeWindow.has_value())+int(s.rangeWindowV2.has_value())+int(!s.invalidRangeMetadataDigest.isEmpty());
		if(rangeKinds>1 || (rangeKinds && s.rangeStatus.isEmpty())) return {};
		if(s.rangeWindow)
		{
			if(validateAudioWindowRecord(*s.rangeWindow)!="ok") return {};
			auto range=rangeJson(*s.rangeWindow);range.insert("version",1);range.insert("phase","linear_equal_v1");row.insert("range_v1",range);
		}
		if(s.rangeWindowV2)
		{
			if(validateAudioWindowRecord(*s.rangeWindowV2)!="ok") return {};
			auto range=rangeJson(s.rangeWindowV2->base);range.insert("version",2);range.insert("phase","linear_equal_ref256_v2");
			range.insert("reference_end",QString::number(s.rangeWindowV2->referenceEndFrame));row.insert("range_v2",range);
		}
		if(!s.invalidRangeMetadataDigest.isEmpty())
		{
			if(s.invalidRangeMetadataDigest.size()!=32) return {};
			row.insert("invalid_range_digest",QString::fromLatin1(s.invalidRangeMetadataDigest.toHex()));
		}
		if(rangeKinds) row.insert("range_status",s.rangeStatus);
		rows.append(row);
	}
	return QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("aibridge-audio-state-v1:")
		+QJsonDocument(rows).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
}
