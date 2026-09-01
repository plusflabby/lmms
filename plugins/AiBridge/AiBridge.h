#ifndef LMMS_AIBRIDGE_H
#define LMMS_AIBRIDGE_H

#include "ToolPlugin.h"

namespace lmms
{

class AiBridge : public ToolPlugin
{
public:
	AiBridge();
	gui::PluginView *instantiateView(QWidget *parent) override;
	QString nodeName() const override;
	void saveSettings(QDomDocument &, QDomElement &) override {}
	void loadSettings(const QDomElement &) override {}
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_H
