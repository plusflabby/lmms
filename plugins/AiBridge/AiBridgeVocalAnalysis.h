#ifndef LMMS_AI_BRIDGE_VOCAL_ANALYSIS_H
#define LMMS_AI_BRIDGE_VOCAL_ANALYSIS_H
#include "AiBridgeVocalTypes.h"
namespace lmms {
class VocalAnalysisAccumulator
{
public:
	VocalAnalysisAccumulator(const VocalAnalysisSettings&,const VocalSelectionMetadata&);
	PcmWaveVisitDecision consume(quint64,std::span<const SampleFrame>);
	VocalAnalysisResult finish();
private:
	struct Sums {quint64 count=0,candidates=0,overrange=0;double sum=0,squares=0,peak=0;};
	VocalSelectionMetadata m_meta;
	VocalAnalysisSettings m_settings;
	VocalAnalysisResult m_result;
	std::array<Sums,2> m_total,m_window;
	std::vector<VocalAnalysisRecord> m_spans;
	std::optional<quint64> m_next;
	std::optional<qint64> m_lowStart;
	quint64 m_selected=0,m_capacity=0;
	qint64 m_windowStart=0,m_end=0;
	double m_threshold=0;
	QString m_error;
	bool m_finished=false;
	VocalChannelMetrics metrics(const Sums&) const;
	void flushWindow();
	void closeLowRun(qint64);
};
}
#endif
