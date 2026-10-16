#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QFile>
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
class AudioImportPolicyTest : public QObject
{
	Q_OBJECT
	QStringList old;
	const QStringList keys{"audio_import_roots", "audio_import_max_file_bytes", "audio_import_max_decoded_bytes"};
	QString fixture() { return QFileInfo(QFINDTESTDATA("../../../data/samples/shapes/vowel_u.wav")).canonicalFilePath(); }
	void settings(const QString& roots, const QString& file = "20000", const QString& memory = "20000")
	{
		auto c = lmms::ConfigManager::inst();
		c->setValue("aibridge", keys[0], roots); c->setValue("aibridge", keys[1], file); c->setValue("aibridge", keys[2], memory);
	}
	QString roots() { return QString::fromUtf8(QJsonDocument(QJsonArray{QFileInfo(fixture()).absolutePath()}).toJson(QJsonDocument::Compact)); }
private slots:
	void init() { for (auto& key : keys) old << lmms::ConfigManager::inst()->value("aibridge", key, "test-absent"); }
	void cleanup()
	{
		for (int i=0;i<keys.size();++i) { if(old[i]=="test-absent") lmms::ConfigManager::inst()->deleteValue("aibridge",keys[i]); else lmms::ConfigManager::inst()->setValue("aibridge",keys[i],old[i]); }
		old.clear();
	}
	void preparesBoundedAudioOnlyUnderExplicitLocalRoot()
	{
		settings(roots()); lmms::AudioImportPolicy policy;
		auto result = policy.prepare(fixture());
		QCOMPARE(result.code, QStringLiteral("ok")); QVERIFY(result.audio);
		QVERIFY(result.audio->isCurrent()); QCOMPARE(result.audio->info().frames, quint64(337));
		QCOMPARE(result.audio->info().channels, 2); QVERIFY(!result.audio->identity().isEmpty());
		QFile original(fixture()); QVERIFY(original.open(QIODevice::ReadOnly));
		QCOMPARE(result.audio->info().sourceSha256, QCryptographicHash::hash(original.readAll(),QCryptographicHash::Sha256));
	}
	void rejectsMissingMalformedAndUnapprovedConfiguration()
	{
		lmms::AudioImportPolicy policy;
		for(const auto& raw : QStringList{"", "[]", "{}", "invalid", "[4]", "[\"relative\"]"})
		{ settings(raw); auto r=policy.prepare(fixture()); QVERIFY(r.code != "ok"); QVERIFY(!r.audio); }
		settings("[\"G:/lmms-plus-phase3i/lmms/tests\"]");
		QCOMPARE(policy.prepare(fixture()).code,QStringLiteral("import_path_denied"));
		settings(roots(), "", "20000"); QCOMPARE(policy.prepare(fixture()).code,QStringLiteral("audio_resource_limits_not_configured"));
		settings(roots(), "1", "1"); QCOMPARE(policy.prepare(fixture()).code,QStringLiteral("source_too_large"));
	}
	void configurationChangesRevokePreparedAudioAndBoundOutstandingWork()
	{
		settings(roots()); lmms::AudioImportPolicy policy;
		auto result=policy.prepare(fixture()); QCOMPARE(result.code,QStringLiteral("ok"));
		QCOMPARE(policy.prepare(fixture()).code,QStringLiteral("audio_import_busy"));
		settings("[]"); QVERIFY(!result.audio->isCurrent());
		QVERIFY(policy.refresh()!="ok"); QVERIFY(!result.audio->isCurrent());
		settings(roots()); QCOMPARE(policy.refresh(),QStringLiteral("ok"));
		QVERIFY(!result.audio->isCurrent());
		result.audio.reset(); auto current=policy.prepare(fixture()); QCOMPARE(current.code,QStringLiteral("ok"));
	}
	void restoringSettingsCannotResurrectAnEarlierAuthorization()
	{
		settings(roots()); lmms::AudioImportPolicy policy;
		auto prepared=policy.prepare(fixture()); QCOMPARE(prepared.code,QStringLiteral("ok"));
		settings("[]"); settings(roots());
		QVERIFY(!prepared.audio->isCurrent());
		QCOMPARE(policy.prepare(fixture()).code,QStringLiteral("audio_import_busy"));
		prepared.audio.reset(); QCOMPARE(policy.prepare(fixture()).code,QStringLiteral("ok"));
	}
	void preparedImportsAreBoundedAcrossPolicyInstances()
	{
		settings(roots()); lmms::AudioImportPolicy first, second;
		auto held=first.prepare(fixture()); QCOMPARE(held.code,QStringLiteral("ok"));
		QCOMPARE(second.prepare(fixture()).code,QStringLiteral("audio_import_busy"));
		held.audio.reset(); QCOMPARE(second.prepare(fixture()).code,QStringLiteral("ok"));
	}
};
QTEST_GUILESS_MAIN(AudioImportPolicyTest)
#include "AudioImportPolicyTest.moc"
