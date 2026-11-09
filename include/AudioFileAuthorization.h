#ifndef LMMS_AUDIO_FILE_AUTHORIZATION_H
#define LMMS_AUDIO_FILE_AUTHORIZATION_H
#include <QIODevice>
#include <QString>
#include <memory>
#include <atomic>
#include "lmms_export.h"
namespace lmms {
// Private native acquisition result. Paths and OS diagnostics never enter IPC.
class LMMS_EXPORT AudioFileAuthorization
{
public:
	class Root
	{
	public:
		void revoke() { m_revoked = true; }
	private:
		friend class AudioFileAuthorization;
		QString m_path;
		QByteArray m_identity;
		std::unique_ptr<QIODevice> m_pin;
		std::atomic<bool> m_revoked{false};
	};
	struct Approval
	{
		QString code;
		std::unique_ptr<Root> root;
	};
	struct Result
	{
		QString code;
		QByteArray identity;
		std::unique_ptr<QIODevice> source;
	};
	struct PinnedResult
	{
		QString code;
		QByteArray identity;
		std::unique_ptr<QIODevice> source;
		std::shared_ptr<void> pin;
	};
	static Result acquire(const QString& approvedRoot, const QString& path);
	// The independent native pin may be released on a worker after the source
	// device has been destroyed on its owning thread. Directory handles permit
	// sibling creation but deny directory replacement; the final file still
	// denies writes and replacement.
	static PinnedResult acquireWithRetainedPin(const QString& approvedRoot, const QString& path);
	static Approval approveRoot(const QString& approvedRoot);
	static Result acquire(const Root& approvedRoot, const QString& path);
};
}
#endif
