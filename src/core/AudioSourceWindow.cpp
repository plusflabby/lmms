#include "AudioSourceWindow.h"
#include <QRegularExpression>
#include <QStringList>
#include <climits>

namespace lmms {
namespace {
const QString Tag=QStringLiteral("audio_source_window");
const QString Invalid=QStringLiteral("invalid_audio_window");
const QString Unsupported=QStringLiteral("unsupported_audio_window_version");
bool integer(const QDomElement& node,const QString& name,qint64& result)
{
	static const QRegularExpression canonical(QStringLiteral("\\A(?:0|-?[1-9][0-9]*)\\z"));
	const auto text=node.attribute(name);
	if(!canonical.match(text).hasMatch()) return false;
	bool ok=false; result=text.toLongLong(&ok); return ok;
}
bool integer(const QDomElement& node,const QString& name,qint32& result)
{
	qint64 wide=0;
	if(!integer(node,name,wide) || wide<INT_MIN || wide>INT_MAX) return false;
	result=qint32(wide); return true;
}
}

QString validateAudioWindowRecord(const AudioSourceWindowV1& w)
{
	if(w.sourceRate!=44100 && w.sourceRate!=48000) return Invalid;
	const qint64 numerator=qint64(w.sourceRate)*60, denominator=48*125;
	if(w.referenceTempoBpm!=125 || w.referenceOutputRate!=w.sourceRate || w.ticksPerBeat!=48
		|| numerator%denominator || w.framesPerTick!=numerator/denominator || w.lengthTicks<=0) return Invalid;
	if(w.phaseOriginFrame<0 || w.phaseOriginFrame>w.inFrame || w.inFrame>=w.outFrame
		|| w.outFrame>w.sourceFrameCount || w.sourceFrameCount>INT_MAX) return Invalid;
	if(w.inFrame%w.framesPerTick || w.phaseOriginFrame%w.framesPerTick
		|| w.offsetTicks!=-(w.inFrame/w.framesPerTick)
		|| w.outFrame-w.inFrame!=qint64(w.lengthTicks)*w.framesPerTick) return Invalid;
	return QStringLiteral("ok");
}

QString validateAudioWindowRecord(const AudioSourceWindowV2& w)
{
	if(validateAudioWindowRecord(w.base)!="ok" || w.referenceEndFrame<w.base.outFrame
		|| w.referenceEndFrame>w.base.sourceFrameCount
		|| (w.referenceEndFrame-w.base.phaseOriginFrame)%w.base.framesPerTick) return Invalid;
	return QStringLiteral("ok");
}

AudioWindowStorage parseAudioWindow(const QDomElement& clip)
{
	AudioWindowStorage result;
	QDomElement node;
	int count=0;
	for(auto child=clip.firstChildElement(Tag);!child.isNull();child=child.nextSiblingElement(Tag))
	{
		if(count++==0)
		{
			result.retainedInvalidXml.appendChild(result.retainedInvalidXml.createElement("retained_audio_window_nodes"));
			node=child;
		}
		result.retainedInvalidXml.documentElement().appendChild(result.retainedInvalidXml.importNode(child,true));
	}
	if(!count) return result;
	result.kind=AudioWindowStorageKind::InvalidMetadata; result.parseCode=Invalid;
	if(count!=1) return result;
	const bool v2=node.attribute("version")=="2" && node.attribute("phase")=="linear_equal_ref256_v2";
	const bool v1=node.attribute("version")=="1" && node.attribute("phase")=="linear_equal_v1";
	if(!v1 && !v2) { result.parseCode=Unsupported; return result; }
	QStringList names{"version","phase","in","out","origin","source_frames","source_rate","reference_bpm",
		"reference_output_rate","ticks_per_beat","frames_per_tick","length_ticks","offset_ticks"};
	if(v2) names.append("reference_end");
	if(node.attributes().size()!=names.size()) return result;
	for(const auto& name:names) if(!node.hasAttribute(name)) return result;
	for(auto child=node.firstChild();!child.isNull();child=child.nextSibling())
		if(child.isElement() || (!child.isComment() && !child.nodeValue().trimmed().isEmpty())) return result;
	AudioSourceWindowV1 w;
	if(!integer(node,"in",w.inFrame) || !integer(node,"out",w.outFrame) || !integer(node,"origin",w.phaseOriginFrame)
		|| !integer(node,"source_frames",w.sourceFrameCount) || !integer(node,"source_rate",w.sourceRate)
		|| !integer(node,"reference_bpm",w.referenceTempoBpm) || !integer(node,"reference_output_rate",w.referenceOutputRate)
		|| !integer(node,"ticks_per_beat",w.ticksPerBeat) || !integer(node,"frames_per_tick",w.framesPerTick)
		|| !integer(node,"length_ticks",w.lengthTicks) || !integer(node,"offset_ticks",w.offsetTicks)) return result;
	if(validateAudioWindowRecord(w)!="ok") return result;
	if(v2)
	{
		AudioSourceWindowV2 record{w,0};
		if(!integer(node,"reference_end",record.referenceEndFrame) || validateAudioWindowRecord(record)!="ok") return result;
		result.kind=AudioWindowStorageKind::V2; result.valueV2=record;
	}
	else { result.kind=AudioWindowStorageKind::V1; result.value=w; }
	result.parseCode="ok";
	result.retainedInvalidXml=QDomDocument{}; return result;
}

void writeAudioWindow(const AudioWindowStorage& storage,QDomDocument& doc,QDomElement& clip)
{
	for(auto child=clip.firstChildElement(Tag);!child.isNull();)
	{ const auto next=child.nextSiblingElement(Tag); clip.removeChild(child); child=next; }
	if(storage.kind==AudioWindowStorageKind::Legacy) return;
	if(storage.kind==AudioWindowStorageKind::InvalidMetadata)
	{
		for(auto child=storage.retainedInvalidXml.documentElement().firstChild();!child.isNull();child=child.nextSibling())
			clip.appendChild(doc.importNode(child,true));
		return;
	}
	const bool v2=storage.kind==AudioWindowStorageKind::V2;
	const auto& w=v2 ? storage.valueV2.value().base : storage.value.value();
	auto node=doc.createElement(Tag); node.setAttribute("version",v2?"2":"1");
	node.setAttribute("phase",v2?"linear_equal_ref256_v2":"linear_equal_v1");
	if(v2) node.setAttribute("reference_end",QString::number(storage.valueV2->referenceEndFrame));
	node.setAttribute("in",QString::number(w.inFrame)); node.setAttribute("out",QString::number(w.outFrame));
	node.setAttribute("origin",QString::number(w.phaseOriginFrame)); node.setAttribute("source_frames",QString::number(w.sourceFrameCount));
	node.setAttribute("source_rate",QString::number(w.sourceRate)); node.setAttribute("reference_bpm",QString::number(w.referenceTempoBpm));
	node.setAttribute("reference_output_rate",QString::number(w.referenceOutputRate)); node.setAttribute("ticks_per_beat",QString::number(w.ticksPerBeat));
	node.setAttribute("frames_per_tick",QString::number(w.framesPerTick)); node.setAttribute("length_ticks",QString::number(w.lengthTicks));
	node.setAttribute("offset_ticks",QString::number(w.offsetTicks)); clip.appendChild(node);
}
}
