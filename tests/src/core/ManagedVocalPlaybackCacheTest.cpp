#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <array>
#include <cmath>
#include "ConfigManager.h"
#include "ManagedVocalAssetRoot.h"
#include "ManagedVocalAssetStore.h"
#include "ManagedVocalPlaybackCache.h"

using namespace lmms;

class ManagedVocalPlaybackCacheTest : public QObject
{
	Q_OBJECT
	QString m_oldRoot;

	static QString digest(char value) { return QString(64, QChar(value)); }

	static ManagedVocalVariantReference createVariant(int rate)
	{
		auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot();
		if (!approval.root) return {};
		ManagedVocalAssetStore store(approval.root);
		auto created = store.beginCreate(3, rate, 2);
		if (!created.writer) return {};
		const std::array<float, 6> samples{0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f};
		if (created.writer->appendFrames(samples) != QStringLiteral("ok")) return {};
		ManagedVocalManifest manifest;
		manifest.projectId = QStringLiteral("11111111-1111-4111-8111-111111111111");
		manifest.assetRootId = approval.root->rootId();
		manifest.media = {3, rate, 2, digest('0'), 0.0, false};
		long double sumSquares = 0.0L;
		for (const auto sample : samples) sumSquares += static_cast<long double>(sample) * sample;
		manifest.media.rms = std::sqrt(double(sumSquares / samples.size()));
		manifest.media.rmsKnown = true;
		manifest.recipeHash = digest('a');
		manifest.rendererId = QStringLiteral("native-float-renderer");
		manifest.rendererVersion = QStringLiteral("1");
		manifest.recipe = QJsonObject{{"recipe_version", 1}, {"operation", "gain_envelope"}};
		manifest.sources.push_back({"clip-a", digest('b'), digest('c'), QStringLiteral("embedded:source"),
			{0, 8}, 8, rate, 2, ManagedVocalSourceBasis::AuthorizedPcm, {}, {}});
		const auto published = store.publish(std::move(created.writer), std::move(manifest));
		return published.value.value_or(ManagedVocalVariantReference{});
	}

private slots:
	void initTestCase()
	{
		m_oldRoot = ConfigManager::inst()->value(QStringLiteral("vocal_assets"),
			QStringLiteral("managed_root"), QStringLiteral("__unset__"));
	}

	void cleanupTestCase()
	{
		if (m_oldRoot == QStringLiteral("__unset__"))
			ConfigManager::inst()->deleteValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"));
		else
			ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"),
				QStringLiteral("managed_root"), m_oldRoot);
	}

	void sharesOneVerifiedBufferAndReleasesItsBudgetWithTheLastView()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir rootDirectory;
		QVERIFY(rootDirectory.isValid());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(rootDirectory.path()));
		const auto rate = 48000;
		const auto reference = createVariant(rate);
		QVERIFY(!reference.assetId.isEmpty());
		auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot();
		QVERIFY(approval.root);
		ManagedVocalAssetStore store(approval.root);
		ManagedVocalPlaybackCache cache(1024);

		auto first = cache.acquire(store, reference);
		QCOMPARE(first.code, QStringLiteral("ok"));
		QVERIFY(first.samples);
		QCOMPARE(cache.usedBytes(), quint64(3 * 2 * sizeof(float)));
		auto second = cache.acquire(store, reference);
		QCOMPARE(second.code, QStringLiteral("ok"));
		QVERIFY(first.samples == second.samples);
		first.samples.reset();
		QCOMPARE(cache.usedBytes(), quint64(3 * 2 * sizeof(float)));
		second.samples.reset();
		QTRY_COMPARE(cache.usedBytes(), quint64(0));
#endif
	}

	void warmCacheDoesNotReuseSamplesForAMismatchedManifestDigest()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir rootDirectory;
		QVERIFY(rootDirectory.isValid());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(rootDirectory.path()));
		const auto reference = createVariant(48000);
		QVERIFY(!reference.assetId.isEmpty());
		auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot();
		QVERIFY(approval.root);
		ManagedVocalAssetStore store(approval.root);
		ManagedVocalPlaybackCache cache(1024);
		const auto verified = cache.acquire(store, reference);
		QCOMPARE(verified.code, QStringLiteral("ok"));
		auto mismatched = reference;
		mismatched.manifestSha256 = digest('d');
		const auto rejected = cache.acquire(store, mismatched);
		QCOMPARE(rejected.code, QStringLiteral("asset_integrity_failed"));
		QVERIFY(!rejected.samples);
#endif
	}

	void warmCacheDoesNotReuseSamplesForMismatchedFrameFormat()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir rootDirectory;
		QVERIFY(rootDirectory.isValid());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(rootDirectory.path()));
		const auto reference = createVariant(48000);
		QVERIFY(!reference.assetId.isEmpty());
		auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot();
		QVERIFY(approval.root);
		ManagedVocalAssetStore store(approval.root);
		ManagedVocalPlaybackCache cache(1024);
		const auto verified = cache.acquire(store, reference);
		QCOMPARE(verified.code, QStringLiteral("ok"));
		auto mismatched = reference;
		mismatched.frames = 6;
		mismatched.channels = 1;
		const auto rejected = cache.acquire(store, mismatched);
		QCOMPARE(rejected.code, QStringLiteral("asset_integrity_failed"));
		QVERIFY(!rejected.samples);
#endif
	}

	void warmCacheDoesNotReuseBufferAcrossProjectOwners()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir rootDirectory;
		QVERIFY(rootDirectory.isValid());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(rootDirectory.path()));
		const auto reference = createVariant(48000);
		QVERIFY(!reference.assetId.isEmpty());
		auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot();
		QVERIFY(approval.root);
		ManagedVocalAssetStore store(approval.root);
		ManagedVocalPlaybackCache cache(1024);
		const auto owner = QStringLiteral("11111111-1111-4111-8111-111111111111");
		const auto foreignOwner = QStringLiteral("22222222-2222-4222-8222-222222222222");
		const auto verified = cache.acquire(store, reference, owner);
		QCOMPARE(verified.code, QStringLiteral("ok"));
		const auto rejected = cache.acquire(store, reference, foreignOwner);
		QCOMPARE(rejected.code, QStringLiteral("asset_project_mismatch"));
		QVERIFY(!rejected.samples);
#endif
	}

	void refusesAdmissionBeforeExceedingPlaybackMemoryBudget()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir rootDirectory;
		QVERIFY(rootDirectory.isValid());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(rootDirectory.path()));
		const auto reference = createVariant(44100);
		QVERIFY(!reference.assetId.isEmpty());
		auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot();
		QVERIFY(approval.root);
		ManagedVocalAssetStore store(approval.root);
		ManagedVocalPlaybackCache cache(16);
		const auto refused = cache.acquire(store, reference);
		QCOMPARE(refused.code, QStringLiteral("resource_limit"));
		QVERIFY(!refused.samples);
		QCOMPARE(cache.usedBytes(), quint64(0));
#endif
	}

	void loadsVerifiedPlaybackBuffersThroughASeparateBoundedTicket()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir rootDirectory;
		QVERIFY(rootDirectory.isValid());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(rootDirectory.path()));
		const auto reference = createVariant(48000);
		QVERIFY(!reference.assetId.isEmpty());
		auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot();
		QVERIFY(approval.root);
		ManagedVocalAssetStore store(approval.root);
		ManagedVocalPlaybackCache cache(1024);

		const auto request = cache.requestLoad(store, reference);
		QCOMPARE(request.code, QStringLiteral("ok"));
		QVERIFY(request.ticket);
		QTRY_VERIFY_WITH_TIMEOUT(request.ticket->isComplete(), 5000);
		const auto loaded = request.ticket->result();
		QCOMPARE(loaded.code, QStringLiteral("ok"));
		QVERIFY(loaded.samples);
		QCOMPARE(loaded.samples->size(), std::size_t(6));
		QCOMPARE(cache.usedBytes(), quint64(6 * sizeof(float)));
#endif
	}
};

QTEST_APPLESS_MAIN(ManagedVocalPlaybackCacheTest)
#include "ManagedVocalPlaybackCacheTest.moc"
