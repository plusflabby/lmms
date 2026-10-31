#include "AiBridgeVocalTypes.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QSet>
#include <cmath>
#include <limits>
#include <numeric>
namespace lmms {
namespace {
bool bounded(const QString& text, int maximum, bool empty=false)
{
	return (empty || !text.isEmpty()) && text.size()<=maximum && text.toUtf8().size()<=maximum;
}
bool digest(const QString& text)
{
	if(text.size()!=64) return false;
	for(const auto ch:text) if(!((ch>='0' && ch<='9') || (ch>='a' && ch<='f'))) return false;
	return true;
}
bool add(quint64& value, quint64 extra)
{
	if(extra>std::numeric_limits<quint64>::max()-value) return false;
	value+=extra; return true;
}
bool multiply(quint64 a, quint64 b, quint64& result)
{
	if(b && a>std::numeric_limits<quint64>::max()/b) return false;
	result=a*b;return true;
}
// Checked exact rational seconds; no floating tolerance admits a frame past 120s.
bool duration(quint64& numerator,quint64& denominator,quint64 frames,quint64 rate)
{
	const auto g=std::gcd(denominator,rate);
	quint64 left=0,right=0,nextDenominator=0;
	if(!multiply(numerator,rate/g,left) || !multiply(frames,denominator/g,right)
		|| !multiply(denominator,rate/g,nextDenominator) || !add(left,right))return false;
	const auto reduce=std::gcd(left,nextDenominator);
	numerator=left/reduce;denominator=nextDenominator/reduce;
	return numerator/denominator<VocalLimits::seconds
		|| (numerator/denominator==VocalLimits::seconds && numerator%denominator==0);
}
void configure(QDataStream& stream)
{
	stream.setVersion(QDataStream::Qt_5_15);stream.setByteOrder(QDataStream::BigEndian);
	stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
}
double canonical(double x){return x==0 ? 0 : x;}
QString finishHash(const QByteArray& data)
{
	return QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex());
}
void window(QDataStream& out,const AudioSourceWindowV2& w)
{
	const auto& b=w.base;
	out<<b.inFrame<<b.outFrame<<b.phaseOriginFrame<<b.sourceFrameCount<<b.sourceRate<<b.referenceTempoBpm
		<<b.referenceOutputRate<<b.ticksPerBeat<<b.framesPerTick<<b.lengthTicks<<b.offsetTicks<<w.referenceEndFrame;
}
}
std::optional<qint64> parseVocalFrame(const QString& text)
{
	if(text.isEmpty() || text.size()>19 || (text.size()>1 && text[0]=='0'))return {};
	quint64 result=0;
	for(const auto ch:text)
	{
		if(ch<'0' || ch>'9')return {};
		const auto digit=quint64(ch.unicode()-'0');
		if(result>(quint64(std::numeric_limits<qint64>::max())-digit)/10)return {};
		result=result*10+digit;
	}
	return qint64(result);
}
QString validateVocalRequest(const VocalAnalyzeRequest& request)
{
	if(request.selections.empty())return "invalid_parameter";
	if(request.selections.size()>VocalLimits::selections)return "resource_limit";
	if(!std::isfinite(request.settings.lowLevelDbfs) || request.settings.lowLevelDbfs< -96
		|| request.settings.lowLevelDbfs> -20 || request.settings.minimumLowLevelMs<50
		|| request.settings.minimumLowLevelMs>2000)return "invalid_parameter";
	QSet<QString> ids;
	for(const auto& selection:request.selections)
	{
		if(!bounded(selection.clipId,VocalLimits::idBytes) || !digest(selection.expectedAudioHash)
			|| selection.frames.inFrame<0 || selection.frames.outFrame<=selection.frames.inFrame
			|| ids.contains(selection.clipId))return "invalid_parameter";
		ids.insert(selection.clipId);
	}
	return "ok";
}
VocalBudget estimateVocalBudget(const VocalInputSnapshot& s)
{
	VocalBudget result;result.code=validateVocalRequest(s.request);
	if(result.code!="ok")return result;
	auto fail=[&](const char* code){return VocalBudget{0,0,0,code};};
	if(!s.generation || s.sources.size()!=s.request.selections.size())return fail("invalid_parameter");
	// Fixed bookkeeping allowance includes job/thread/queued completion descriptors.
	quint64 bytes=65536,numerator=0,denominator=1;
	for(size_t i=0;i<s.sources.size();++i)
	{
		const auto& t=s.sources[i];const auto& sel=s.request.selections[i];const auto& info=t.expectedInfo;
		if(t.clipId!=sel.clipId || info.sampleRate<=0 || (info.channels!=1 && info.channels!=2)
			|| quint64(sel.frames.outFrame)>info.frames || info.frames>quint64(std::numeric_limits<int>::max())
			|| t.expectedIdentity.isEmpty() || t.expectedIdentity.size()>256 || info.sourceSha256.size()!=32
			|| t.policyRevision!=s.policyRevision)return fail("invalid_parameter");
		if(!t.limits.maxFileBytes || !t.limits.maxDecodedBytes)return fail("audio_resource_limits_not_configured");
		if(info.fileBytes>t.limits.maxFileBytes || info.decodedBytes>t.limits.maxDecodedBytes)return fail("source_too_large");
		if(t.roots.isEmpty())return fail("import_root_not_configured");
		if(t.roots.size()>VocalLimits::rootCount || t.reference.isEmpty() || t.reference.size()>VocalLimits::pathChars
			|| !bounded(t.sourceRevision,VocalLimits::idBytes) || !bounded(t.sourceStatus,64))return fail("resource_limit");
		quint64 rootChars=0;
		for(const auto& root:t.roots)
		{
			if(root.isEmpty() || root.size()>VocalLimits::pathChars || !add(rootChars,quint64(root.size()))
				|| rootChars>VocalLimits::rootChars)return fail("resource_limit");
		}
		const auto frames=quint64(sel.frames.outFrame-sel.frames.inFrame);
		if(!duration(numerator,denominator,frames,quint64(info.sampleRate)))return fail("resource_limit");
		const quint64 width=std::max(1,info.sampleRate/20);
		const quint64 windows=frames/width+(frames%width!=0);
		const quint64 records=windows+(windows/2+windows%2);
		if(records>VocalLimits::selectionRecords || !add(result.recordCapacity,records)
			|| result.recordCapacity>VocalLimits::jobRecords)return fail("resource_limit");
		// Reserve copied strings/root tickets, metadata and full vector capacities before any worker allocation.
		quint64 chars=rootChars+quint64(t.reference.size())+quint64(t.geometry.nativeName.size())+quint64(t.geometry.color.size());
		if(t.geometry.nativeName.size()>4096 || t.geometry.color.size()>64)return fail("resource_limit");
		if(!add(bytes,chars*2+8192+sizeof(VocalSourceTicket)+sizeof(VocalSelectionSummary)))return fail("resource_limit");
	}
	quint64 recordBytes=0;
	if(!multiply(result.recordCapacity,sizeof(VocalAnalysisRecord),recordBytes) || !add(bytes,recordBytes)
		|| bytes>VocalLimits::sessionBytes)return fail("resource_limit");
	result.sessionBytes=bytes;
	// Allow registry + worker + queued-result copies, fixed input/output conversion scratch and hash context.
	if(!multiply(bytes,3,result.jobBytes) || !add(result.jobBytes,65536)
		|| result.jobBytes>VocalLimits::jobBytes)return fail("resource_limit");
	result.code="ok";return result;
}
QString vocalInputHash(const VocalInputSnapshot& s)
{
	if(estimateVocalBudget(s).code!="ok")return {};
	QByteArray data;QDataStream out(&data,QIODevice::WriteOnly);configure(out);
	out<<QString("aibridge-vocal-input-v1")<<s.generation<<s.policyRevision
		<<canonical(s.request.settings.lowLevelDbfs)<<s.request.settings.minimumLowLevelMs<<quint32(s.sources.size());
	for(size_t i=0;i<s.sources.size();++i)
	{
		const auto& sel=s.request.selections[i];const auto& t=s.sources[i];const auto& info=t.expectedInfo;
		if(!std::isfinite(t.geometry.frequency) || !std::isfinite(t.geometry.amplification))return {};
		out<<sel.clipId<<sel.expectedAudioHash<<sel.frames.inFrame<<sel.frames.outFrame<<t.clipId
			<<t.reference<<t.roots<<t.policyRevision<<t.limits.maxFileBytes<<t.limits.maxDecodedBytes
			<<info.frames<<info.fileBytes<<info.decodedBytes<<info.channels<<info.sampleRate<<info.sourceSha256
			<<t.expectedIdentity<<t.sourceRevision<<t.sourceStatus<<bool(t.window);
		if(t.window)window(out,*t.window);
		out<<t.world.tempoBpm<<t.world.outputRate<<t.world.ticksPerBeat<<t.world.tempoAutomationPresent
			<<t.geometry.positionTicks<<t.geometry.lengthTicks<<t.geometry.offsetTicks
			<<t.geometry.autoResize<<t.geometry.reversed<<t.geometry.solo<<t.geometry.nativeName<<t.geometry.color
			<<canonical(t.geometry.frequency)<<canonical(t.geometry.amplification);
	}
	return finishHash(data);
}
QString vocalTrackHash(const VocalTrackSnapshot& s)
{
	if(!s.generation || !bounded(s.trackId,VocalLimits::idBytes) || s.name.size()>4096
		|| !bounded(s.mixerId,VocalLimits::idBytes,true) || !bounded(s.membershipRevision,256)
		|| !std::isfinite(s.volume) || !std::isfinite(s.panning) || s.volume<0 || s.volume>200
		|| s.panning< -100 || s.panning>100)return {};
	QByteArray data;QDataStream out(&data,QIODevice::WriteOnly);configure(out);
	out<<QString("aibridge-vocal-track-v1")<<s.trackId<<s.generation<<s.name<<s.mixerId<<s.membershipRevision
		<<canonical(s.volume)<<canonical(s.panning)<<s.muted<<s.solo;
	return finishHash(data);
}
}
