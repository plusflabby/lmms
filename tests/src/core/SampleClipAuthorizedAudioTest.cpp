#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
class SampleClipAuthorizedAudioTest : public QObject
{
	Q_OBJECT
	QStringList old;
	const QStringList keys{"audio_import_roots", "audio_import_max_file_bytes", "audio_import_max_decoded_bytes"};
	QString fixture() { return QFileInfo(QFINDTESTDATA("../../../data/samples/shapes/vowel_u.wav")).canonicalFilePath(); }
	void root(const QString& path) { lmms::ConfigManager::inst()->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{path}).toJson(QJsonDocument::Compact))); }
	QDomElement save(lmms::SampleClip& clip)
	{ QDomDocument d; auto e=d.createElement("sampleclip"); d.appendChild(e); clip.saveSettings(d,e); return e; }
private slots:
	void initTestCase() { lmms::Engine::init(true); }
	void cleanupTestCase() { lmms::Engine::destroy(); }
	void init()
	{
		for(auto& key:keys) old<<lmms::ConfigManager::inst()->value("aibridge",key,"test-absent");
		root(QFileInfo(fixture()).absolutePath());
		lmms::ConfigManager::inst()->setValue("aibridge",keys[1],"20000"); lmms::ConfigManager::inst()->setValue("aibridge",keys[2],"20000");
	}
	void cleanup()
	{
		for(int i=0;i<keys.size();++i) { if(old[i]=="test-absent") lmms::ConfigManager::inst()->deleteValue("aibridge",keys[i]); else lmms::ConfigManager::inst()->setValue("aibridge",keys[i],old[i]); }
		old.clear(); lmms::AudioImportPolicy::configured().refresh();
	}
	void adoptionPreservesAuthorizedIdentityAndNativeReload()
	{
		auto prepared=lmms::AudioImportPolicy::configured().prepare(fixture()); QCOMPARE(prepared.code,QStringLiteral("ok"));
		const auto identity=prepared.audio->identity(); const auto sha=prepared.audio->info().sourceSha256;
		lmms::SampleTrack track(lmms::Engine::getSong()); lmms::SampleClip clip(&track);
		QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QStringLiteral("ok"));
		QCOMPARE(clip.sample().sampleSize(),size_t(337)); QVERIFY(clip.usesBoundedSampleSource());
		clip.movePosition(300); clip.setAutoResize(false); clip.changeLength(40); clip.setStartTimeOffset(-10);
		auto saved=save(clip); QCOMPARE(saved.attribute("source_file_id").toLatin1(),identity.toHex());
		QCOMPARE(saved.attribute("source_sha256").toLatin1(),sha.toHex());
		lmms::SampleClip restored(&track); restored.loadSettings(saved);
		QCOMPARE(restored.sample().sampleSize(),size_t(337)); QCOMPARE(restored.startPosition().getTicks(),300);
		QCOMPARE(restored.length().getTicks(),40); QCOMPARE(restored.startTimeOffset().getTicks(),-10);
		QCOMPARE(save(restored).attribute("source_file_id"),saved.attribute("source_file_id"));
		QFile f(fixture()); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(QCryptographicHash::hash(f.readAll(),QCryptographicHash::Sha256),sha);
	}
	void revokedPreparedAudioCannotReplaceExistingNativeBuffer()
	{
		auto prepared=lmms::AudioImportPolicy::configured().prepare(fixture()); QCOMPARE(prepared.code,QStringLiteral("ok"));
		lmms::SampleTrack track(lmms::Engine::getSong()); lmms::SampleClip clip(&track);
		clip.setSampleBuffer(std::make_shared<const lmms::SampleBuffer>(std::vector<lmms::SampleFrame>(4),44100));
		lmms::ConfigManager::inst()->setValue("aibridge",keys[0],"[]");
		QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QStringLiteral("plan_stale"));
		QCOMPARE(clip.sample().sampleSize(),size_t(4)); QVERIFY(!clip.usesBoundedSampleSource());
	}
	void boundedReloadMustReauthorizeAndRetainDeniedReference()
	{
		lmms::SampleTrack track(lmms::Engine::getSong()); lmms::SampleClip clip(&track);
		auto prepared=lmms::AudioImportPolicy::configured().prepare(fixture());
		QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QStringLiteral("ok")); auto saved=save(clip);
		lmms::ConfigManager::inst()->setValue("aibridge",keys[0],"[]");
		clip.loadSettings(saved); QCOMPARE(clip.sample().sampleSize(),size_t(0)); QVERIFY(!clip.sourceLoadError().isEmpty());
		QCOMPARE(clip.sampleFile(),saved.attribute("src"));
		QCOMPARE(save(clip).attribute("source_sha256"),saved.attribute("source_sha256"));
		QCOMPARE(save(clip).attribute("source_file_id"),saved.attribute("source_file_id"));
		root(QFileInfo(fixture()).absolutePath()); clip.loadSettings(saved); QCOMPARE(clip.sample().sampleSize(),size_t(337));
		saved.setAttribute("source_file_id",QString(64,QChar('0'))); clip.loadSettings(saved);
		QCOMPARE(clip.sample().sampleSize(),size_t(0)); QCOMPARE(clip.sourceLoadError(),QStringLiteral("audio_source_changed"));
		QCOMPARE(save(clip).attribute("source_file_id"),saved.attribute("source_file_id"));
	}
#ifdef Q_OS_WIN
	void reloadKeepsItsAuthorizedHandleThroughNativeAdoptionNotifications()
	{
		lmms::SampleTrack track(lmms::Engine::getSong()); lmms::SampleClip clip(&track);
		auto prepared=lmms::AudioImportPolicy::configured().prepare(fixture());
		QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QStringLiteral("ok")); const auto saved=save(clip);
		bool observed=false, locked=true;
		connect(&clip,&lmms::SampleClip::sampleChanged,this,[&] {
			observed=true;
			const auto path=fixture();
			const auto writer=CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),GENERIC_WRITE,
				FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
			if(writer!=INVALID_HANDLE_VALUE) { locked=false; CloseHandle(writer); }
			else if(GetLastError()!=ERROR_SHARING_VIOLATION) locked=false;
		});
		clip.loadSettings(saved); QCOMPARE(clip.sample().sampleSize(),size_t(337)); QVERIFY(observed); QVERIFY(locked);
	}
	void missingAndReparseSourcesRetainTheirNativeReferences()
	{
		lmms::SampleTrack track(lmms::Engine::getSong()); lmms::SampleClip clip(&track);
		auto prepared=lmms::AudioImportPolicy::configured().prepare(fixture());
		QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QStringLiteral("ok")); auto saved=save(clip);
		QTemporaryDir directory(QCoreApplication::applicationDirPath()+"/adopt-XXXXXX"); QVERIFY(directory.isValid());
		root(directory.path());
		const auto link=directory.filePath("escape.wav"); const auto target=fixture();
		QVERIFY(CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(link.utf16()),reinterpret_cast<LPCWSTR>(target.utf16()),SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));
		saved.setAttribute("src",link); clip.loadSettings(saved);
		QCOMPARE(clip.sourceLoadError(),QStringLiteral("import_path_denied")); QCOMPARE(clip.sample().sampleSize(),size_t(0));
		QCOMPARE(save(clip).attribute("src"),link); QVERIFY(DeleteFileW(reinterpret_cast<LPCWSTR>(link.utf16())));
		const auto missing=directory.filePath("missing.wav"); saved.setAttribute("src",missing);
		for(int i=0;i<2;++i) { clip.loadSettings(saved); QCOMPARE(clip.sourceLoadError(),QStringLiteral("audio_source_missing")); saved=save(clip); QCOMPARE(saved.attribute("src"),missing); QVERIFY(!saved.attribute("source_file_id").isEmpty()); }
		lmms::ConfigManager::inst()->setValue("aibridge",keys[0],"[]"); lmms::AudioImportPolicy::configured().refresh();
	}
#endif
	void invalidNativeIdentityStaysQuarantinedAndExplicitBufferClearsIt()
	{
		lmms::SampleTrack track(lmms::Engine::getSong()); lmms::SampleClip clip(&track);
		auto prepared=lmms::AudioImportPolicy::configured().prepare(fixture());
		QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QStringLiteral("ok")); auto saved=save(clip);
		saved.setAttribute("source_file_id","invalid"); clip.loadSettings(saved); saved=save(clip); clip.loadSettings(saved);
		QCOMPARE(clip.sourceLoadError(),QStringLiteral("unsupported_audio_operation")); QCOMPARE(clip.sample().sampleSize(),size_t(0));
		clip.setSampleBuffer(std::make_shared<const lmms::SampleBuffer>(std::vector<lmms::SampleFrame>(4),44100));
		auto document=saved.ownerDocument(); clip.saveSettings(document,saved); QVERIFY(!saved.hasAttribute("source_file_id")); QVERIFY(!saved.hasAttribute("bounded_source"));
	}
};
QTEST_MAIN(SampleClipAuthorizedAudioTest)
#include "SampleClipAuthorizedAudioTest.moc"
