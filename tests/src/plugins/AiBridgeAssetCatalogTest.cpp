#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>

#include "AiBridgeAssetCatalog.h"

class AiBridgeAssetCatalogTest : public QObject
{
	Q_OBJECT

private slots:
	void assetIdRoundTripsNormalizedSeparators()
	{
		const auto id = lmms::AiBridgeAssetCatalog::makeAssetId(
			lmms::AiBridgeAssetKind::Sample, QStringLiteral("audiofileprocessor"),
			QStringLiteral("drums\\Kick.WAV"));
		QVERIFY(id.has_value());
		QCOMPARE(*id, QStringLiteral("sample/audiofileprocessor/drums/Kick.WAV"));
		const auto relative = lmms::AiBridgeAssetCatalog::parseAssetId(
			*id, lmms::AiBridgeAssetKind::Sample);
		QVERIFY(relative.has_value());
		QCOMPARE(*relative, QStringLiteral("drums/Kick.WAV"));
	}

	void rejectsDotDotDriveUncDeviceAndMalformedIds()
	{
		for (const auto &resource : {QStringLiteral("../x.wav"), QStringLiteral("C:/x.wav"),
			QStringLiteral("//server/x.wav"), QStringLiteral("\\\\.\\pipe\\x"), QStringLiteral("")})
			QVERIFY(!lmms::AiBridgeAssetCatalog::makeAssetId(
				lmms::AiBridgeAssetKind::Sample, QStringLiteral("audiofileprocessor"), resource));
		QVERIFY(!lmms::AiBridgeAssetCatalog::parseAssetId(
			QStringLiteral("not-an-asset-id"), lmms::AiBridgeAssetKind::Sample));
	}

	void rejectsCaseInsensitiveRootEscape()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		QDir root(directory.path());
		QVERIFY(root.mkpath(QStringLiteral("samples/drums")));
		QFile file(root.filePath(QStringLiteral("samples/drums/kick.wav")));
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.close();
		QVERIFY(lmms::AiBridgeAssetCatalog::canonicalContained(
			root.filePath(QStringLiteral("samples")),
			root.filePath(QStringLiteral("samples/drums/kick.wav"))));
		QVERIFY(!lmms::AiBridgeAssetCatalog::canonicalContained(
			root.filePath(QStringLiteral("samples")),
			root.filePath(QStringLiteral("samples-old/kick.wav"))));
	}

	void rejectsMissingCanonicalTarget()
	{
		QVERIFY(!lmms::AiBridgeAssetCatalog::canonicalContained(
			QStringLiteral("Z:/path/that/does/not/exist"),
			QStringLiteral("Z:/path/that/does/not/exist/file.wav")));
	}

	void listsOnlyWavOggDsFlacAndPagesDeterministically()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		QDir root(directory.path());
		for (const auto &name : {QStringLiteral("b.ogg"), QStringLiteral("a.wav"),
			QStringLiteral("c.ds"), QStringLiteral("d.flac"), QStringLiteral("ignore.mp3")})
		{
			QFile file(root.filePath(name));
			QVERIFY(file.open(QIODevice::WriteOnly));
			file.close();
		}
		lmms::AiBridgeAssetFilter filter;
		filter.limit = 2;
		const auto first = lmms::AiBridgeAssetCatalog::listBundledSamplesAtRoot(directory.path(), filter);
		QCOMPARE(first.total, 4);
		QCOMPARE(first.offset, 0);
		QCOMPARE(first.limit, 2);
		QVERIFY(first.hasMore);
		QCOMPARE(first.items.size(), 2);
		QCOMPARE(first.items.at(0).displayName, QStringLiteral("a"));
		QCOMPARE(first.items.at(1).displayName, QStringLiteral("b"));
		filter.offset = 2;
		const auto second = lmms::AiBridgeAssetCatalog::listBundledSamplesAtRoot(directory.path(), filter);
		QCOMPARE(second.items.size(), 2);
		QCOMPARE(second.items.at(0).displayName, QStringLiteral("c"));
		QCOMPARE(second.items.at(1).displayName, QStringLiteral("d"));
		QVERIFY(!second.hasMore);
	}

	void filtersByFormatAndQuery()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		QDir root(directory.path());
		QVERIFY(root.mkpath(QStringLiteral("drums")));
		QFile file(root.filePath(QStringLiteral("drums/Kick.WAV")));
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.close();
		lmms::AiBridgeAssetFilter filter;
		filter.format = QStringLiteral("wav");
		filter.query = QStringLiteral("kick");
		const auto page = lmms::AiBridgeAssetCatalog::listBundledSamplesAtRoot(directory.path(), filter);
		QCOMPARE(page.total, 1);
		QCOMPARE(page.items.size(), 1);
		QCOMPARE(page.items.at(0).category, QStringLiteral("drums"));
		QCOMPARE(page.items.at(0).format, QStringLiteral(".wav"));
		QCOMPARE(page.items.at(0).instrumentId, QStringLiteral("audiofileprocessor"));
	}

	void developmentRootIsTrustedWhenPresent()
	{
		const auto page = lmms::AiBridgeAssetCatalog::listBundledSamples({});
		QVERIFY(page.total > 0);
		for (const auto &item : page.items)
		{
			QVERIFY(!item.assetId.contains(':'));
			QVERIFY(!item.assetId.contains(QStringLiteral("..")));
		}
	}
};

QTEST_MAIN(AiBridgeAssetCatalogTest)
#include "AiBridgeAssetCatalogTest.moc"
