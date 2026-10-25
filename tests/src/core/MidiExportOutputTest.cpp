#include <QtTest/QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QDir>

#include <array>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "MidiExportOutput.h"

class MidiExportOutputTest : public QObject
{
	Q_OBJECT

private slots:
	void writesAtomicNoClobberMidiAndReportsHash();
	void rejectsTraversalAndNonexistentRoots();
	void concurrentNoClobberExportsPublishOnlyOneFile();
	void deniedPublicationPermitRemovesStagedFile();
	void failedSerializationRemovesStagedFile();
#ifdef Q_OS_WIN
	void overwriteDoesNotFollowDestinationSymlink();
	void configuredOutputRootSymlinkIsRejected();
#else
	void symlinkAncestorOfOutputRootIsRejected();
#endif
};

void MidiExportOutputTest::writesAtomicNoClobberMidiAndReportsHash()
{
	QTemporaryDir root;QVERIFY(root.isValid());
	lmms::MidiExportPlan plan;plan.tracks.append({QStringLiteral("Conductor"),{},0});
	const auto first=lmms::writeMidiExportFile(root.path(),QStringLiteral("song.mid"),false,plan);
	QVERIFY2(first.success,qPrintable(first.code));
	QCOMPARE(first.sha256.size(),64);
	QVERIFY(first.bytesWritten>22);
	QFile file(first.destinationPath);QVERIFY(file.open(QIODevice::ReadOnly));
	QCOMPARE(file.read(4),QByteArrayLiteral("MThd"));file.close();
	const auto second=lmms::writeMidiExportFile(root.path(),QStringLiteral("song.mid"),false,plan);
	QVERIFY(!second.success);
	QCOMPARE(second.code,QStringLiteral("destination_exists"));
	const auto replaced=lmms::writeMidiExportFile(root.path(),QStringLiteral("song.mid"),true,plan);
	QVERIFY2(replaced.success,qPrintable(replaced.code));
	QCOMPARE(replaced.sha256,first.sha256);
}

void MidiExportOutputTest::rejectsTraversalAndNonexistentRoots()
{
	QTemporaryDir root;QVERIFY(root.isValid());
	lmms::MidiExportPlan plan;plan.tracks.append({QStringLiteral("Conductor"),{},0});
	QVERIFY(!lmms::writeMidiExportFile(root.path(),QStringLiteral("../song.mid"),false,plan).success);
	QVERIFY(!lmms::writeMidiExportFile(root.path()+QStringLiteral("/missing"),QStringLiteral("song.mid"),false,plan).success);
}

void MidiExportOutputTest::concurrentNoClobberExportsPublishOnlyOneFile()
{
	QTemporaryDir root;QVERIFY(root.isValid());
	lmms::MidiExportPlan plan;plan.tracks.append({QStringLiteral("Conductor"),{},0});
	std::array<lmms::MidiExportFileResult,2> results;
	std::thread first([&]{results[0]=lmms::writeMidiExportFile(root.path(),QStringLiteral("race.mid"),false,plan);});
	std::thread second([&]{results[1]=lmms::writeMidiExportFile(root.path(),QStringLiteral("race.mid"),false,plan);});
	first.join();second.join();
	QVERIFY(results[0].success!=results[1].success);
	QCOMPARE(results[0].success?results[1].code:results[0].code,QStringLiteral("destination_exists"));
	QCOMPARE(QDir(root.path()).entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot),QStringList{QStringLiteral("race.mid")});
	QCOMPARE(QDir(root.path()).entryList(QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot),QStringList{});
}

void MidiExportOutputTest::deniedPublicationPermitRemovesStagedFile()
{
	QTemporaryDir root;QVERIFY(root.isValid());
	lmms::MidiExportPlan plan;plan.tracks.append({QStringLiteral("Conductor"),{},0});
	const auto result=lmms::writeMidiExportFile(root.path(),QStringLiteral("cancelled.mid"),false,plan,nullptr,[]{return false;});
	QVERIFY(!result.success);QCOMPARE(result.code,QStringLiteral("cancelled"));
	QCOMPARE(QDir(root.path()).entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot),QStringList{});
	QCOMPARE(QDir(root.path()).entryList(QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot),QStringList{});
}

void MidiExportOutputTest::failedSerializationRemovesStagedFile()
{
	QTemporaryDir root;QVERIFY(root.isValid());
	lmms::MidiExportPlan invalid;invalid.ppq=0;invalid.tracks.append({QStringLiteral("Conductor"),{},0});
	const auto result=lmms::writeMidiExportFile(root.path(),QStringLiteral("failed.mid"),false,invalid);
	QVERIFY(!result.success);QCOMPARE(result.code,QStringLiteral("invalid_midi_header"));
	QCOMPARE(QDir(root.path()).entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot),QStringList{});
	QCOMPARE(QDir(root.path()).entryList(QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot),QStringList{});
}

#ifdef Q_OS_WIN
void MidiExportOutputTest::overwriteDoesNotFollowDestinationSymlink()
{
	QTemporaryDir root;QVERIFY(root.isValid());
	QTemporaryDir outside;QVERIFY(outside.isValid());
	const auto outsidePath=outside.filePath(QStringLiteral("original.mid"));
	QFile original(outsidePath);QVERIFY(original.open(QIODevice::WriteOnly));QCOMPARE(original.write("preserve-me"),qint64(11));original.close();
	const auto linkPath=root.filePath(QStringLiteral("linked.mid"));
	DWORD flags=0;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
	flags|=SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
	if(!CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(linkPath.utf16()),reinterpret_cast<LPCWSTR>(outsidePath.utf16()),flags))
		QSKIP("Windows symbolic-link creation is unavailable in this environment");
	lmms::MidiExportPlan plan;plan.tracks.append({QStringLiteral("Conductor"),{},0});
	const auto result=lmms::writeMidiExportFile(root.path(),QStringLiteral("linked.mid"),true,plan);
	QVERIFY(!result.success);
	QFile unchanged(outsidePath);QVERIFY(unchanged.open(QIODevice::ReadOnly));QCOMPARE(unchanged.readAll(),QByteArrayLiteral("preserve-me"));
}

void MidiExportOutputTest::configuredOutputRootSymlinkIsRejected()
{
	QTemporaryDir root;QVERIFY(root.isValid());
	QTemporaryDir outside;QVERIFY(outside.isValid());
	const auto rootAlias=root.filePath(QStringLiteral("root-link"));
	DWORD flags=SYMBOLIC_LINK_FLAG_DIRECTORY;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
	flags|=SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
	if(!CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(rootAlias.utf16()),reinterpret_cast<LPCWSTR>(outside.path().utf16()),flags))
		QSKIP("Windows directory symbolic-link creation is unavailable in this environment");
	lmms::MidiExportPlan plan;plan.tracks.append({QStringLiteral("Conductor"),{},0});
	const auto result=lmms::writeMidiExportFile(rootAlias,QStringLiteral("escape.mid"),false,plan);
	QVERIFY(!result.success);QVERIFY(!QFile::exists(outside.filePath(QStringLiteral("escape.mid"))));
	QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(rootAlias.utf16())));
}
#else
void MidiExportOutputTest::symlinkAncestorOfOutputRootIsRejected()
{
	QTemporaryDir root;QVERIFY(root.isValid());
	QTemporaryDir outside;QVERIFY(outside.isValid());
	QVERIFY(QDir(outside.path()).mkdir(QStringLiteral("child")));
	const auto alias=root.filePath(QStringLiteral("alias"));
	QVERIFY(QFile::link(outside.path(),alias));
	lmms::MidiExportPlan plan;plan.tracks.append({QStringLiteral("Conductor"),{},0});
	const auto result=lmms::writeMidiExportFile(alias+QStringLiteral("/child"),QStringLiteral("escape.mid"),false,plan);
	QVERIFY(!result.success);QVERIFY(!QFile::exists(outside.filePath(QStringLiteral("child/escape.mid"))));
}
#endif

QTEST_APPLESS_MAIN(MidiExportOutputTest)
#include "MidiExportOutputTest.moc"
