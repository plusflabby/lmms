#include "AudioWindowPlayback.h"
#include "AudioEngine.h"
#include <QCoreApplication>
#include <QThread>
#include <QCryptographicHash>
#include <QFile>
#include <samplerate.h>
#include <algorithm>
#include <array>
#include <cstring>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace lmms {
bool audioWindowReferenceProfileMatches(qint32 capacity,const QByteArray& digest)
{
	// Initial qualified Windows runtime only. Other builds require independent
	// qualification, not automatic approval because they reuse a version string.
	return capacity==256 && digest==QByteArray::fromHex("4f7f9b2ecea84bb8f5b27c8f36e85e8a100663c745ac25e82a512f485b663ac7");
}
namespace {
bool qualifiedLibraryImage()
{
#ifdef Q_OS_WIN
	// MinGW may expose an executable import thunk for &src_get_version.
	// Its returned immutable version literal belongs to the actual loaded image.
	HMODULE module=nullptr;
	if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(src_get_version()),&module)) return false;
	std::array<wchar_t,32768> path{};
	const auto length=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
	if(!length || length>=path.size()) return false;
	QFile file(QString::fromWCharArray(path.data(),int(length)));
	if(!file.open(QIODevice::ReadOnly) || file.size()<=0 || file.size()>8*1024*1024) return false;
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if(!hash.addData(&file)) return false;
	return audioWindowReferenceProfileMatches(DEFAULT_BUFFER_SIZE,hash.result());
#else
	return false;
#endif
}
bool qualifyReference()
{
	// The profile is more than a version string. Bounded, initialized conformance
	// input verifies phase and consumption under the qualified binary profile.
	// This runs once on the control thread, never as part of rendering.
	if(DEFAULT_BUFFER_SIZE!=256 || std::strncmp(src_get_version(),"libsamplerate-0.2.2 ",19)!=0
		|| !qualifiedLibraryImage()) return false;
	int error=0;
	std::unique_ptr<SRC_STATE,decltype(&src_delete)> state(src_new(SRC_LINEAR,2,&error),src_delete);
	if(!state || error) return false;
	std::array<float,516> input{};std::array<float,512> output{};
	for(int i=0;i<256;++i) {input[2+i*2]=float(i%97-48)/128;input[3+i*2]=float(i%89-44)/128;}
	SRC_DATA data{};data.data_in=input.data()+2;data.input_frames=256;data.data_out=output.data();
	data.output_frames=256;data.src_ratio=1;
	if(src_process(state.get(),&data) || data.input_frames_used!=256 || data.output_frames_gen!=256) return false;
	for(int i=0;i<256;++i)for(int ch=0;ch<2;++ch)
		if(output[i*2+ch]!=input[2+std::max(0,i-1)*2+ch]) return false;
	const auto historyLeft=input[512],historyRight=input[513];
	input[0]=0.875f;input[1]=-0.875f;input[2]=0.5f;input[3]=-0.5f;input[4]=0.25f;input[5]=-0.25f;
	data=SRC_DATA{};data.data_in=input.data()+2;data.input_frames=2;data.data_out=output.data();data.output_frames=2;data.src_ratio=1;
	if(src_process(state.get(),&data) || data.input_frames_used!=2 || data.output_frames_gen!=2
		|| output[0]!=historyLeft || output[1]!=historyRight || output[2]!=0.5f || output[3]!=-0.5f) return false;
	// Never exercise the known singleton issue in production qualification.
	// Only historical bounded investigation tests characterize that path.
	return true;
}
bool validSpan(const AudioSourceWindowV2& window,const SampleBuffer& buffer)
{
	const auto& w=window.base;
	return w.phaseOriginFrame>=0 && w.phaseOriginFrame<=w.inFrame && w.inFrame<w.outFrame
		&& w.outFrame<=window.referenceEndFrame && window.referenceEndFrame<=w.sourceFrameCount
		&& w.sourceFrameCount<=INT_MAX && size_t(w.sourceFrameCount)==buffer.size()
		&& (w.sourceRate==44100 || w.sourceRate==48000)
		&& static_cast<sample_rate_t>(w.sourceRate)==buffer.sampleRate()
		&& DEFAULT_BUFFER_SIZE==256 && (window.referenceEndFrame-w.phaseOriginFrame)%DEFAULT_BUFFER_SIZE!=1;
}
}
bool verifyAudioWindowReferenceProfile()
{
	if(!QCoreApplication::instance() || QThread::currentThread()!=QCoreApplication::instance()->thread()) return false;
	static const bool qualified=qualifyReference();
	return qualified;
}
std::optional<AudioWindowPlaybackView> makeAudioWindowPlaybackView(std::shared_ptr<const SampleBuffer> buffer,
	const AudioSourceWindowV2& window,qint64 elapsed,double amplification,
	std::shared_ptr<const AudioWindowPlaybackGuard> guard,quint64 epoch)
{
	if(!verifyAudioWindowReferenceProfile() || !buffer || !guard || !epoch || amplification!=1
		|| guard->epoch.load(std::memory_order_acquire)!=epoch || validateAudioWindowRecord(window)!="ok"
		|| !validSpan(window,*buffer) || elapsed<0 || elapsed>window.base.outFrame-window.base.inFrame) return {};
	return AudioWindowPlaybackView{std::move(buffer),window,elapsed,window.base.outFrame-window.base.inFrame-elapsed,
		amplification,std::move(guard),epoch};
}
void renderAudioWindow(const AudioWindowPlaybackView& view,qint64 rendered,std::span<SampleFrame> destination)
{
	std::fill(destination.begin(),destination.end(),SampleFrame{});
	if(!view.buffer || !view.guard || !view.expectedEpoch || view.amplification!=1 || rendered<0
		|| !validSpan(view.window,*view.buffer) || view.firstRelativeOutputFrame<0 || view.outputFrameCount<0
		|| view.firstRelativeOutputFrame>view.window.base.outFrame-view.window.base.inFrame
		|| view.outputFrameCount>view.window.base.outFrame-view.window.base.inFrame-view.firstRelativeOutputFrame
		|| rendered>=view.outputFrameCount || view.guard->epoch.load(std::memory_order_acquire)!=view.expectedEpoch) return;
	const auto count=std::min<quint64>(destination.size(),quint64(view.outputFrameCount-rendered));
	const auto first=view.window.base.inFrame+view.firstRelativeOutputFrame+rendered;
	for(quint64 i=0;i<count;++i)
	{
		const auto index=std::max(view.window.base.phaseOriginFrame,first+qint64(i)-1);
		// Bounds follow from the validated view above; retain a check at every read.
		if(index<view.window.base.phaseOriginFrame || index>=view.window.referenceEndFrame
			|| quint64(index)>=view.buffer->size()) return;
		destination[i]=view.buffer->data()[index]*float(view.amplification);
	}
}
}
