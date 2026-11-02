#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QDir>
#include <QUuid>
#include <array>
#include <limits>
#include <cstring>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#endif
#include "ManagedVocalAssetRoot.h"
#include "ManagedVocalAssetStore.h"
#include "RecordingLibrary.h"
#include "RecordingRecovery.h"

using namespace lmms;

#ifdef Q_OS_WIN
namespace {
struct JunctionCleanup
{
	QString path;
	~JunctionCleanup()
	{
		if (!path.isEmpty()) RemoveDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16()));
	}
};

bool createDirectoryJunction(const QString& linkPath, QString targetPath)
{
	if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(linkPath.utf16()), nullptr))
	{
		return false;
	}
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(linkPath.utf16()),
		GENERIC_READ | GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		RemoveDirectoryW(reinterpret_cast<LPCWSTR>(linkPath.utf16()));
		return false;
	}
	targetPath.replace('/', '\\');
	const auto substitute = QStringLiteral("\\??\\") + targetPath;
	const auto substituteBytes = USHORT(substitute.size() * sizeof(wchar_t));
	const auto printBytes = USHORT(targetPath.size() * sizeof(wchar_t));
	const auto dataLength = USHORT(8 + substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t));
	std::vector<BYTE> bytes(std::size_t(dataLength) + 8, 0);
	const ULONG tag = IO_REPARSE_TAG_MOUNT_POINT;
	const USHORT zero = 0;
	const USHORT printOffset = USHORT(substituteBytes + sizeof(wchar_t));
	std::memcpy(bytes.data(), &tag, sizeof(tag));
	std::memcpy(bytes.data() + 4, &dataLength, sizeof(dataLength));
	std::memcpy(bytes.data() + 8, &zero, sizeof(zero));
	std::memcpy(bytes.data() + 10, &substituteBytes, sizeof(substituteBytes));
	std::memcpy(bytes.data() + 12, &printOffset, sizeof(printOffset));
	std::memcpy(bytes.data() + 14, &printBytes, sizeof(printBytes));
	std::memcpy(bytes.data() + 16, substitute.utf16(), substituteBytes);
	std::memcpy(bytes.data() + 16 + substituteBytes + sizeof(wchar_t), targetPath.utf16(), printBytes);
	DWORD returned = 0;
	const bool linked = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, bytes.data(), DWORD(bytes.size()),
		nullptr, 0, &returned, nullptr) != FALSE;
	CloseHandle(handle);
	if (!linked) RemoveDirectoryW(reinterpret_cast<LPCWSTR>(linkPath.utf16()));
	return linked;
}
}
#endif

class ManagedVocalAssetStoreTest : public QObject
{
	Q_OBJECT

	static QString digest(char value)
	{
		return QString(64, QChar(value));
	}

	static QByteArray contents(const QString& path)
	{
		QFile file(path);
		return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
	}

	static ManagedVocalManifest manifest(const QString& assetId, const QString& sourcePath)
	{
		ManagedVocalManifest value;
		value.assetId = assetId;
		value.assetRootId = digest('e');
		value.media = {3, 48000, 1, digest('0'), 0.0, false};
		value.recipeHash = digest('a');
		value.rendererId = QStringLiteral("native-float-renderer");
		value.rendererVersion = QStringLiteral("1");
		value.recipe = QJsonObject{{"recipe_version", 1}, {"operation", "gain_envelope"}};
		value.sources.push_back({"clip-a", digest('b'), digest('c'), sourcePath, {0, 3}, 3, 48000, 1,
			ManagedVocalSourceBasis::AuthorizedPcm, {}, {}});
		return value;
	}

private slots:
	void publishesImmutableVerifiedVariantAndPreservesOriginal()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		const auto originalPath = temp.filePath(QStringLiteral("lead.wav"));
		const QByteArray originalBytes("immutable original vocal");
		{
			QFile original(originalPath);
			QVERIFY(original.open(QIODevice::WriteOnly | QIODevice::NewOnly));
			QCOMPARE(original.write(originalBytes), qint64(originalBytes.size()));
		}
		const auto originalHash = QCryptographicHash::hash(contents(originalPath), QCryptographicHash::Sha256);
		auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(temp.path());
		QCOMPARE(approval.code, QStringLiteral("ok"));
		QVERIFY(approval.root);
		QCOMPARE(approval.root->rootId().size(), 64);
		ManagedVocalAssetStore store(approval.root);
		auto created = store.beginCreate(3, 48000, 1);
		QCOMPARE(created.code, QStringLiteral("ok"));
		QVERIFY(created.writer);
		const std::array<float, 3> samples{0.0f, 0.5f, -1.25f};
		QCOMPARE(created.writer->appendFrames(samples), QStringLiteral("ok"));
		auto published = store.publish(std::move(created.writer), manifest(created.assetId, originalPath));
		QCOMPARE(published.code, QStringLiteral("ok"));
		QVERIFY(published.value);
		QCOMPARE(published.value->frames, quint64(3));
		QVERIFY(!published.value->mediaSha256.contains('/'));

		const auto inspection = store.inspect(*published.value);
		QCOMPARE(inspection.code, QStringLiteral("ok"));
		QVERIFY(inspection.reference);
		const auto opened = store.openVerified(*published.value);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		QCOMPARE(opened.samples, std::vector<float>({0.0f, 0.5f, -1.25f}));
		QCOMPARE(QCryptographicHash::hash(contents(originalPath), QCryptographicHash::Sha256), originalHash);
		QCOMPARE(contents(originalPath), originalBytes);

		const auto mediaPath = temp.filePath(QStringLiteral("vocal-asset-") + published.value->assetId
			+ QStringLiteral(".wav"));
		QFile changed(mediaPath);
		QVERIFY(changed.open(QIODevice::ReadWrite));
		QVERIFY(changed.seek(56));
		QCOMPARE(changed.write("\x01", 1), qint64(1));
		changed.close();
		QCOMPARE(store.inspect(*published.value).code, QStringLiteral("asset_integrity_failed"));
	}

	void cleansOnlyOwnedIncompleteStagesAndEnforcesBudgets()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#endif
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		ManagedVocalAssetStore unavailable({});
		QCOMPARE(unavailable.beginCreate(3, 48000, 1).code, QStringLiteral("asset_root_not_configured"));
		auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(temp.path());
		QCOMPARE(approval.code, QStringLiteral("ok"));
		ManagedVocalAssetStore store(approval.root, 1024);
		QCOMPARE(store.beginCreate(0, 48000, 1).code, QStringLiteral("invalid_parameter"));
		QCOMPARE(store.beginCreate(1000000, 48000, 2).code, QStringLiteral("resource_limit"));

		auto created = store.beginCreate(3, 48000, 1);
		QVERIFY(created.writer);
		const auto partialName = QStringLiteral("vocal-asset-") + created.assetId + QStringLiteral(".wav.partial");
		QVERIFY(QFileInfo::exists(temp.filePath(partialName)));
		const std::array<float, 2> shortBlock{0.0f, 0.25f};
		QCOMPARE(created.writer->appendFrames(shortBlock), QStringLiteral("ok"));
		QCOMPARE(store.publish(std::move(created.writer), manifest(created.assetId, {})).code,
			QStringLiteral("asset_incomplete"));
		QVERIFY(!QFileInfo::exists(temp.filePath(partialName)));
	}

	void rejectsManifestChangedAfterPublication()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(temp.path());
		QVERIFY(approval.root);
		ManagedVocalAssetStore store(approval.root);
		auto created = store.beginCreate(3, 48000, 1);
		QVERIFY(created.writer);
		const std::array<float, 3> samples{0.0f, 0.25f, -0.5f};
		QCOMPARE(created.writer->appendFrames(samples), QStringLiteral("ok"));
		auto published = store.publish(std::move(created.writer), manifest(created.assetId, temp.filePath("source.wav")));
		QVERIFY(published.value);

		const auto manifestPath = temp.filePath(QStringLiteral("vocal-asset-") + published.value->assetId
			+ QStringLiteral(".json"));
		const QByteArray tamperedManifest("{\"schema_version\":2}");
		QFile changed(manifestPath);
		QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Truncate));
		QCOMPARE(changed.write(tamperedManifest), qint64(tamperedManifest.size()));
		changed.close();
		QCOMPARE(store.inspect(*published.value).code, QStringLiteral("asset_integrity_failed"));
#endif
	}

	void createNewPublicationNeverReplacesAnExistingAsset()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(temp.path());
		QVERIFY(approval.root);
		ManagedVocalAssetStore store(approval.root);
		auto created = store.beginCreate(3, 48000, 1);
		QVERIFY(created.writer);
		const auto assetId = created.assetId;
		const std::array<float, 3> samples{0.0f, 0.25f, -0.5f};
		QCOMPARE(created.writer->appendFrames(samples), QStringLiteral("ok"));

		const auto collisionPath = temp.filePath(QStringLiteral("vocal-asset-") + assetId
			+ QStringLiteral(".wav"));
		const QByteArray preserved("existing asset");
		{
			QFile collision(collisionPath);
			QVERIFY(collision.open(QIODevice::WriteOnly | QIODevice::NewOnly));
			QCOMPARE(collision.write(preserved), qint64(preserved.size()));
		}
		const auto published = store.publish(std::move(created.writer),
			manifest(assetId, temp.filePath(QStringLiteral("source.wav"))));
		QCOMPARE(published.code, QStringLiteral("asset_exists"));
		QCOMPARE(contents(collisionPath), preserved);
		QVERIFY(!QFileInfo::exists(temp.filePath(QStringLiteral("vocal-asset-") + assetId
			+ QStringLiteral(".wav.partial"))));
#endif
	}

	void reportsRestartStagesWithoutDeletingThem()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		QString assetId;
		QString partialPath;
		{
			auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(temp.path());
			QVERIFY(approval.root);
			ManagedVocalAssetStore store(approval.root);
			auto created = store.beginCreate(3, 48000, 1);
			QVERIFY(created.writer);
			assetId = created.assetId;
			partialPath = temp.filePath(QStringLiteral("vocal-asset-") + assetId
				+ QStringLiteral(".wav.partial"));
			const std::array<float, 1> firstFrame{0.25f};
			QCOMPARE(created.writer->appendFrames(firstFrame), QStringLiteral("ok"));
			approval.root->revoke();
			created.writer.reset();
			QVERIFY(QFileInfo::exists(partialPath));
		}
		auto restartedRoot = ManagedVocalAssetRoot::authorizeLocalRoot(temp.path());
		QCOMPARE(restartedRoot.code, QStringLiteral("ok"));
		ManagedVocalAssetStore restartedStore(restartedRoot.root);
		const auto recovered = restartedStore.scanRecovery();
		QCOMPARE(recovered.code, QStringLiteral("ok"));
		QCOMPARE(recovered.totalItems, size_t(1));
		QVERIFY(!recovered.truncated);
		QCOMPARE(recovered.items.front().assetId, assetId);
		QCOMPARE(recovered.items.front().state, QStringLiteral("staging_recovery_required"));
		QVERIFY(recovered.items.front().stagedAudioPresent);
		QVERIFY(recovered.items.front().stagedAudioBytes > 56);
		QVERIFY(!recovered.items.front().finalAudioPresent);
		QVERIFY(QFileInfo::exists(partialPath));
#endif
	}

	void sharesExclusiveRootLockAndLeavesRecordingRecoveryNamespaceAlone()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#endif
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		auto recordingFirst = lmms::recording::RecordingLibrary::authorizeLocalRoot(temp.path());
		QVERIFY(recordingFirst.library);
		QCOMPARE(ManagedVocalAssetRoot::authorizeLocalRoot(temp.path()).code, QStringLiteral("asset_root_busy"));
		recordingFirst.library.reset();
		{
			auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(temp.path());
			QCOMPARE(approval.code, QStringLiteral("ok"));
			auto duplicate = ManagedVocalAssetRoot::authorizeLocalRoot(temp.path());
			QCOMPARE(duplicate.code, QStringLiteral("asset_root_busy"));
		}
		auto recording = lmms::recording::RecordingLibrary::authorizeLocalRoot(temp.path());
		QVERIFY(recording.library);
		lmms::recording::RecordingRecovery recovery;
		const auto page = recovery.scan(*recording.library);
		QCOMPARE(page.code, QStringLiteral("ok"));
		QVERIFY(page.takes.empty());
	}

	void rejectsReparseAndNetworkRoots()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#endif
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		QVERIFY(QDir(temp.path()).mkdir(QStringLiteral("target")));
		const auto target = temp.filePath(QStringLiteral("target"));
		const auto link = temp.filePath(QStringLiteral("junction"));
		JunctionCleanup cleanup{link};
		QVERIFY(createDirectoryJunction(link, target));
		QCOMPARE(ManagedVocalAssetRoot::authorizeLocalRoot(link).code, QStringLiteral("asset_root_denied"));
		QCOMPARE(ManagedVocalAssetRoot::authorizeLocalRoot(QStringLiteral("\\\\server\\share")).code,
			QStringLiteral("asset_root_denied"));
#endif
	}
};

QTEST_APPLESS_MAIN(ManagedVocalAssetStoreTest)
#include "ManagedVocalAssetStoreTest.moc"
