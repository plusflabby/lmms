#ifndef LMMS_AIBRIDGE_H
#define LMMS_AIBRIDGE_H

#include "ToolPlugin.h"
#include "AiBridgeIpcServer.h"
#include "AiBridgeVocalJobs.h"
#include "AiBridgeMidiExportJobs.h"

namespace lmms
{

class AiBridge : public ToolPlugin
{
public:
	AiBridge();
	~AiBridge() override;
	gui::PluginView *instantiateView(QWidget *parent) override;
	QString nodeName() const override;
	void saveSettings(QDomDocument &, QDomElement &) override {}
	void loadSettings(const QDomElement &) override {}
	AiBridgeVocalJobs& vocalJobs() { return m_vocalJobs; }

private:
	AiBridgeVocalJobs m_vocalJobs;
	AiBridgeMidiExportJobs m_midiExportJobs;
	AiBridgeIpcServer m_ipcServer;
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_H
