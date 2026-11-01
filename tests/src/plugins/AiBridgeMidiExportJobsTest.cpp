#include <QtTest/QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QStringList>
#include <limits>

#include "AiBridgeMidiExportJobs.h"
#include "AutomationTrack.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "Song.h"
#include "TimePos.h"

class AiBridgeMidiExportJobsTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase(){lmms::Engine::init(true);lmms::Engine::audioEngine()->audioDev()->stopProcessing();}
	void cleanupTestCase(){lmms::Engine::destroy();}
	void exportsEmptySongAndRetainsAnIdempotentJobResult();
	void exportsSelectedNativeTrackWithItsTimelineOffset();
};

void AiBridgeMidiExportJobsTest::exportsEmptySongAndRetainsAnIdempotentJobResult()
{
	QTemporaryDir root;QVERIFY(root.isValid());
	lmms::ConfigManager::inst()->setValue("aibridge","midi_export_root",root.path());
	lmms::AiBridgeMidiExportJobs jobs;
	lmms::MidiExportSelection selection;
	const QString operation="93ad2d63-8335-4af8-b9df-ab7728132619";
	const QJsonObject request{{"operation_id",operation},{"filename","empty.mid"}};
	const auto started=jobs.start(operation,selection,true,"empty.mid",false,request);
	QVERIFY2(started.code=="ok",qPrintable(started.code));
	QString state;
	for(int i=0;i<200;++i)
	{
		const auto current=jobs.status(operation,"summary",0,32);QVERIFY(current.code=="ok");state=current.data.value("state").toString();
		if(state=="succeeded"||state=="failed"||state=="cancelled")break;
		QTest::qWait(10);
	}
	QCOMPARE(state,QStringLiteral("succeeded"));
	const auto result=jobs.status(operation,"summary",0,32);
	QCOMPARE(result.data.value("snapshot_hash").toString().size(),64);
	QVERIFY(result.data.value("output_root_configured").toBool());
	QCOMPARE(result.data.value("musical_tracks").toInt(),0);
	const auto destination=result.data.value("destination_path").toString();QVERIFY(!destination.isEmpty());
	QFile file(destination);QVERIFY(file.open(QIODevice::ReadOnly));
	QCOMPARE(file.read(4),QByteArrayLiteral("MThd"));
	const auto repeated=jobs.start(operation,selection,true,"empty.mid",false,request);
	QCOMPARE(repeated.code,QStringLiteral("ok"));QCOMPARE(repeated.data.value("job_id"),started.data.value("job_id"));
	const auto cancelled=jobs.cancel(operation);QVERIFY(cancelled.code=="ok");QVERIFY(!cancelled.data.value("cancel_applied").toBool());
	QCOMPARE(cancelled.data.size(),4);
	QVERIFY(cancelled.data.contains("operation_id"));QVERIFY(cancelled.data.contains("job_id"));QVERIFY(cancelled.data.contains("state"));
	const auto oversizedPage=jobs.status(operation,"tracks",std::numeric_limits<int>::max(),64);
	QVERIFY(oversizedPage.code=="ok");QCOMPARE(oversizedPage.data.value("offset").toInt(),1);QVERIFY(!oversizedPage.data.value("has_more").toBool());
	QVERIFY(jobs.release(operation).data.value("released").toBool());
}

void AiBridgeMidiExportJobsTest::exportsSelectedNativeTrackWithItsTimelineOffset()
{
	QTemporaryDir root;QVERIFY(root.isValid());
	lmms::ConfigManager::inst()->setValue("aibridge","midi_export_root",root.path());
	auto* song=lmms::Engine::getSong();QVERIFY(song);
	new lmms::AutomationTrack(song);
	auto* track=new lmms::InstrumentTrack(song);track->setName("Lead");
	auto* clip=dynamic_cast<lmms::MidiClip*>(track->createClip(lmms::TimePos(192)));QVERIFY(clip);
	clip->addNote(lmms::Note(lmms::TimePos(48),lmms::TimePos(0),60,100),false);
	lmms::AiBridgeProjectContext identities;const auto clipId=identities.idForClip(clip);QVERIFY(!clipId.isEmpty());
	lmms::MidiExportSelection selection;selection.mode=lmms::MidiExportSelectionMode::Clips;selection.clipIds=QStringList{clipId};
	lmms::AiBridgeMidiExportJobs jobs;
	const QString operation="93ad2d63-8335-4af8-b9df-ab7728132620";
	QJsonArray ids;ids.append(clipId);QJsonObject selectionJson{{"mode","clips"},{"clip_ids",ids}};
	const QJsonObject request{{"operation_id",operation},{"filename","lead.mid"},{"selection",selectionJson}};
	const auto started=jobs.start(operation,selection,true,"lead.mid",false,request);QCOMPARE(started.code,QStringLiteral("ok"));
	QString state;
	for(int i=0;i<200;++i){const auto current=jobs.status(operation,"summary",0,32);QVERIFY(current.code=="ok");state=current.data.value("state").toString();if(state=="succeeded"||state=="failed")break;QTest::qWait(10);}
	QCOMPARE(state,QStringLiteral("succeeded"));
	const auto result=jobs.status(operation,"summary",0,32);QCOMPARE(result.data.value("musical_tracks").toInt(),1);QCOMPARE(result.data.value("notes_written").toInt(),1);
	QCOMPARE(result.data.value("start_tick").toInt(),192);QVERIFY(QFile::exists(result.data.value("destination_path").toString()));
}

QTEST_MAIN(AiBridgeMidiExportJobsTest)
#include "AiBridgeMidiExportJobsTest.moc"
