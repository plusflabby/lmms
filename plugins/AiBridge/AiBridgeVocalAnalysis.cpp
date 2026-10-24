#include "AiBridgeVocalAnalysis.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace lmms {
VocalAnalysisAccumulator::VocalAnalysisAccumulator(const VocalAnalysisSettings& settings,const VocalSelectionMetadata& metadata)
	:m_meta(metadata),m_settings(settings),m_windowStart(metadata.frames.inFrame),m_end(metadata.frames.inFrame)
{
	if(metadata.frames.inFrame<0 || metadata.frames.outFrame<=metadata.frames.inFrame || metadata.sampleRate<=0
		|| (metadata.channels!=1 && metadata.channels!=2) || !std::isfinite(settings.lowLevelDbfs)
		|| settings.lowLevelDbfs< -96 || settings.lowLevelDbfs> -20
		|| settings.minimumLowLevelMs<50 || settings.minimumLowLevelMs>2000)
	{m_error="invalid_parameter";return;}
	const auto frames=quint64(metadata.frames.outFrame-metadata.frames.inFrame);
	if(frames>quint64(metadata.sampleRate)*VocalLimits::seconds){m_error="resource_limit";return;}
	m_meta.windowFrames=std::max(1,metadata.sampleRate/20);m_meta.settings=settings;
	const auto windows=frames/quint64(m_meta.windowFrames)+(frames%quint64(m_meta.windowFrames)!=0);
	m_capacity=windows+(windows/2+windows%2);
	if(m_capacity>VocalLimits::selectionRecords){m_error="resource_limit";return;}
	m_threshold=std::pow(10.0,settings.lowLevelDbfs/20.0);
	m_result.records.reserve(size_t(m_capacity));m_spans.reserve(size_t(windows/2+windows%2));
	m_result.summaries.reserve(1);
}
VocalChannelMetrics VocalAnalysisAccumulator::metrics(const Sums& s) const
{
	VocalChannelMetrics m;
	if(!s.count)return m;
	m.count=s.count;m.fullScaleCandidates=s.candidates;m.overrange=s.overrange;m.peak=s.peak;
	m.rms=std::sqrt(s.squares/double(s.count));m.dc=s.sum==0 ? 0 : s.sum/double(s.count);
	m.silence=m.peak==0;
	if(m.peak>0)m.peakDbfs=20*std::log10(m.peak);
	if(m.rms>0)m.rmsDbfs=20*std::log10(m.rms);
	return m;
}
void VocalAnalysisAccumulator::closeLowRun(qint64 end)
{
	if(!m_lowStart)return;
	if(quint64(end-*m_lowStart)*1000>=quint64(m_settings.minimumLowLevelMs)*quint64(m_meta.sampleRate))
	{
		if(m_result.records.size()+m_spans.size()>=m_capacity){m_error="resource_limit";return;}
		VocalAnalysisRecord span;span.kind=VocalAnalysisRecord::Kind::LowLevelSpan;span.frames={*m_lowStart,end};
		m_spans.push_back(span);
	}
	m_lowStart.reset();
}
void VocalAnalysisAccumulator::flushWindow()
{
	if(!m_window[0].count)return;
	if(m_result.records.size()+m_spans.size()>=m_capacity){m_error="resource_limit";return;}
	VocalAnalysisRecord record;record.frames={m_windowStart,m_end};bool low=true;
	for(int c=0;c<m_meta.channels;++c)
	{
		record.channels[c]=metrics(m_window[c]);low=low && record.channels[c].rms<=m_threshold;
	}
	m_result.records.push_back(record);
	if(low){if(!m_lowStart)m_lowStart=m_windowStart;}else closeLowRun(m_windowStart);
	m_window={};m_windowStart=m_end;
}
PcmWaveVisitDecision VocalAnalysisAccumulator::consume(quint64 first,std::span<const SampleFrame> samples)
{
	if(m_finished || !m_error.isEmpty())return PcmWaveVisitDecision::ResourceLimit;
	if(first>quint64(std::numeric_limits<qint64>::max()) || samples.size()>quint64(std::numeric_limits<qint64>::max())-first
		|| (m_next && first!=*m_next) || (!m_next && first>quint64(m_meta.frames.inFrame)))
	{m_error="incomplete_audio_stream";return PcmWaveVisitDecision::ResourceLimit;}
	m_next=first+samples.size();
	const auto start=std::max(first,quint64(m_meta.frames.inFrame));
	const auto end=std::min(first+samples.size(),quint64(m_meta.frames.outFrame));
	for(auto frame=start;frame<end;++frame)
	{
		for(int c=0;c<m_meta.channels;++c)
		{
			const double value=samples[size_t(frame-first)][c];
			if(!std::isfinite(value)){m_error="invalid_audio_samples";return PcmWaveVisitDecision::ResourceLimit;}
			for(auto* s:{&m_total[c],&m_window[c]})
			{
				++s->count;s->sum+=value;s->squares+=value*value;s->peak=std::max(s->peak,std::abs(value));
				if(value== -1 || value==32767/32768.0)++s->candidates;
				if(std::abs(value)>1)++s->overrange;
			}
		}
		++m_selected;m_end=qint64(frame+1);
		if(m_window[0].count==quint64(m_meta.windowFrames))flushWindow();
		if(!m_error.isEmpty())return PcmWaveVisitDecision::ResourceLimit;
	}
	return PcmWaveVisitDecision::Continue;
}
VocalAnalysisResult VocalAnalysisAccumulator::finish()
{
	if(m_finished)return {"analysis_already_finished",{},{},0};
	m_finished=true;
	if(!m_error.isEmpty())return {m_error,{},{},0};
	if(m_selected!=quint64(m_meta.frames.outFrame-m_meta.frames.inFrame))return {"incomplete_audio_stream",{},{},0};
	flushWindow();closeLowRun(m_meta.frames.outFrame);
	if(!m_error.isEmpty())return {m_error,{},{},0};
	VocalSelectionSummary summary;summary.provenance=m_meta;
	for(int c=0;c<m_meta.channels;++c)
	{
		summary.channels[c]=metrics(m_total[c]);
		if(!std::isfinite(summary.channels[c].rms) || !std::isfinite(summary.channels[c].dc))return {"invalid_audio_samples",{},{},0};
	}
	m_result.summaries.push_back(std::move(summary));
	m_result.records.insert(m_result.records.end(),m_spans.begin(),m_spans.end());
	m_result.accountedBytes=quint64(m_result.records.capacity())*sizeof(VocalAnalysisRecord)
		+quint64(m_result.summaries.capacity())*sizeof(VocalSelectionSummary);
	m_result.code="ok";return std::move(m_result);
}
}
