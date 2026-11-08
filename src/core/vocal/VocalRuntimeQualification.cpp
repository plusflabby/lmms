#include "VocalRuntimeQualification.h"
#include "lmmsconfig.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QThread>
#include <array>
#include <samplerate.h>

#if defined(Q_OS_WIN) && defined(LMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME)
#include <windows.h>
#endif

namespace lmms {
namespace {
bool inspectLoadedRuntime()
{
#if defined(Q_OS_WIN) && defined(LMMS_REQUIRE_QUALIFIED_VOCAL_RUNTIME)
	// The returned version literal is inside the loaded DLL even when MinGW
	// exposes an import thunk for the function address.
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(src_get_version()), &module)) return false;
	std::array<wchar_t, 32768> path{};
	const auto length = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
	if (!length || length >= path.size()) return false;
	QFile file(QString::fromWCharArray(path.data(), int(length)));
	if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 8 * 1024 * 1024) return false;
	QCryptographicHash hash(QCryptographicHash::Sha256);
	return hash.addData(&file) && hash.result() == QByteArray::fromHex(
		"bf275a0636dc7dadac8a691842c06b460109efc213c68b5ef6576a216e55d734");
#else
	return false;
#endif
}
}

bool qualifiedVocalSamplerateRuntime()
{
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()) return false;
	static const bool qualified = inspectLoadedRuntime();
	return qualified;
}
}
