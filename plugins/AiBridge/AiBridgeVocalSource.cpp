#include "AiBridgeVocalSource.h"
#include "AudioFileAuthorization.h"
#include "SampleBuffer.h"
#include <QCoreApplication>
#include <QThread>
#include <cmath>
namespace lmms {
VocalScanResult scanVocalSource(const VocalSourceTicket& ticket,const PcmWaveChunkVisitor& visitor,const std::atomic_bool& cancelled)
{
	if(QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread())return {"wrong_thread",{}};
	if(cancelled.load())return {"cancelled",{}};
	const bool recordedSource = bool(ticket.authorizedSourceBuffer) || !ticket.recordedTakeProjectId.isEmpty()
		|| !ticket.recordedTakeId.isEmpty();
	if(recordedSource)
	{
		const auto& expected=ticket.expectedInfo;
		if(!ticket.authorizedSourceBuffer)return {"invalid_parameter",{}};
		if(ticket.authorizedSourceBufferRevision!=ticket.authorizedSourceBuffer->contentRevision())
			return {"audio_source_changed",{}};
		if(ticket.recordedTakeProjectId.isEmpty() || ticket.recordedTakeId.isEmpty()
			|| !ticket.roots.isEmpty() || ticket.expectedIdentity.isEmpty() || expected.sourceSha256.size()!=32
			|| (expected.sampleRate!=44100 && expected.sampleRate!=48000) || expected.channels!=1 || !expected.frames
			|| expected.frames>quint64(expected.sampleRate)*VocalLimits::seconds
			|| ticket.authorizedSourceBuffer->sampleRate()!=quint32(expected.sampleRate)
			|| ticket.authorizedSourceBuffer->size()!=size_t(expected.frames)
			|| ticket.reference!=QStringLiteral("recorded_take:")+ticket.recordedTakeProjectId+QLatin1Char('/')+ticket.recordedTakeId
			|| !visitor)
			return {"invalid_parameter",{}};
		constexpr size_t chunkFrames=4096;
		const auto* data=ticket.authorizedSourceBuffer->data();
		for(quint64 first=0;first<expected.frames;)
		{
			if(cancelled.load())return {"cancelled",{}};
			const auto count=size_t(std::min<quint64>(chunkFrames,expected.frames-first));
			for(size_t i=0;i<count;++i)
				if(!std::isfinite(data[size_t(first)+i].left()) || !std::isfinite(data[size_t(first)+i].right()))
					return {"audio_decode_failed",{}};
			const auto decision=visitor(first,std::span<const SampleFrame>(data+size_t(first),count));
			if(decision!=PcmWaveVisitDecision::Continue)
				return {decision==PcmWaveVisitDecision::Cancelled ? QStringLiteral("cancelled") : QStringLiteral("resource_limit"),{}};
			if(ticket.authorizedSourceBufferRevision!=ticket.authorizedSourceBuffer->contentRevision())
				return {"audio_source_changed",{}};
			first+=count;
		}
		if(cancelled.load())return {"cancelled",{}};
		// The sealed recording capability verified the WAV digest and identity before
		// this immutable decode entered the project. The worker only reads that decode.
		return {"ok",expected};
	}
	if(ticket.roots.isEmpty())return {"import_root_not_configured",{}};
	if(ticket.roots.size()>VocalLimits::rootCount || ticket.reference.size()>VocalLimits::pathChars
		|| ticket.expectedIdentity.isEmpty() || ticket.expectedInfo.sourceSha256.size()!=32)return {"import_path_denied",{}};
	try
	{
		QString failure="import_path_denied";
		for(const auto& path:ticket.roots)
		{
			if(cancelled.load())return {"cancelled",{}};
			if(path.size()>VocalLimits::pathChars)return {"import_path_denied",{}};
			auto approved=AudioFileAuthorization::approveRoot(path);if(!approved.root)continue;
			auto acquired=AudioFileAuthorization::acquire(*approved.root,ticket.reference);
			if(!acquired.source){if(acquired.code=="audio_source_missing")failure=acquired.code;continue;}
			if(acquired.identity!=ticket.expectedIdentity)return {"audio_source_changed",{}};
			auto result=SampleDecoder::scanPcmWave(*acquired.source,ticket.limits,visitor,cancelled);
			if(result.code!="ok")return {result.code,{}};
			const auto& actual=result.info;const auto& expected=ticket.expectedInfo;
			if(actual.sourceSha256!=expected.sourceSha256 || actual.frames!=expected.frames || actual.fileBytes!=expected.fileBytes
				|| actual.channels!=expected.channels || actual.sampleRate!=expected.sampleRate)return {"audio_source_changed",{}};
			return {"ok",result.info};
		}
		return {failure,{}};
	}
	catch(const std::bad_alloc&){return {"resource_limit",{}};}
	catch(...){return {"audio_decode_failed",{}};}
}
}
