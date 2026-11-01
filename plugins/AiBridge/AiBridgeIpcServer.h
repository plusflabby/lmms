#ifndef LMMS_AIBRIDGE_IPC_SERVER_H
#define LMMS_AIBRIDGE_IPC_SERVER_H

#include <QByteArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <memory>

#include "AiBridgeProtocol.h"

namespace lmms
{
class AiBridgeVocalJobs;
class AiBridgeRecordingActions;
namespace recording { class RecordingSessionManager; }
class AiBridgeMidiExportJobs;

class AiBridgeIpcServer : public QObject
{
public:
	explicit AiBridgeIpcServer(QObject *parent = nullptr, QString endpoint = QStringLiteral("lmms-aibridge-v2"),
		AiBridgeVocalJobs* jobs = nullptr, AiBridgeMidiExportJobs* midiExportJobs = nullptr,
		recording::RecordingSessionManager* recordingManager = nullptr);
	~AiBridgeIpcServer() override;

	bool start();
	void stop();
	bool isListening() const { return m_server.isListening(); }
	QString endpointName() const { return m_endpoint; }

private:
	void acceptConnection();
	void readProbe();
	void clientDisconnected();
	void executeRequest(const AiBridgeProtocolRequest &request);
	void sendResponse(const AiBridgeProtocolResponse &response);

	QLocalServer m_server;
	QPointer<QLocalSocket> m_client;
	QByteArray m_input;
	bool m_busy = false;
	QString m_endpoint;
	QPointer<AiBridgeVocalJobs> m_vocalJobs;
	QPointer<AiBridgeMidiExportJobs> m_midiExportJobs;
	// The action adapter borrows the Engine-owned manager; disconnect never owns or cancels a take.
	std::unique_ptr<AiBridgeRecordingActions> m_recordingActions;
	quint64 m_connectionRevision = 0;
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_IPC_SERVER_H
