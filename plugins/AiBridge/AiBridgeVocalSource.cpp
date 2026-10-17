#include "AiBridgeVocalSource.h"
#include "AudioFileAuthorization.h"
#include <QCoreApplication>
#include <QThread>
namespace lmms {
VocalScanResult scanVocalSource(const VocalSourceTicket& ticket,const PcmWaveChunkVisitor& visitor,const std::atomic_bool& cancelled)
{
	if(QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread())return {"wrong_thread",{}};
	if(cancelled.load())return {"cancelled",{}};
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
