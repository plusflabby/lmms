#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QDir>
#include <QUuid>
#include <array>
#include <cmath>
#include <limits>
#include <cstring>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#endif
#include "ManagedVocalAssetRoot.h"
#include "ManagedVocalAssetStore.h"
#include "ManagedVocalProjectOwnerStore.h"
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

	static ManagedVocalManifest manifest(const QString& assetId, const QString& sourcePath,
		double outputRms = std::sqrt(1.8125 / 3.0))
	{
		ManagedVocalManifest value;
		value.assetId = assetId;
		value.projectId = QStringLiteral("11111111-1111-4111-8111-111111111111");
		value.assetRootId = digest('e');
		value.media = {3, 48000, 1, digest('0'), 0.0, false};
		value.media.rms = outputRms;
		value.media.rmsKnown = true;
		value.recipeHash = digest('a');
		value.rendererId = QStringLiteral("native-float-renderer");
		value.rendererVersion = QStringLiteral("1");
		value.recipe = QJsonObject{{"recipe_version", 1}, {"operation", "gain_envelope"}};
		value.sources.push_back({"clip-a", digest('b'), digest('c'), sourcePath, {0, 3}, 3, 48000, 1,
			ManagedVocalSourceBasis::AuthorizedPcm, {}, {}});
		return value;
	}

private slots:
	void projectOwnerBindsSavedFileAndRejectsChangedOrCopiedFiles()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed vocal project ownership requires Windows file identities");
#else
		QTemporaryDir rootDir, projectDir;
		QVERIFY(rootDir.isValid() && projectDir.isValid());
		const auto original = projectDir.filePath(QStringLiteral("project.mmp"));
		const QByteArray document("synthetic saved project");
		{ QFile file(original); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write(document), qint64(document.size())); }
		auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(rootDir.path());
		QVERIFY(approval.root);
		ManagedVocalProjectOwnerStore store(approval.root);
		QCOMPARE(store.lookup(original).code, QStringLiteral("project_identity_unverified"));
		const auto first = store.ensure(original);
		QCOMPARE(first.code, QStringLiteral("ok"));
		QVERIFY(first.owner);
		QCOMPARE(first.owner->rootId, approval.root->rootId());
		const auto found = store.lookup(original);
		QVERIFY(found.owner);
		QCOMPARE(found.owner->projectId, first.owner->projectId);
		QCOMPARE(store.verify(original, *first.owner), QStringLiteral("ok"));
		const auto copied = projectDir.filePath(QStringLiteral("copy.mmp"));
		QVERIFY(QFile::copy(original, copied));
		QCOMPARE(store.lookup(copied).code, QStringLiteral("project_identity_unverified"));
		QCOMPARE(store.verify(copied, *first.owner), QStringLiteral("project_identity_unverified"));
		const auto other = store.ensure(copied);
		QVERIFY(other.owner);
		QVERIFY(other.owner->projectId != first.owner->projectId);
		{ QFile file(original); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write("changed"), qint64(7)); }
		QCOMPARE(store.verify(original, *first.owner), QStringLiteral("project_identity_unverified"));
		QCOMPARE(store.lookup(original).code, QStringLiteral("project_identity_unverified"));
		QCOMPARE(contents(copied), document);
		approval.root->revoke();
		QCOMPARE(store.verify(copied, *other.owner), QStringLiteral("asset_root_revoked"));
#endif
	}

	void projectOwnerRejectsMissingOrHardlinkedProject()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed vocal project ownership requires Windows file identities");
#else
		QTemporaryDir rootDir, projectDir;
		QVERIFY(rootDir.isValid() && projectDir.isValid());
		auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(rootDir.path());
		QVERIFY(approval.root);
		ManagedVocalProjectOwnerStore store(approval.root);
		QCOMPARE(store.ensure(QString{}).code, QStringLiteral("project_identity_unverified"));
		QCOMPARE(store.ensure(projectDir.filePath(QStringLiteral("missing.mmp"))).code, QStringLiteral("project_identity_unverified"));
		const auto original = projectDir.filePath(QStringLiteral("project.mmp"));
		{ QFile file(original); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("safe"), qint64(4)); }
		const auto link = projectDir.filePath(QStringLiteral("hardlink.mmp"));
		QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(link.utf16()), reinterpret_cast<LPCWSTR>(original.utf16()), nullptr));
		QCOMPARE(store.ensure(original).code, QStringLiteral("project_identity_unverified"));
		QCOMPARE(store.ensure(link).code, QStringLiteral("project_identity_unverified"));
#endif
	}

	void projectOwnerSurvivesRootReopenAndRejectsCorruptBinding()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed vocal project ownership requires Windows file identities");
#else
		QTemporaryDir rootDir, projectDir;
		QVERIFY(rootDir.isValid() && projectDir.isValid());
		const auto project = projectDir.filePath(QStringLiteral("project.mmp"));
		{ QFile file(project); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("project"), qint64(7)); }
		QString ownerId;
		{
			auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(rootDir.path());
			QVERIFY(approval.root);
			ManagedVocalProjectOwnerStore store(approval.root);
			const auto created = store.ensure(project);
			QVERIFY(created.owner);
			ownerId = created.owner->projectId;
		}
		auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(rootDir.path());
		QVERIFY(approval.root);
		ManagedVocalProjectOwnerStore store(approval.root);
		const auto reopened = store.lookup(project);
		QVERIFY(reopened.owner);
		QCOMPARE(reopened.owner->projectId, ownerId);
		const auto records = QDir(rootDir.path()).entryList({QStringLiteral("vocal-project-*.json")}, QDir::Files);
		QCOMPARE(records.size(), 1);
		const auto recordPath = rootDir.filePath(records.front());
		{ QFile file(recordPath); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write("{}"), qint64(2)); }
		QCOMPARE(store.lookup(project).code, QStringLiteral("asset_integrity_failed"));
		QCOMPARE(store.ensure(project).code, QStringLiteral("asset_integrity_failed"));
		QCOMPARE(contents(recordPath), QByteArray("{}"));
#endif
	}

	void projectOwnerVerificationRejectsWellFormedOwnerSubstitution()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed vocal project ownership requires Windows file identities");
#else
		QTemporaryDir rootDir, projectDir;
		QVERIFY(rootDir.isValid() && projectDir.isValid());
		const auto project = projectDir.filePath(QStringLiteral("project.mmp"));
		{ QFile file(project); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("project"), qint64(7)); }
		auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(rootDir.path());
		QVERIFY(approval.root);
		ManagedVocalProjectOwnerStore store(approval.root);
		const auto original = store.ensure(project);
		QVERIFY(original.owner);
		const auto records = QDir(rootDir.path()).entryList({QStringLiteral("vocal-project-*.json")}, QDir::Files);
		QCOMPARE(records.size(), 1);
		const auto recordPath = rootDir.filePath(records.front());
		auto object = QJsonDocument::fromJson(contents(recordPath)).object();
		object.insert(QStringLiteral("project_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
		const auto forged = QJsonDocument(object).toJson(QJsonDocument::Compact);
		{ QFile file(recordPath); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(forged), qint64(forged.size())); }
		QCOMPARE(store.verify(project, *original.owner), QStringLiteral("project_identity_unverified"));
		QCOMPARE(contents(recordPath), forged);
#endif
	}

	void projectOwnerRejectsReparseAncestor()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed vocal project ownership requires Windows file identities");
#else
		QTemporaryDir rootDir, projectDir, parentDir;
		QVERIFY(rootDir.isValid() && projectDir.isValid() && parentDir.isValid());
		const auto project = projectDir.filePath(QStringLiteral("project.mmp"));
		{ QFile file(project); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("project"), qint64(7)); }
		JunctionCleanup junction{parentDir.filePath(QStringLiteral("redirect"))};
		QVERIFY(createDirectoryJunction(junction.path, projectDir.path()));
		auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(rootDir.path());
		QVERIFY(approval.root);
		ManagedVocalProjectOwnerStore store(approval.root);
		QCOMPARE(store.ensure(junction.path + QStringLiteral("/project.mmp")).code,
			QStringLiteral("project_identity_unverified"));
#endif
	}

	void projectOwnerRejectsReplacementWithIdenticalBytes()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed vocal project ownership requires Windows file identities");
#else
		QTemporaryDir rootDir, projectDir;
		QVERIFY(rootDir.isValid() && projectDir.isValid());
		const auto project = projectDir.filePath(QStringLiteral("project.mmp"));
		{ QFile file(project); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("project"), qint64(7)); }
		auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(rootDir.path());
		QVERIFY(approval.root);
		ManagedVocalProjectOwnerStore store(approval.root);
		const auto first = store.ensure(project);
		QVERIFY(first.owner);
		const auto replacement = projectDir.filePath(QStringLiteral("replacement.mmp"));
		{ QFile file(replacement); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("project"), qint64(7)); }
		QVERIFY(QFile::remove(project));
		QVERIFY(QFile::rename(replacement, project));
		QCOMPARE(store.verify(project, *first.owner), QStringLiteral("project_identity_unverified"));
		QCOMPARE(store.lookup(project).code, QStringLiteral("project_identity_unverified"));
#endif
	}

	void projectOwnerSupportsProjectInsideManagedRoot()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed vocal project ownership requires Windows file identities");
#else
		QTemporaryDir rootDir;
		QVERIFY(rootDir.isValid());
		const auto project = rootDir.filePath(QStringLiteral("project.mmp"));
		{ QFile file(project); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("project"), qint64(7)); }
		auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(rootDir.path());
		QVERIFY(approval.root);
		ManagedVocalProjectOwnerStore store(approval.root);
		const auto owner = store.ensure(project);
		QCOMPARE(owner.code, QStringLiteral("ok"));
		QVERIFY(owner.owner);
		QCOMPARE(store.verify(project, *owner.owner), QStringLiteral("ok"));
#endif
	}

	void projectOwnerSupportsNestedProjectInsideManagedRoot()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed vocal project ownership requires Windows file identities");
#else
		QTemporaryDir rootDir, outsideDir;
		QVERIFY(rootDir.isValid() && outsideDir.isValid());
		QVERIFY(QDir().mkpath(rootDir.filePath(QStringLiteral("projects/nested"))));
		const auto project = rootDir.filePath(QStringLiteral("projects/nested/vocal take.mmp"));
		{ QFile file(project); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("project"), qint64(7)); }
		const auto outside = outsideDir.filePath(QStringLiteral("elsewhere.mmp"));
		{ QFile file(outside); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("outside"), qint64(7)); }
		const auto hardlink = rootDir.filePath(QStringLiteral("projects/nested/hardlink.mmp"));
		QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(hardlink.utf16()), reinterpret_cast<LPCWSTR>(outside.utf16()), nullptr));
		JunctionCleanup junction{rootDir.filePath(QStringLiteral("projects/redirect"))};
		QVERIFY(createDirectoryJunction(junction.path, outsideDir.path()));
		auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(rootDir.path());
		QVERIFY(approval.root);
		ManagedVocalProjectOwnerStore store(approval.root);
		const auto owner = store.ensure(project);
		QCOMPARE(owner.code, QStringLiteral("ok"));
		QVERIFY(owner.owner);
		QCOMPARE(store.verify(project, *owner.owner), QStringLiteral("ok"));
		QCOMPARE(store.ensure(junction.path + QStringLiteral("/elsewhere.mmp")).code,
			QStringLiteral("project_identity_unverified"));
		QCOMPARE(store.ensure(hardlink).code, QStringLiteral("project_identity_unverified"));
#endif
	}
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
		QCOMPARE(inspection.assetId, published.value->assetId);
		QVERIFY(!inspection.mediaSha256.isEmpty());
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

	void inspectsAStoredVariantByStableIdWithinTheExpectedProject()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir temp;
		QVERIFY(temp.isValid());
		auto approval=ManagedVocalAssetRoot::authorizeLocalRoot(temp.path());
		QVERIFY(approval.root);
		ManagedVocalAssetStore store(approval.root);
		auto created=store.beginCreate(3,48000,1);
		QVERIFY(created.writer);
		const std::array<float,3> samples{0.0f,0.25f,-0.5f};
		QCOMPARE(created.writer->appendFrames(samples),QStringLiteral("ok"));
		const auto projectId=QStringLiteral("11111111-1111-4111-8111-111111111111");
		auto source=manifest(created.assetId,temp.filePath(QStringLiteral("lead.wav")),std::sqrt(0.3125/3.0));
		auto published=store.publish(std::move(created.writer),std::move(source));
		QVERIFY(published.value);

		const auto inspected=store.inspectById(published.value->assetId,projectId);
		QCOMPARE(inspected.code,QStringLiteral("ok"));
		QCOMPARE(inspected.projectId,projectId);
		QCOMPARE(inspected.reference.assetId,published.value->assetId);
		QCOMPARE(inspected.sources.size(),size_t(1));
		QCOMPARE(store.inspectById(published.value->assetId,QStringLiteral("22222222-2222-4222-8222-222222222222")).code,
			QStringLiteral("asset_project_mismatch"));
		QCOMPARE(store.inspectById(QStringLiteral("00000000-0000-4000-8000-000000000001"),projectId).code,
			QStringLiteral("asset_missing"));
#endif
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
		auto published = store.publish(std::move(created.writer), manifest(created.assetId,
			temp.filePath("source.wav"), std::sqrt(0.3125 / 3.0)));
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
			manifest(assetId, temp.filePath(QStringLiteral("source.wav")), std::sqrt(0.3125 / 3.0)));
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
		const auto project = temp.filePath(QStringLiteral("project.mmp"));
		{ QFile file(project); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("project"), qint64(7)); }
		auto recordingFirst = lmms::recording::RecordingLibrary::authorizeLocalRoot(temp.path());
		QVERIFY(recordingFirst.library);
		QCOMPARE(ManagedVocalAssetRoot::authorizeLocalRoot(temp.path()).code, QStringLiteral("asset_root_busy"));
		recordingFirst.library.reset();
		{
			auto approval = ManagedVocalAssetRoot::authorizeLocalRoot(temp.path());
			QCOMPARE(approval.code, QStringLiteral("ok"));
			auto duplicate = ManagedVocalAssetRoot::authorizeLocalRoot(temp.path());
			QCOMPARE(duplicate.code, QStringLiteral("asset_root_busy"));
			ManagedVocalProjectOwnerStore ownerStore(approval.root);
			QCOMPARE(ownerStore.ensure(project).code, QStringLiteral("ok"));
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
