#ifndef LMMS_PROJECT_SAVE_PUBLICATION_H
#define LMMS_PROJECT_SAVE_PUBLICATION_H

#include <QByteArray>
#include <QString>
#include <memory>
#include "lmms_export.h"

namespace lmms {
// Internal publication primitive. It is not an authenticated journal or a
// complete save transaction and must not be connected to project saving alone.
// All operations and destruction must run on the acquiring thread.
class LMMS_EXPORT ProjectSavePublication
{
public:
	struct Result { QString code; std::unique_ptr<ProjectSavePublication> publication; };
	struct Evidence { QString target, transactionId; QByteArray originalIdentity, originalSha256, stageIdentity, stageSha256; QString stageNativeFileId; bool originalMissing = false; };
	static Result acquire(const QString& target, const QByteArray& expectedIdentity,
		const QByteArray& expectedSha256);
	// For a target explicitly chosen by the native save UI. No MCP path input.
	static Result acquireSelectedTarget(const QString& target);
	static Result acquireNewTarget(const QString& target);
	static Result reopenForRecovery(const QString& target, const QString& transactionId,
		const QByteArray& originalIdentity, const QByteArray& originalSha256,
		const QByteArray& stageIdentity, const QByteArray& stageSha256);
	// Restores only an authenticated interrupted original. Journal finalization
	// remains separate; success does not make the project ready for a new save.
	static QString restoreOriginalFromNativeJournal(const QString& target);
	// Reconciles an authenticated journal and clears its witnesses only after
	// retaining and verifying the relevant project files.
	static QString resolveNativeJournal(const QString& target);
	static QString nativeJournalStatus(const QString& target);
	~ProjectSavePublication();
	QString createStage(const QString& transactionId, const QByteArray& bytes);
	// Prepares the independent project-save journal. Not a completed save.
	QString prepareNativeJournal();
	QString publishNativePending();
	QString finalizeNativePublication();
	// Commits only a fully witnessed native journal while retaining its pins.
	QString commitNativeSave();
	QString moveOriginalToBackup(const QString& transactionId);
	QString publishStage();
	QString restoreOriginal();
	bool evidence(Evidence& output) const;
	bool stagedEvidence(Evidence& output) const;
	// Valid only while the witnessed, published handles are retained.
	bool publishedEvidence(Evidence& output) const;
private:
	struct State;
	static std::unique_ptr<State> openParent(const QString& target);
	static Result acquireImpl(const QString& target, const QByteArray& expectedIdentity,
		const QByteArray& expectedSha256, bool requireExpected);
	bool nativeWitnessValid() const;
	explicit ProjectSavePublication(std::unique_ptr<State> state);
	std::unique_ptr<State> m_state;
};
}
#endif
