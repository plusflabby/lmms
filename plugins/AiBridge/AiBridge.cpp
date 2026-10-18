#include "AiBridge.h"

#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "AiBridgeActions.h"
#include "embed.h"
#include "ToolPluginView.h"
#include "plugin_export.h"

namespace lmms
{

extern "C"
{
Plugin::Descriptor PLUGIN_EXPORT aibridge_plugin_descriptor =
{
	"aibridge", "AI Bridge", QT_TRANSLATE_NOOP("AiBridge", "AI control bridge"),
	"LMMS Plus", 0x0100, Plugin::Type::Tool, new PluginPixmapLoader("logo"), nullptr, nullptr
};

PLUGIN_EXPORT Plugin *lmms_plugin_main(Model *)
{
	return new AiBridge;
}
}

class AiBridgeView : public gui::ToolPluginView
{
public:
	AiBridgeView(ToolPlugin *tool) : gui::ToolPluginView(tool)
	{
		auto *layout = new QVBoxLayout(this);
		layout->addWidget(new QLabel(tr("Phase 1 LMMS AI Bridge"), this));

		auto *tempo = new QPushButton(tr("Set Tempo"), this);
		connect(tempo, &QPushButton::clicked, this, [] {
			AiBridgeContext context;
			AiBridgeActions::setSongTempo(context, 82);
		});
		layout->addWidget(tempo);

		auto *volume = new QPushButton(tr("Adjust Volume"), this);
		connect(volume, &QPushButton::clicked, this, [] {
			AiBridgeContext context;
			AiBridgeActions::adjustCurrentInstrumentVolume(context, -10.0f);
		});
		layout->addWidget(volume);

		auto *note = new QPushButton(tr("Add Test Note"), this);
		connect(note, &QPushButton::clicked, this, [] {
			AiBridgeContext context;
			AiBridgeActions::insertNoteIntoCurrentMidiClip(context, 60, 0, 48, 100);
		});
		layout->addWidget(note);
		layout->addStretch();
	}
};

AiBridge::AiBridge() : ToolPlugin(&aibridge_plugin_descriptor, nullptr), m_vocalJobs(this), m_ipcServer(this,QStringLiteral("lmms-aibridge-v2"),&m_vocalJobs)
{
	m_ipcServer.start();
}

AiBridge::~AiBridge()
{
	m_ipcServer.stop();
	m_vocalJobs.shutdown();
}

gui::PluginView *AiBridge::instantiateView(QWidget *parent)
{
	auto *view = new AiBridgeView(this);
	Q_UNUSED(parent);
	return view;
}

QString AiBridge::nodeName() const
{
	return aibridge_plugin_descriptor.name;
}

} // namespace lmms
