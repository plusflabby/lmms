#include <QtTest>
#include <QFile>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QProcess>
#include <thread>
#include <atomic>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include "AudioFileAuthorization.h"
#include "SampleDecoder.h"

class AudioFileAuthorizationTest : public QObject
{
	Q_OBJECT
private slots:
	void decodesOnlyTheAuthorizedReadOnlyHandle()
	{
		const auto path = QFileInfo(QFINDTESTDATA("../../../data/samples/shapes/vowel_u.wav")).canonicalFilePath();
		const auto root = QFileInfo(path).absolutePath();
		QFile original(path);
		QVERIFY(original.open(QIODevice::ReadOnly));
		const auto digest = QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256);
		auto result = lmms::AudioFileAuthorization::acquire(root, path);
		QCOMPARE(result.code, QStringLiteral("ok"));
		QVERIFY(result.source);
		QVERIFY(!result.identity.isEmpty());
		QVERIFY(!result.source->isWritable());
		auto decoded = lmms::SampleDecoder::decodePcmWave(*result.source, {20000, 20000});
		QCOMPARE(decoded.code, QStringLiteral("ok"));
		QVERIFY(decoded.sample);
		QCOMPARE(decoded.sample->data.size(), size_t(337));
		QCOMPARE(decoded.info.sourceSha256, digest);
		QVERIFY(original.seek(0));
		QCOMPARE(QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256), digest);
	}
	void rejectsNoncanonicalAndEscapingPaths()
	{
		const auto path = QFINDTESTDATA("../../../data/samples/shapes/vowel_u.wav");
		const auto root = QFileInfo(path).absolutePath();
		QCOMPARE(lmms::AudioFileAuthorization::acquire(QString{}, path).code, QStringLiteral("import_root_not_configured"));
		const QStringList denied = {"vowel_u.wav", "C:relative.wav", "file:///C:/v.wav", "//server/share/v.wav",
			"\\\\?\\C:\\v.wav", "\\\\.\\C:\\v.wav", root + "/../shapes/vowel_u.wav", path + ":stream",
			root, root + "/vowel_u.wav.", root + "/vowel_u.wav ", root + "/CON", root + "/a//b.wav",
			QStringLiteral("C:/") + QString(1022, QChar('a'))};
		for (const auto& candidate : denied)
		{
			auto result = lmms::AudioFileAuthorization::acquire(root, candidate);
			QCOMPARE(result.code, QStringLiteral("import_path_denied"));
			QVERIFY(!result.source);
		}
		// The candidate begins with this string, but not at a directory boundary.
		QCOMPARE(lmms::AudioFileAuthorization::acquire(root.left(root.size() - 1), QFileInfo(path).canonicalFilePath()).code,
			QStringLiteral("import_path_denied"));
		QCOMPARE(lmms::AudioFileAuthorization::acquire(root, root + "/absent.wav").code, QStringLiteral("audio_source_missing"));
	}
#ifdef Q_OS_WIN
	void rejectsAnExistingWritableFileMapping()
	{
		QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/auth-XXXXXX");
		QVERIFY(directory.isValid());
		const auto path = directory.filePath("mapped.txt");
		{ QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("original"), qint64(8)); }
		const auto writer = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		QVERIFY(writer != INVALID_HANDLE_VALUE);
		const auto mapping = CreateFileMappingW(writer, nullptr, PAGE_READWRITE, 0, 0, nullptr);
		QVERIFY(mapping);
		auto* view = static_cast<char*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 8));
		QVERIFY(view);
		CloseHandle(writer);
		auto acquired = lmms::AudioFileAuthorization::acquire(directory.path(), path);
		UnmapViewOfFile(view); CloseHandle(mapping);
		QCOMPARE(acquired.code, QStringLiteral("import_path_denied"));
		QVERIFY(!acquired.source);
	}
	void canonicalCaseAndAlternateNamespacesCannotBroadenTheRoot()
	{
		QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/auth-XXXXXX");
		QVERIFY(directory.isValid());
		QVERIFY(QDir(directory.path()).mkdir("Approved"));
		QVERIFY(QDir(directory.path()).mkdir("Approved-other"));
		const auto root = directory.filePath("Approved");
		const auto file = root + "/Source.txt";
		{ QFile f(file); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); f.write("allowed"); }
		const auto outside = directory.filePath("Approved-other/Source.txt");
		{ QFile f(outside); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); f.write("outside"); }
		auto approval = lmms::AudioFileAuthorization::approveRoot(root);
		QCOMPARE(approval.code, QStringLiteral("ok"));
		QCOMPARE(lmms::AudioFileAuthorization::acquire(*approval.root, file).code, QStringLiteral("ok"));
		for (const auto& candidate : QStringList{outside, root + "/source.txt", file + ":$DATA", "//?/" + file,
			root + "/./Source.txt", QStringLiteral("//localhost/C$/Source.txt")})
			QCOMPARE(lmms::AudioFileAuthorization::acquire(*approval.root, candidate).code, QStringLiteral("import_path_denied"));
		QCOMPARE(lmms::AudioFileAuthorization::approveRoot(root.left(3)).code, QStringLiteral("import_path_denied"));
	}
	void concurrentDirectoryReplacementNeverReadsOutsideTheRoot()
	{
		QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/auth-XXXXXX");
		QVERIFY(directory.isValid());
		QVERIFY(QDir(directory.path()).mkdir("approved"));
		QVERIFY(QDir(directory.path()).mkdir("outside"));
		const auto root = directory.filePath("approved");
		QVERIFY(QDir(root).mkdir("child"));
		const auto child = root + "/child";
		const auto parked = root + "/parked";
		const auto outside = directory.filePath("outside");
		for (const auto& item : QList<QPair<QString, QByteArray>>{{child + "/source.txt", "allowed"}, {outside + "/source.txt", "outside"}})
		{ QFile f(item.first); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write(item.second), qint64(item.second.size())); }
		std::atomic<int> swaps{0};
		std::atomic<bool> started{false};
		std::jthread attacker([&](std::stop_token stop) {
			while (!stop.stop_requested())
			{
				if (!MoveFileExW(reinterpret_cast<LPCWSTR>(child.utf16()), reinterpret_cast<LPCWSTR>(parked.utf16()), 0)) continue;
				if (CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(child.utf16()), reinterpret_cast<LPCWSTR>(outside.utf16()),
					SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
				{
					++swaps; started = true;
					std::this_thread::yield();
					RemoveDirectoryW(reinterpret_cast<LPCWSTR>(child.utf16()));
				}
				MoveFileExW(reinterpret_cast<LPCWSTR>(parked.utf16()), reinterpret_cast<LPCWSTR>(child.utf16()), 0);
			}
		});
		QElapsedTimer timer; timer.start();
		while (!started && timer.elapsed() < 3000) std::this_thread::yield();
		QVERIFY(started);
		for (int i = 0; i < 300; ++i)
		{
			auto result = lmms::AudioFileAuthorization::acquire(root, child + "/source.txt");
			if (result.source) { QCOMPARE(result.code, QStringLiteral("ok")); QCOMPARE(result.source->readAll(), QByteArray("allowed")); }
			else QVERIFY(result.code == "import_path_denied" || result.code == "audio_source_missing");
		}
		attacker.request_stop(); attacker.join();
		QVERIFY(swaps > 0);
		// Restore only this fixture's entries after the racing handles are gone.
		const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(child.utf16()));
		if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
			QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(child.utf16())));
		if (QFileInfo::exists(parked))
			QVERIFY(MoveFileExW(reinterpret_cast<LPCWSTR>(parked.utf16()), reinterpret_cast<LPCWSTR>(child.utf16()), 0));
	}
	void deniesNetworkReparseTargetsWithoutFollowingThem()
	{
		QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/auth-XXXXXX");
		QVERIFY(directory.isValid());
		const auto link = directory.filePath("network");
		QVERIFY(CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(link.utf16()), L"\\\\nonexistent-phase3i.invalid\\recordings",
			SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));
		QCOMPARE(lmms::AudioFileAuthorization::acquire(directory.path(), link + "/source.wav").code, QStringLiteral("import_path_denied"));
		QCOMPARE(lmms::AudioFileAuthorization::approveRoot(link).code, QStringLiteral("import_path_denied"));
		QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16())));
	}
	void approvedRootLifetimeAndRevocationBindFutureOpens()
	{
		QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/auth-XXXXXX");
		QVERIFY(directory.isValid());
		QVERIFY(QDir(directory.path()).mkdir("approved"));
		const auto root = directory.filePath("approved");
		const auto path = root + "/source.txt";
		{ QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(f.write("approved"), qint64(8)); }
		auto approval = lmms::AudioFileAuthorization::approveRoot(root);
		QCOMPARE(approval.code, QStringLiteral("ok"));
		QVERIFY(approval.root);
		QVERIFY(!MoveFileExW(reinterpret_cast<LPCWSTR>(root.utf16()),
			reinterpret_cast<LPCWSTR>(directory.filePath("replacement").utf16()), 0));
		auto acquired = lmms::AudioFileAuthorization::acquire(*approval.root, path);
		QCOMPARE(acquired.code, QStringLiteral("ok"));
		approval.root->revoke();
		QCOMPARE(lmms::AudioFileAuthorization::acquire(*approval.root, path).code, QStringLiteral("import_path_denied"));
		// Revocation prohibits new leases; an already authorized lease keeps its original bytes.
		QCOMPARE(acquired.source->readAll(), QByteArray("approved"));
		approval.root.reset(); acquired.source.reset();
		QVERIFY(MoveFileExW(reinterpret_cast<LPCWSTR>(root.utf16()),
			reinterpret_cast<LPCWSTR>(directory.filePath("replacement").utf16()), 0));
	}
	void preventsWritersAndReplacementWhileDecoding()
	{
		QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/auth-XXXXXX");
		QVERIFY(directory.isValid());
		const auto path = directory.filePath("source.txt");
		{ QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("original"), qint64(8)); }
		auto result = lmms::AudioFileAuthorization::acquire(directory.path(), path);
		QCOMPARE(result.code, QStringLiteral("ok"));
		auto writer = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (writer != INVALID_HANDLE_VALUE) { CloseHandle(writer); QFAIL("Authorized source allowed a concurrent writer"); }
		QCOMPARE(GetLastError(), DWORD(ERROR_SHARING_VIOLATION));
		QVERIFY(!MoveFileExW(reinterpret_cast<LPCWSTR>(path.utf16()),
			reinterpret_cast<LPCWSTR>(directory.filePath("renamed.txt").utf16()), MOVEFILE_REPLACE_EXISTING));
		QCOMPARE(result.source->readAll(), QByteArray("original"));
		result.source.reset();
		writer = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		QVERIFY(writer != INVALID_HANDLE_VALUE);
		auto busy = lmms::AudioFileAuthorization::acquire(directory.path(), path);
		CloseHandle(writer);
		QCOMPARE(busy.code, QStringLiteral("import_path_denied"));
		QVERIFY(!busy.source);
	}
	void rejectsFileAndDirectorySymbolicLinksBeforeFollowingThem()
	{
		QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/auth-XXXXXX");
		QVERIFY(directory.isValid());
		const auto path = QFINDTESTDATA("../../../data/samples/shapes/vowel_u.wav");
		const auto link = directory.filePath("escape.wav");
		const auto created = CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(link.utf16()), reinterpret_cast<LPCWSTR>(path.utf16()),
			SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
		const auto creationError = GetLastError();
		QVERIFY2(created, qPrintable(QStringLiteral("Cannot create Windows symlink fixture; Win32 error %1").arg(creationError)));
		QCOMPARE(lmms::AudioFileAuthorization::acquire(directory.path(), link).code, QStringLiteral("import_path_denied"));
		const auto dirLink = directory.filePath("escape-dir");
		QVERIFY2(CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(dirLink.utf16()),
			reinterpret_cast<LPCWSTR>(QFileInfo(path).absolutePath().utf16()),
			SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE), "Windows directory symlink fixture requires privilege");
		QCOMPARE(lmms::AudioFileAuthorization::acquire(directory.path(), dirLink + "/vowel_u.wav").code, QStringLiteral("import_path_denied"));
		// Remove only links created above, never their targets.
		QVERIFY(DeleteFileW(reinterpret_cast<LPCWSTR>(link.utf16())));
		QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(dirLink.utf16())));
	}
	void rejectsJunctionEscapesAndJunctionApprovedRoots()
	{
		QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/auth-XXXXXX");
		QVERIFY(directory.isValid());
		const auto target = QFileInfo(QFINDTESTDATA("../../../data/samples/shapes/vowel_u.wav")).absolutePath();
		const auto junction = directory.filePath("junction");
		// Fixture creation only: no shell invocation exists in production acquisition.
		QCOMPARE(QProcess::execute(QStringLiteral("C:/Windows/System32/cmd.exe"),
			{"/d", "/c", "mklink", "/J", QDir::toNativeSeparators(junction), QDir::toNativeSeparators(target)}), 0);
		QCOMPARE(lmms::AudioFileAuthorization::acquire(directory.path(), junction + "/vowel_u.wav").code,
			QStringLiteral("import_path_denied"));
		QCOMPARE(lmms::AudioFileAuthorization::acquire(junction, junction + "/vowel_u.wav").code,
			QStringLiteral("import_path_denied"));
		QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(junction.utf16())));
	}
	void pinsDirectoryAndFinalFileIdentityUntilReleased()
	{
		QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/auth-XXXXXX");
		QVERIFY(directory.isValid());
		QVERIFY(QDir(directory.path()).mkdir("approved"));
		const auto root = directory.filePath("approved");
		const auto path = root + "/source.txt";
		{ QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("first"), qint64(5)); }
		auto first = lmms::AudioFileAuthorization::acquire(root, path);
		QCOMPARE(first.code, QStringLiteral("ok"));
		auto second = lmms::AudioFileAuthorization::acquire(root, path);
		QCOMPARE(second.code, QStringLiteral("ok"));
		QCOMPARE(first.identity, second.identity);
		QVERIFY(!MoveFileExW(reinterpret_cast<LPCWSTR>(root.utf16()),
			reinterpret_cast<LPCWSTR>(directory.filePath("moved").utf16()), 0));
		first.source.reset(); second.source.reset();
		QVERIFY(QFile::rename(path, root + "/old.txt"));
		{ QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(file.write("second"), qint64(6)); }
		auto replaced = lmms::AudioFileAuthorization::acquire(root, path);
		QCOMPARE(replaced.code, QStringLiteral("ok"));
		QVERIFY(replaced.identity != first.identity);
		QCOMPARE(replaced.source->readAll(), QByteArray("second"));
	}
#endif
};
QTEST_GUILESS_MAIN(AudioFileAuthorizationTest)
#include "AudioFileAuthorizationTest.moc"
