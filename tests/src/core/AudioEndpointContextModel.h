// Proposal oracle only. Not an installed native codec or production renderer.
#ifndef LMMS_TEST_AUDIO_ENDPOINT_CONTEXT_MODEL_H
#define LMMS_TEST_AUDIO_ENDPOINT_CONTEXT_MODEL_H
#include "AudioSourceWindow.h"
#include "Sample.h"
#include <QRegularExpression>

namespace lmms::test {
struct EndpointContextProposal
{
	AudioSourceWindowV1 base;
	qint64 referenceEndFrame=0;
};
inline QString validateProposal(const EndpointContextProposal& p)
{
	if(validateAudioWindowRecord(p.base)!="ok" || p.referenceEndFrame<p.base.outFrame
		|| p.referenceEndFrame>p.base.sourceFrameCount
		|| (p.referenceEndFrame-p.base.phaseOriginFrame)%p.base.framesPerTick) return "invalid_audio_window";
	return "ok";
}
inline QString proposalEligibility(const EndpointContextProposal& p,const QString& sourceCode="ok")
{
	if(validateProposal(p)!="ok") return "invalid_audio_window";
	if(sourceCode!="ok") return sourceCode;
	// The versioned policy pins the measured 256-frame reference input chunks.
	if((p.referenceEndFrame-p.base.phaseOriginFrame)%256==1) return "unrepresentable_audio_boundary";
	return "ok";
}
inline QDomElement writeProposal(const EndpointContextProposal& p)
{
	QDomDocument doc; auto root=doc.createElement("sampleclip");doc.appendChild(root);
	AudioWindowStorage old;old.kind=AudioWindowStorageKind::V1;old.value=p.base;writeAudioWindow(old,doc,root);
	auto node=root.firstChildElement("audio_source_window");node.setAttribute("version","2");
	node.setAttribute("phase","linear_equal_ref256_v2");node.setAttribute("reference_end",QString::number(p.referenceEndFrame));
	return root;
}
inline std::optional<EndpointContextProposal> readProposal(const QDomElement& root)
{
	auto node=root.firstChildElement("audio_source_window");
	if(node.isNull() || !node.nextSiblingElement("audio_source_window").isNull()
		|| node.attributes().size()!=14 || node.attribute("version")!="2"
		|| node.attribute("phase")!="linear_equal_ref256_v2") return {};
	const auto token=node.attribute("reference_end");
	static const QRegularExpression canonical("\\A(?:0|[1-9][0-9]*)\\z");
	if(!canonical.match(token).hasMatch()) return {};
	bool ok=false;const auto end=token.toLongLong(&ok);if(!ok) return {};
	QDomDocument doc;auto adapted=doc.createElement("sampleclip");doc.appendChild(adapted);
	auto copy=doc.importNode(node,true).toElement();copy.removeAttribute("reference_end");copy.setAttribute("version","1");copy.setAttribute("phase","linear_equal_v1");adapted.appendChild(copy);
	auto old=parseAudioWindow(adapted);if(old.kind!=AudioWindowStorageKind::V1) return {};
	EndpointContextProposal p{*old.value,end};if(validateProposal(p)!="ok")return {};return p;
}
inline EndpointContextProposal subwindow(EndpointContextProposal p,qint64 first,qint64 last)
{
	p.base.inFrame=first;p.base.outFrame=last;p.base.offsetTicks=qint32(-first/p.base.framesPerTick);
	p.base.lengthTicks=qint32((last-first)/p.base.framesPerTick);return p;
}
inline std::optional<SampleFrame> proposalSample(const EndpointContextProposal& p,const Sample& source,qint64 relative)
{
	if(proposalEligibility(p)!="ok" || source.sampleSize()!=size_t(p.base.sourceFrameCount)
		|| source.sampleRate()!=p.base.sourceRate || relative<0 || relative>=p.base.outFrame-p.base.inFrame) return {};
	const auto index=std::max(p.base.phaseOriginFrame,p.base.inFrame+relative-1);
	return source.data()[index];
}
}
#endif
