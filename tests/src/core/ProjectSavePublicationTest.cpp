#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QUuid>
#include <QProcess>
#include <QProcessEnvironment>
#include <QElapsedTimer>
#include <QThread>
#include "ProjectSavePublication.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif
using namespace lmms;
class ProjectSavePublicationTest : public QObject
{
	Q_OBJECT
	static QByteArray contents(const QString& path)
	{
		QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
	}
#ifdef Q_OS_WIN
	struct TemporaryCredential
	{
		QString name;
		bool owned = false;
		~TemporaryCredential() { if (owned) CredDeleteW(reinterpret_cast<LPCWSTR>(name.utf16()), CRED_TYPE_GENERIC, 0); }
	};
	static QString journalCredentialName(const QString& directory)
	{
		const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(directory.utf16()), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return {};
		BY_HANDLE_FILE_INFORMATION info{}; const bool ok = GetFileInformationByHandle(handle, &info); CloseHandle(handle);
		if (!ok) return {};
		const auto material = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
			+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
		const auto id = QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex();
		return QStringLiteral("LMMS/NativeProjectSave/v1/") + QString::fromLatin1(id);
	}
	static QByteArray fileIdentity(const QString& path)
	{
		const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return {};
		BY_HANDLE_FILE_INFORMATION info{}; const bool ok = GetFileInformationByHandle(handle, &info); CloseHandle(handle);
		if (!ok) return {};
		const auto material = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
			+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
		return QCryptographicHash::hash(material, QCryptographicHash::Sha256);
	}
#endif
private slots:
	void publishesFromPinnedHandlesWithoutReplacingBackup()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid());
		const auto target = dir.filePath(QStringLiteral("project.mmp"));
		const auto oldBackup = target + QStringLiteral(".bak");
		for (const auto& pair : {qMakePair(target, QByteArray("original")), qMakePair(oldBackup, QByteArray("old-backup"))})
		{
			QFile f(pair.first); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly));
			QCOMPARE(f.write(pair.second), qint64(pair.second.size()));
		}
		const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(target.utf16()), GENERIC_READ,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		QVERIFY(handle != INVALID_HANDLE_VALUE);
		BY_HANDLE_FILE_INFORMATION info{}; QVERIFY(GetFileInformationByHandle(handle, &info)); CloseHandle(handle);
		const auto material = QByteArray::number(qulonglong(info.dwVolumeSerialNumber)) + ':'
			+ QByteArray::number(qulonglong(info.nFileIndexHigh)) + ':' + QByteArray::number(qulonglong(info.nFileIndexLow));
		const auto identity = QCryptographicHash::hash(material, QCryptographicHash::Sha256);
		const auto digest = QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256);
		auto acquired = ProjectSavePublication::acquire(target, identity, digest);
		QCOMPARE(acquired.code, QStringLiteral("ok")); QVERIFY(acquired.publication);
		// A concurrent legacy writer deletes .bak before attempting to move the
		// locked project. The retained publication must protect that backup too.
		QVERIFY(!QFile::remove(oldBackup));
		QCOMPARE(contents(oldBackup), QByteArray("old-backup"));
		const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(acquired.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
		ProjectSavePublication::Evidence evidence;
		QVERIFY(acquired.publication->evidence(evidence));
		QCOMPARE(evidence.target, target); QCOMPARE(evidence.transactionId, tx);
		QCOMPARE(evidence.originalIdentity, identity); QCOMPARE(evidence.originalSha256, digest);
		QCOMPARE(evidence.stageSha256, QCryptographicHash::hash(QByteArray("edited"), QCryptographicHash::Sha256));
		QCOMPARE(evidence.stageIdentity.size(), 32); QVERIFY(evidence.stageIdentity != identity);
		QVERIFY(!QFile::rename(target, dir.filePath(QStringLiteral("competing"))));
		QCOMPARE(acquired.publication->moveOriginalToBackup(tx), QStringLiteral("ok"));
		QVERIFY(!QFileInfo::exists(target));
		QCOMPARE(acquired.publication->publishStage(), QStringLiteral("ok"));
		// The publication handles intentionally deny new opens that do not
		// share their DELETE access. Inspect bytes after releasing those pins.
		acquired.publication.reset();
		QCOMPARE(contents(target), QByteArray("edited"));
		QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-backup-") + tx)), QByteArray("original"));
		QCOMPARE(contents(oldBackup), QByteArray("old-backup"));
#endif
	}
	void failedPublicationRestoresOriginalOnlyAfterConflictCleared()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid());
		const auto target = dir.filePath(QStringLiteral("project.mmp"));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
		const auto identity = fileIdentity(target); QCOMPARE(identity.size(), 32);
		const auto digest = QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256);
		auto held = ProjectSavePublication::acquire(target, identity, digest); QVERIFY(held.publication);
		const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
		QCOMPARE(held.publication->moveOriginalToBackup(tx), QStringLiteral("ok"));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("foreign"), qint64(7)); }
		QVERIFY(held.publication->publishStage() != QStringLiteral("ok"));
		QVERIFY(held.publication->restoreOriginal() != QStringLiteral("ok"));
		QCOMPARE(contents(target), QByteArray("foreign"));
		QVERIFY(QFile::remove(target)); // This exact path was created above by this test.
		QCOMPARE(held.publication->restoreOriginal(), QStringLiteral("ok"));
		held.publication.reset();
		QCOMPARE(contents(target), QByteArray("original"));
		QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-stage-") + tx)), QByteArray("edited"));
#endif
	}
	void rejectsBackupCollisionWithoutMovingOriginal()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
		const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
		auto held = ProjectSavePublication::acquire(target, id, QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256));
		QVERIFY(held.publication); const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
		const auto collision = dir.filePath(QStringLiteral(".lmms-vocal-backup-") + tx);
		{ QFile f(collision); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("foreign"), qint64(7)); }
		QVERIFY(held.publication->moveOriginalToBackup(tx) != QStringLiteral("ok"));
		held.publication.reset();
		QCOMPARE(contents(target), QByteArray("original"));
		QCOMPARE(contents(collision), QByteArray("foreign"));
		QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-stage-") + tx)), QByteArray("edited"));
#endif
	}
	void independentJournalUsesPinnedDirectoryAndUserBoundWitness()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
		TemporaryCredential credential; credential.name = journalCredentialName(dir.path()); QVERIFY(!credential.name.isEmpty());
		PCREDENTIALW prior = nullptr;
		const bool existed = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &prior);
		const auto readError = existed ? DWORD(ERROR_SUCCESS) : GetLastError();
		if (prior) CredFree(prior);
		QVERIFY(!existed); QCOMPARE(readError, DWORD(ERROR_NOT_FOUND)); credential.owned = true;
		QCOMPARE(ProjectSavePublication::nativeJournalStatus(target), QStringLiteral("none"));
		const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
		auto held = ProjectSavePublication::acquire(target, id, QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256));
		QVERIFY(held.publication); const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
		const auto marker = dir.filePath(QStringLiteral(".lmms-native-save-journal-v1"));
		{ QFile f(marker); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("foreign"), qint64(7)); }
		QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("project_recovery_required"));
		QCOMPARE(contents(marker), QByteArray("foreign")); QVERIFY(QFile::remove(marker)); // Test-owned collision.
		QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("ok"));
		QCOMPARE(ProjectSavePublication::nativeJournalStatus(target), QStringLiteral("pending"));
		PCREDENTIALW witness = nullptr;
		QVERIFY(CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &witness));
		QVERIFY(witness); CredFree(witness);
		QVERIFY(held.publication->prepareNativeJournal() != QStringLiteral("ok"));
		held.publication.reset();
		const auto bytes = contents(marker); QVERIFY(!bytes.isEmpty());
		QVERIFY(!bytes.contains(target.toUtf8())); QVERIFY(!bytes.contains("original"));
		QCOMPARE(contents(target), QByteArray("original"));
#endif
	}
	void independentJournalRestoresInterruptedOriginal()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
		TemporaryCredential credential; credential.name = journalCredentialName(dir.path()); QVERIFY(!credential.name.isEmpty());
		PCREDENTIALW prior = nullptr;
		const bool existed = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &prior);
		const auto error = existed ? DWORD(ERROR_SUCCESS) : GetLastError(); if (prior) CredFree(prior);
		QVERIFY(!existed); QCOMPARE(error, DWORD(ERROR_NOT_FOUND)); credential.owned = true;
		const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
		auto held = ProjectSavePublication::acquire(target, id, QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256));
		QVERIFY(held.publication); const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
		QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("ok"));
		QCOMPARE(held.publication->moveOriginalToBackup(tx), QStringLiteral("ok")); held.publication.reset();
		QVERIFY(!QFileInfo::exists(target));
		QCOMPARE(ProjectSavePublication::restoreOriginalFromNativeJournal(target), QStringLiteral("ok"));
		QCOMPARE(contents(target), QByteArray("original"));
		QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-stage-") + tx)), QByteArray("edited"));
		QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral(".lmms-native-save-journal-v1"))));
#endif
	}
	void independentJournalRejectsTamperingOrMissingWitness()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		for (const bool removeWitness : {false, true})
		{
			QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
			{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
			TemporaryCredential credential; credential.name = journalCredentialName(dir.path()); QVERIFY(!credential.name.isEmpty());
			PCREDENTIALW prior = nullptr;
			const bool existed = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &prior);
			const auto error = existed ? DWORD(ERROR_SUCCESS) : GetLastError(); if (prior) CredFree(prior);
			QVERIFY(!existed); QCOMPARE(error, DWORD(ERROR_NOT_FOUND)); credential.owned = true;
			const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
			auto held = ProjectSavePublication::acquire(target, id, QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256));
			QVERIFY(held.publication); const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
			QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
			QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("ok"));
			QCOMPARE(held.publication->moveOriginalToBackup(tx), QStringLiteral("ok")); held.publication.reset();
			const auto marker = dir.filePath(QStringLiteral(".lmms-native-save-journal-v1"));
			if (removeWitness)
				QVERIFY(CredDeleteW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0));
			else
			{
				QFile f(marker); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
				QCOMPARE(f.write("tampered"), qint64(8));
			}
			QVERIFY(ProjectSavePublication::restoreOriginalFromNativeJournal(target) != QStringLiteral("ok"));
			QVERIFY(!QFileInfo::exists(target));
			QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-backup-") + tx)), QByteArray("original"));
			QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-stage-") + tx)), QByteArray("edited"));
		}
#endif
	}
	void publishedNativeJournalFinalizesWithBothVersionsPreserved()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
		TemporaryCredential credential; credential.name = journalCredentialName(dir.path()); QVERIFY(!credential.name.isEmpty());
		PCREDENTIALW prior = nullptr;
		const bool existed = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &prior);
		const auto error = existed ? DWORD(ERROR_SUCCESS) : GetLastError(); if (prior) CredFree(prior);
		QVERIFY(!existed); QCOMPARE(error, DWORD(ERROR_NOT_FOUND)); credential.owned = true;
		const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
		auto held = ProjectSavePublication::acquire(target, id, QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256));
		QVERIFY(held.publication); const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
		QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("ok"));
		QCOMPARE(held.publication->moveOriginalToBackup(tx), QStringLiteral("ok"));
		QCOMPARE(held.publication->publishStage(), QStringLiteral("ok")); held.publication.reset();
		QCOMPARE(ProjectSavePublication::resolveNativeJournal(target), QStringLiteral("committed"));
		QCOMPARE(contents(target), QByteArray("edited"));
		QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-backup-") + tx)), QByteArray("original"));
		QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral(".lmms-native-save-journal-v1"))));
		PCREDENTIALW pending = nullptr;
		const bool remains = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &pending);
		const auto readError = remains ? DWORD(ERROR_SUCCESS) : GetLastError(); if (pending) CredFree(pending);
		QVERIFY(!remains); QCOMPARE(readError, DWORD(ERROR_NOT_FOUND));
#endif
	}
	void nativeCommitPublishesAndFinalizesWhileFilesRemainPinned()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
		for (const auto& pair : {qMakePair(target, QByteArray("original")), qMakePair(target + ".bak", QByteArray("old-backup"))})
		{ QFile f(pair.first); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write(pair.second), qint64(pair.second.size())); }
		TemporaryCredential credential; credential.name = journalCredentialName(dir.path()); QVERIFY(!credential.name.isEmpty());
		PCREDENTIALW prior = nullptr;
		const bool existed = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &prior);
		const auto error = existed ? DWORD(ERROR_SUCCESS) : GetLastError(); if (prior) CredFree(prior);
		QVERIFY(!existed); QCOMPARE(error, DWORD(ERROR_NOT_FOUND)); credential.owned = true;
		const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
		auto held = ProjectSavePublication::acquire(target, id, QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256));
		QVERIFY(held.publication); const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
		QVERIFY(held.publication->commitNativeSave() != QStringLiteral("ok"));
		QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("ok"));
		QCOMPARE(held.publication->commitNativeSave(), QStringLiteral("ok")); held.publication.reset();
		QCOMPARE(contents(target), QByteArray("edited")); QCOMPARE(contents(target + ".bak"), QByteArray("old-backup"));
		QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-backup-") + tx)), QByteArray("original"));
		QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral(".lmms-native-save-journal-v1"))));
		PCREDENTIALW pending = nullptr;
		const bool remains = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &pending);
		const auto readError = remains ? DWORD(ERROR_SUCCESS) : GetLastError(); if (pending) CredFree(pending);
		QVERIFY(!remains); QCOMPARE(readError, DWORD(ERROR_NOT_FOUND));
#endif
	}
	void publicationRetainsJournalUntilExplicitFinalization()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
		TemporaryCredential credential; credential.name = journalCredentialName(dir.path()); QVERIFY(!credential.name.isEmpty());
		PCREDENTIALW prior = nullptr;
		const bool existed = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &prior);
		const auto error = existed ? DWORD(ERROR_SUCCESS) : GetLastError(); if (prior) CredFree(prior);
		QVERIFY(!existed); QCOMPARE(error, DWORD(ERROR_NOT_FOUND)); credential.owned = true;
		const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
		auto held = ProjectSavePublication::acquire(target, id, QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256));
		QVERIFY(held.publication); const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
		ProjectSavePublication::Evidence expected; QVERIFY(held.publication->evidence(expected));
		QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("ok"));
		QVERIFY(held.publication->finalizeNativePublication() != QStringLiteral("ok"));
		QCOMPARE(held.publication->publishNativePending(), QStringLiteral("ok"));
		ProjectSavePublication::Evidence published; QVERIFY(held.publication->publishedEvidence(published));
		QCOMPARE(published.stageIdentity, expected.stageIdentity); QCOMPARE(published.stageSha256, expected.stageSha256);
		QCOMPARE(ProjectSavePublication::nativeJournalStatus(target), QStringLiteral("pending"));
		QCOMPARE(held.publication->finalizeNativePublication(), QStringLiteral("ok"));
		ProjectSavePublication::Evidence finalized; QVERIFY(held.publication->publishedEvidence(finalized));
		QCOMPARE(finalized.stageIdentity, published.stageIdentity); QCOMPARE(finalized.stageSha256, published.stageSha256);
		held.publication.reset();
		QCOMPARE(ProjectSavePublication::nativeJournalStatus(target), QStringLiteral("none"));
		QCOMPARE(contents(target), QByteArray("edited"));
		QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-backup-") + tx)), QByteArray("original"));
#endif
	}
	void credentialOnlyFinalizationRejectsForeignTarget()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
		TemporaryCredential credential; credential.name = journalCredentialName(dir.path()); QVERIFY(!credential.name.isEmpty());
		PCREDENTIALW prior = nullptr;
		const bool existed = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &prior);
		const auto error = existed ? DWORD(ERROR_SUCCESS) : GetLastError(); if (prior) CredFree(prior);
		QVERIFY(!existed); QCOMPARE(error, DWORD(ERROR_NOT_FOUND)); credential.owned = true;
		const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
		auto held = ProjectSavePublication::acquire(target, id, QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256));
		QVERIFY(held.publication); const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
		QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("ok"));
		QCOMPARE(held.publication->moveOriginalToBackup(tx), QStringLiteral("ok"));
		QCOMPARE(held.publication->publishStage(), QStringLiteral("ok")); held.publication.reset();
		const auto marker = dir.filePath(QStringLiteral(".lmms-native-save-journal-v1"));
		QVERIFY(QFile::remove(marker)); // Model interruption after removing this test-owned marker.
		const auto retained = dir.filePath(QStringLiteral("retained-published.mmp"));
		QVERIFY(QFile::rename(target, retained));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("foreign"), qint64(7)); }
		QVERIFY(ProjectSavePublication::resolveNativeJournal(target) != QStringLiteral("committed"));
		QCOMPARE(contents(target), QByteArray("foreign")); QCOMPARE(contents(retained), QByteArray("edited"));
		QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-backup-") + tx)), QByteArray("original"));
		QVERIFY(QFile::remove(target)); QVERIFY(QFile::rename(retained, target));
		QCOMPARE(ProjectSavePublication::resolveNativeJournal(target), QStringLiteral("committed"));
		QCOMPARE(contents(target), QByteArray("edited"));
#endif
	}
	void interruptedNativeJournalRestoresAndFinalizes()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		for (const bool preRestored : {false, true})
		{
			QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
			{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
			TemporaryCredential credential; credential.name = journalCredentialName(dir.path()); QVERIFY(!credential.name.isEmpty());
			PCREDENTIALW prior = nullptr;
			const bool existed = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &prior);
			const auto error = existed ? DWORD(ERROR_SUCCESS) : GetLastError(); if (prior) CredFree(prior);
			QVERIFY(!existed); QCOMPARE(error, DWORD(ERROR_NOT_FOUND)); credential.owned = true;
			const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
			auto held = ProjectSavePublication::acquire(target, id, QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256));
			QVERIFY(held.publication); const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
			QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
			QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("ok"));
			QCOMPARE(held.publication->moveOriginalToBackup(tx), QStringLiteral("ok")); held.publication.reset();
			if (preRestored)
			{
				QCOMPARE(ProjectSavePublication::restoreOriginalFromNativeJournal(target), QStringLiteral("ok"));
				QVERIFY(QFile::remove(dir.filePath(QStringLiteral(".lmms-native-save-journal-v1"))));
			}
			QCOMPARE(ProjectSavePublication::resolveNativeJournal(target), QStringLiteral("restored"));
			QCOMPARE(contents(target), QByteArray("original"));
			QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-stage-") + tx)), QByteArray("edited"));
			QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral(".lmms-native-save-journal-v1"))));
			PCREDENTIALW pending = nullptr;
			const bool remains = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &pending);
			const auto readError = remains ? DWORD(ERROR_SUCCESS) : GetLastError(); if (pending) CredFree(pending);
			QVERIFY(!remains); QCOMPARE(readError, DWORD(ERROR_NOT_FOUND));
		}
#endif
	}
	void killedNativePublisherIsReconciledAfterRestart()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		if (qEnvironmentVariableIsSet("LMMS_TEST_NATIVE_PUBLISHER_CHILD"))
		{
			const auto target = qEnvironmentVariable("LMMS_TEST_NATIVE_TARGET");
			const auto tx = qEnvironmentVariable("LMMS_TEST_NATIVE_TX");
			const auto marker = qEnvironmentVariable("LMMS_TEST_NATIVE_READY");
			const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
			auto held = ProjectSavePublication::acquire(target, id, QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256));
			QVERIFY(held.publication); QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
			QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("ok"));
			QCOMPARE(held.publication->moveOriginalToBackup(tx), QStringLiteral("ok"));
			if (qEnvironmentVariable("LMMS_TEST_NATIVE_PHASE") == QStringLiteral("published"))
				QCOMPARE(held.publication->publishStage(), QStringLiteral("ok"));
			{ QFile f(marker); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly));
			  QCOMPARE(f.write("ready"), qint64(5)); QVERIFY(f.flush()); }
			QThread::sleep(30); QFAIL("The parent did not interrupt the disposable publisher");
		}
		for (const bool published : {false, true})
		{
			QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
			const auto ready = dir.filePath(QStringLiteral("publisher-ready"));
			{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
			TemporaryCredential credential; credential.name = journalCredentialName(dir.path()); QVERIFY(!credential.name.isEmpty());
			PCREDENTIALW prior = nullptr;
			const bool existed = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &prior);
			const auto error = existed ? DWORD(ERROR_SUCCESS) : GetLastError(); if (prior) CredFree(prior);
			QVERIFY(!existed); QCOMPARE(error, DWORD(ERROR_NOT_FOUND)); credential.owned = true;
			const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
			QProcess child; auto env = QProcessEnvironment::systemEnvironment();
			env.insert(QStringLiteral("LMMS_TEST_NATIVE_PUBLISHER_CHILD"), QStringLiteral("1"));
			env.insert(QStringLiteral("LMMS_TEST_NATIVE_TARGET"), target);
			env.insert(QStringLiteral("LMMS_TEST_NATIVE_TX"), tx);
			env.insert(QStringLiteral("LMMS_TEST_NATIVE_READY"), ready);
			env.insert(QStringLiteral("LMMS_TEST_NATIVE_PHASE"), published ? QStringLiteral("published") : QStringLiteral("gap"));
			child.setProcessEnvironment(env); const auto log = dir.filePath(QStringLiteral("publisher-child.txt"));
			child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("killedNativePublisherIsReconciledAfterRestart"),
				QStringLiteral("-o"), log + QStringLiteral(",txt")});
			QVERIFY(child.waitForStarted(5000)); QElapsedTimer timer; timer.start();
			while (!QFileInfo::exists(ready) && child.state() != QProcess::NotRunning && timer.elapsed() < 10000) QThread::msleep(20);
			const bool reached = QFileInfo::exists(ready);
			if (child.state() != QProcess::NotRunning) { child.kill(); child.waitForFinished(5000); }
			QVERIFY2(reached, contents(log).constData());
			QCOMPARE(ProjectSavePublication::resolveNativeJournal(target), published ? QStringLiteral("committed") : QStringLiteral("restored"));
			QCOMPARE(contents(target), published ? QByteArray("edited") : QByteArray("original"));
			const auto retained = dir.filePath((published ? QStringLiteral(".lmms-vocal-backup-") : QStringLiteral(".lmms-vocal-stage-")) + tx);
			QCOMPARE(contents(retained), published ? QByteArray("original") : QByteArray("edited"));
			QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral(".lmms-native-save-journal-v1"))));
		}
#endif
	}
	void rejectsByteIdenticalReplacementAndLockedTarget()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
		const auto retained = dir.filePath(QStringLiteral("retained.mmp")); const auto backup = target + QStringLiteral(".bak");
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
		{ QFile f(backup); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("backup"), qint64(6)); }
		const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
		const auto digest = QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256);
		const auto lock = CreateFileW(reinterpret_cast<LPCWSTR>(target.utf16()), GENERIC_READ, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		QVERIFY(lock != INVALID_HANDLE_VALUE);
		QVERIFY(!ProjectSavePublication::acquire(target, id, digest).publication); CloseHandle(lock);
		QCOMPARE(contents(backup), QByteArray("backup"));
		QVERIFY(QFile::rename(target, retained));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
		QVERIFY(!ProjectSavePublication::acquire(target, id, digest).publication);
		QCOMPARE(contents(target), QByteArray("original")); QCOMPARE(contents(retained), QByteArray("original"));
		QCOMPARE(contents(backup), QByteArray("backup"));
#endif
	}
	void selectedSaveTargetIsPinnedAndItsActualIdentityRecorded()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
		const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
		auto held = ProjectSavePublication::acquireSelectedTarget(target);
		QCOMPARE(held.code, QStringLiteral("ok")); QVERIFY(held.publication);
		const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
		ProjectSavePublication::Evidence evidence; QVERIFY(held.publication->evidence(evidence));
		QCOMPARE(evidence.originalIdentity, id);
		QCOMPARE(evidence.originalSha256, QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256));
		QVERIFY(!QFile::rename(target, dir.filePath(QStringLiteral("foreign"))));
#endif
	}
	void newTargetPublishesWithoutReplacingExistingBackup()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("new-project.mmp"));
		{ QFile f(target + ".bak"); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("old-backup"), qint64(10)); }
		TemporaryCredential credential; credential.name = journalCredentialName(dir.path()); QVERIFY(!credential.name.isEmpty());
		PCREDENTIALW prior = nullptr;
		const bool existed = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &prior);
		const auto error = existed ? DWORD(ERROR_SUCCESS) : GetLastError(); if (prior) CredFree(prior);
		QVERIFY(!existed); QCOMPARE(error, DWORD(ERROR_NOT_FOUND)); credential.owned = true;
		auto held = ProjectSavePublication::acquireNewTarget(target);
		QCOMPARE(held.code, QStringLiteral("ok")); QVERIFY(held.publication);
		const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(held.publication->createStage(tx, QByteArray("new-project")), QStringLiteral("ok"));
		QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("ok"));
		QCOMPARE(held.publication->commitNativeSave(), QStringLiteral("ok")); held.publication.reset();
		QCOMPARE(contents(target), QByteArray("new-project")); QCOMPARE(contents(target + ".bak"), QByteArray("old-backup"));
		QCOMPARE(ProjectSavePublication::nativeJournalStatus(target), QStringLiteral("none"));
#endif
	}
	void interruptedNewTargetJournalReconcilesWithoutOverwriting()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		for (const bool published : {false, true})
		{
			QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("new-project.mmp"));
			TemporaryCredential credential; credential.name = journalCredentialName(dir.path()); QVERIFY(!credential.name.isEmpty());
			PCREDENTIALW prior = nullptr;
			const bool existed = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &prior);
			const auto error = existed ? DWORD(ERROR_SUCCESS) : GetLastError(); if (prior) CredFree(prior);
			QVERIFY(!existed); QCOMPARE(error, DWORD(ERROR_NOT_FOUND)); credential.owned = true;
			auto held = ProjectSavePublication::acquireNewTarget(target); QVERIFY(held.publication);
			const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
			QCOMPARE(held.publication->createStage(tx, QByteArray("new-project")), QStringLiteral("ok"));
			QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("ok"));
			if (published) QCOMPARE(held.publication->publishStage(), QStringLiteral("ok"));
			held.publication.reset();
			QCOMPARE(ProjectSavePublication::resolveNativeJournal(target), published ? QStringLiteral("committed") : QStringLiteral("aborted"));
			if (published) QCOMPARE(contents(target), QByteArray("new-project"));
			else { QVERIFY(!QFileInfo::exists(target)); QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-stage-") + tx)), QByteArray("new-project")); }
			QCOMPARE(ProjectSavePublication::nativeJournalStatus(target), QStringLiteral("none"));
		}
#endif
	}
	void newTargetDoesNotReplaceACompetingFile()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("new-project.mmp"));
		TemporaryCredential credential; credential.name = journalCredentialName(dir.path()); QVERIFY(!credential.name.isEmpty());
		PCREDENTIALW prior = nullptr;
		const bool existed = CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()), CRED_TYPE_GENERIC, 0, &prior);
		const auto error = existed ? DWORD(ERROR_SUCCESS) : GetLastError(); if (prior) CredFree(prior);
		QVERIFY(!existed); QCOMPARE(error, DWORD(ERROR_NOT_FOUND)); credential.owned = true;
		auto held = ProjectSavePublication::acquireNewTarget(target); QVERIFY(held.publication);
		const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(held.publication->createStage(tx, QByteArray("new-project")), QStringLiteral("ok"));
		QCOMPARE(held.publication->prepareNativeJournal(), QStringLiteral("ok"));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("foreign"), qint64(7)); }
		QVERIFY(held.publication->commitNativeSave() != QStringLiteral("ok")); held.publication.reset();
		QVERIFY(ProjectSavePublication::resolveNativeJournal(target) != QStringLiteral("committed"));
		QCOMPARE(contents(target), QByteArray("foreign"));
		QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-stage-") + tx)), QByteArray("new-project"));
		QCOMPARE(ProjectSavePublication::nativeJournalStatus(target), QStringLiteral("pending"));
#endif
	}
	void recoveryRejectsReplacedStageAndPreservesAllFiles()
	{
#ifndef Q_OS_WIN
		QSKIP("Windows handle-bound publication");
#else
		QTemporaryDir dir; QVERIFY(dir.isValid()); const auto target = dir.filePath(QStringLiteral("project.mmp"));
		{ QFile f(target); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
		const auto id = fileIdentity(target); QCOMPARE(id.size(), 32);
		const auto digest = QCryptographicHash::hash(QByteArray("original"), QCryptographicHash::Sha256);
		auto held = ProjectSavePublication::acquire(target, id, digest); QVERIFY(held.publication);
		const auto tx = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QCOMPARE(held.publication->createStage(tx, QByteArray("edited")), QStringLiteral("ok"));
		ProjectSavePublication::Evidence evidence; QVERIFY(held.publication->evidence(evidence));
		QCOMPARE(held.publication->moveOriginalToBackup(tx), QStringLiteral("ok")); held.publication.reset();
		const auto stage = dir.filePath(QStringLiteral(".lmms-vocal-stage-") + tx);
		const auto retained = dir.filePath(QStringLiteral("retained-stage"));
		QVERIFY(QFile::rename(stage, retained));
		{ QFile f(stage); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("edited"), qint64(6)); }
		const auto reopened = ProjectSavePublication::reopenForRecovery(target, tx, evidence.originalIdentity,
			evidence.originalSha256, evidence.stageIdentity, evidence.stageSha256);
		QVERIFY(!reopened.publication); QVERIFY(!QFileInfo::exists(target));
		QCOMPARE(contents(dir.filePath(QStringLiteral(".lmms-vocal-backup-") + tx)), QByteArray("original"));
		QCOMPARE(contents(stage), QByteArray("edited")); QCOMPARE(contents(retained), QByteArray("edited"));
#endif
	}
};
QTEST_MAIN(ProjectSavePublicationTest)
#include "ProjectSavePublicationTest.moc"
