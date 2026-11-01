#include "MidiExportOutput.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QSet>
#include <QUuid>

#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winternl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lmms
{
namespace
{
bool validFilename(const QString& filename)
{
	if(filename.size()<5||filename.size()>180||!filename.endsWith(QStringLiteral(".mid"),Qt::CaseInsensitive))return false;
	if(filename==QStringLiteral(".")||filename==QStringLiteral("..")||filename.endsWith(QLatin1Char('.'))||filename.endsWith(QLatin1Char(' ')))return false;
	for(const auto character:filename)
		if(character.unicode()<0x20||QStringLiteral("<>:\"/\\|?*").contains(character))return false;
	const auto stem=filename.section(QLatin1Char('.'),0,0).toUpper();
	static const QSet<QString> reserved{QStringLiteral("CON"),QStringLiteral("PRN"),QStringLiteral("AUX"),QStringLiteral("NUL"),
		QStringLiteral("COM1"),QStringLiteral("COM2"),QStringLiteral("COM3"),QStringLiteral("COM4"),QStringLiteral("COM5"),QStringLiteral("COM6"),QStringLiteral("COM7"),QStringLiteral("COM8"),QStringLiteral("COM9"),
		QStringLiteral("LPT1"),QStringLiteral("LPT2"),QStringLiteral("LPT3"),QStringLiteral("LPT4"),QStringLiteral("LPT5"),QStringLiteral("LPT6"),QStringLiteral("LPT7"),QStringLiteral("LPT8"),QStringLiteral("LPT9")};
	return !reserved.contains(stem);
}

bool rootPath(const QString& requested,QString& canonical)
{
	const QFileInfo info(requested);
	if(!info.exists()||!info.isDir()||info.isSymLink())return false;
	canonical=QDir::cleanPath(info.canonicalFilePath());
	const auto absolute=QDir::cleanPath(info.absoluteFilePath());
	#ifdef Q_OS_WIN
	return !canonical.isEmpty()&&QString::compare(canonical,absolute,Qt::CaseInsensitive)==0;
	#else
	return !canonical.isEmpty()&&canonical==absolute;
	#endif
}

class HashingDevice : public QIODevice
{
public:
	explicit HashingDevice(QCryptographicHash& hash):m_hash(hash){}
	bool isSequential()const override{return true;}
protected:
	qint64 readData(char*,qint64)override{return -1;}
protected:
	QCryptographicHash& m_hash;
};

#ifdef Q_OS_WIN
struct WinRoot
{
	HANDLE directory=INVALID_HANDLE_VALUE;
	QString path;
	~WinRoot(){if(directory!=INVALID_HANDLE_VALUE)CloseHandle(directory);}
};

using NtCreateFileFunction=NTSTATUS (NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
using NtSetInformationFileFunction=NTSTATUS (NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);

HANDLE openRelative(HANDLE parent,const QString& name,ACCESS_MASK access,ULONG disposition,NTSTATUS& status)
{
	const auto module=GetModuleHandleW(L"ntdll.dll");
	const auto create=reinterpret_cast<NtCreateFileFunction>(GetProcAddress(module,"NtCreateFile"));
	if(!create){status=static_cast<NTSTATUS>(0xc0000002u);return INVALID_HANDLE_VALUE;}
	UNICODE_STRING text{};text.Buffer=const_cast<PWSTR>(reinterpret_cast<LPCWSTR>(name.utf16()));
	text.Length=USHORT(name.size()*sizeof(wchar_t));text.MaximumLength=text.Length;
	OBJECT_ATTRIBUTES attributes{};attributes.Length=sizeof(attributes);attributes.RootDirectory=parent;attributes.ObjectName=&text;
	attributes.Attributes=OBJ_DONT_REPARSE;
	IO_STATUS_BLOCK io{};HANDLE result=INVALID_HANDLE_VALUE;
	status=create(&result,access|SYNCHRONIZE,&attributes,&io,nullptr,FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ,disposition,
		FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,nullptr,0);
	return status>=0?result:INVALID_HANDLE_VALUE;
}

bool acquireRoot(const QString& requested,WinRoot& root)
{
	if(!rootPath(requested,root.path))return false;
	root.directory=CreateFileW(reinterpret_cast<LPCWSTR>(root.path.utf16()),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
	if(root.directory==INVALID_HANDLE_VALUE)return false;
	BY_HANDLE_FILE_INFORMATION info{};
	if(GetFileType(root.directory)!=FILE_TYPE_DISK||!GetFileInformationByHandle(root.directory,&info)
		||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
	std::array<wchar_t,2048> name{};
	const auto length=GetFinalPathNameByHandleW(root.directory,name.data(),DWORD(name.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
	if(!length||length>=name.size())return false;
	const auto pinned=QDir::cleanPath(QString::fromWCharArray(name.data(),int(length)).replace(QStringLiteral("\\\\?\\"),QString()));
	return QString::compare(pinned,root.path,Qt::CaseInsensitive)==0;
}

class WinDevice final:public HashingDevice
{
public:
	WinDevice(HANDLE file,QCryptographicHash& hash):HashingDevice(hash),m_file(file){}
	~WinDevice()override{if(m_file!=INVALID_HANDLE_VALUE)CloseHandle(m_file);}
	bool flush(){return FlushFileBuffers(m_file)!=FALSE;}
protected:
	qint64 writeData(const char* data,qint64 length)override
	{
		qint64 done=0;
		while(done<length){DWORD written=0;const auto chunk=DWORD(qMin<qint64>(length-done,1<<20));if(!WriteFile(m_file,data+done,chunk,&written,nullptr)||written==0)return -1;done+=written;}
		m_hash.addData(data,length);return done;
	}
private:
	HANDLE m_file;
};

struct WinCleanup
{
	HANDLE file=INVALID_HANDLE_VALUE;bool active=true;
	~WinCleanup(){if(active&&file!=INVALID_HANDLE_VALUE){const auto set=reinterpret_cast<NtSetInformationFileFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetInformationFile"));struct Info{BOOLEAN remove;};Info info{TRUE};IO_STATUS_BLOCK io{};if(set)set(file,&io,&info,sizeof(info),static_cast<FILE_INFORMATION_CLASS>(13));}}
};

bool publishWindows(HANDLE staged,HANDLE root,const QString& filename,bool overwrite,bool& destinationExists)
{
	NTSTATUS status=0;
	destinationExists=false;
	if(!overwrite)
	{
		auto existing=openRelative(root,filename,FILE_READ_ATTRIBUTES,FILE_OPEN,status);
		if(existing!=INVALID_HANDLE_VALUE){CloseHandle(existing);destinationExists=true;return false;}
		if(static_cast<ULONG>(status)!=0xc0000034u)return false;
	}
	if(overwrite)
	{
		auto existing=openRelative(root,filename,FILE_READ_ATTRIBUTES,FILE_OPEN,status);
		if(existing!=INVALID_HANDLE_VALUE){BY_HANDLE_FILE_INFORMATION info{};const bool ordinary=GetFileInformationByHandle(existing,&info)
			&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&info.nNumberOfLinks==1;CloseHandle(existing);if(!ordinary)return false;}
		else if(static_cast<ULONG>(status)!=0xc0000034u)return false;
		struct RenameInfo{BOOLEAN replace;HANDLE root;ULONG nameBytes;WCHAR name[1];};
		const auto bytes=offsetof(RenameInfo,name)+size_t(filename.size())*sizeof(WCHAR);
		std::vector<char> storage(bytes);auto* info=reinterpret_cast<RenameInfo*>(storage.data());
		info->replace=TRUE;info->root=root;info->nameBytes=ULONG(filename.size()*sizeof(WCHAR));std::memcpy(info->name,filename.utf16(),info->nameBytes);
		const auto set=reinterpret_cast<NtSetInformationFileFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetInformationFile"));
		IO_STATUS_BLOCK io{};return set&&set(staged,&io,info,ULONG(bytes),static_cast<FILE_INFORMATION_CLASS>(10))>=0;
	}
	struct LinkInfo{BOOLEAN replace;HANDLE root;ULONG nameBytes;WCHAR name[1];};
	const auto bytes=offsetof(LinkInfo,name)+size_t(filename.size())*sizeof(WCHAR);
	std::vector<char> storage(bytes);auto* info=reinterpret_cast<LinkInfo*>(storage.data());
	info->replace=FALSE;info->root=root;info->nameBytes=ULONG(filename.size()*sizeof(WCHAR));std::memcpy(info->name,filename.utf16(),info->nameBytes);
	const auto set=reinterpret_cast<NtSetInformationFileFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetInformationFile"));
	IO_STATUS_BLOCK io{};
	if(!set)return false;
	const auto linked=set(staged,&io,info,ULONG(bytes),static_cast<FILE_INFORMATION_CLASS>(11));
	if(linked<0)
	{
		if(static_cast<ULONG>(linked)==0xc0000035u)destinationExists=true;
		return false;
	}
	return true;
}
#else
struct PosixRoot
{
	int directory=-1;
	QString path;
	~PosixRoot(){if(directory>=0)::close(directory);}
};

bool acquireRoot(const QString& requested,PosixRoot& root)
{
	if(!rootPath(requested,root.path))return false;
	const auto encoded=QFile::encodeName(root.path);
	struct stat expected{};
	if(::stat(encoded.constData(),&expected)!=0||!S_ISDIR(expected.st_mode))return false;
	int current=::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
	if(current<0)return false;
	const auto components=root.path.split(QLatin1Char('/'),Qt::SkipEmptyParts);
	for(const auto& component:components)
	{
		const auto name=QFile::encodeName(component);
		const int next=::openat(current,name.constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
		::close(current);
		if(next<0)return false;
		current=next;
	}
	struct stat opened{};
	if(::fstat(current,&opened)!=0||!S_ISDIR(opened.st_mode)||opened.st_dev!=expected.st_dev||opened.st_ino!=expected.st_ino)
	{
		::close(current);
		return false;
	}
	root.directory=current;
	return true;
}

class PosixDevice final:public HashingDevice
{
public:
	PosixDevice(int file,QCryptographicHash& hash):HashingDevice(hash),m_file(file){}
	~PosixDevice()override{if(m_file>=0)::close(m_file);}
	bool flush(){return ::fsync(m_file)==0;}
protected:
	qint64 writeData(const char* data,qint64 length)override
	{
		qint64 done=0;while(done<length){const auto count=::write(m_file,data+done,size_t(length-done));if(count<=0)return -1;done+=count;}
		m_hash.addData(data,length);return done;
	}
private:int m_file;
};

struct PosixCleanup
{
	int root=-1,staging=-1;QByteArray directoryName,fileName;
	~PosixCleanup(){if(staging>=0){::unlinkat(staging,fileName.constData(),0);::close(staging);}if(root>=0&&!directoryName.isEmpty())::unlinkat(root,directoryName.constData(),AT_REMOVEDIR);}
};
#endif

MidiExportFileResult failure(const QString& code){return {false,code,{},{},0};}
}

MidiExportFileResult writeMidiExportFile(const QString& authorizedRoot,const QString& filename,bool overwrite,const MidiExportPlan& plan,
	const std::atomic_bool* cancelled,const std::function<bool()>& acquirePublishPermit)
{
	if(cancelled&&cancelled->load(std::memory_order_acquire))return failure(QStringLiteral("cancelled"));
	if(!validFilename(filename))return failure(QStringLiteral("destination_denied"));
	QCryptographicHash hash(QCryptographicHash::Sha256);
	const auto temporary=QStringLiteral(".midi-export-%1.partial").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
#ifdef Q_OS_WIN
	WinRoot root;if(!acquireRoot(authorizedRoot,root))return failure(QStringLiteral("destination_denied"));
	NTSTATUS status=0;HANDLE file=openRelative(root.directory,temporary,GENERIC_WRITE|FILE_READ_ATTRIBUTES|DELETE,FILE_CREATE,status);
	if(file==INVALID_HANDLE_VALUE)return failure(QStringLiteral("write_failed"));
	WinDevice device(file,hash);device.open(QIODevice::WriteOnly);
	if(!device.isOpen())
	{
		WinCleanup cleanup{file};
		return failure(QStringLiteral("write_failed"));
	}
	WinCleanup cleanup{file};
#else
	PosixRoot root;if(!acquireRoot(authorizedRoot,root))return failure(QStringLiteral("destination_denied"));
	const auto directoryBytes=QFile::encodeName(temporary);
	if(::mkdirat(root.directory,directoryBytes.constData(),0700)!=0)return failure(QStringLiteral("write_failed"));
	const int staging=::openat(root.directory,directoryBytes.constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
	if(staging<0){::unlinkat(root.directory,directoryBytes.constData(),AT_REMOVEDIR);return failure(QStringLiteral("write_failed"));}
	const QByteArray stagedName=QByteArrayLiteral("payload.mid");
	PosixCleanup cleanup{root.directory,staging,directoryBytes,stagedName};
	const int file=::openat(staging,stagedName.constData(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
	if(file<0)return failure(QStringLiteral("write_failed"));
	PosixDevice device(file,hash);device.open(QIODevice::WriteOnly);
#endif
	const auto written=writeStandardMidi(plan,device,cancelled);
	if(!written.success)return failure(written.code);
	if(!device.flush())return failure(QStringLiteral("write_failed"));
	device.close();
	if((cancelled&&cancelled->load(std::memory_order_acquire))||(acquirePublishPermit&&!acquirePublishPermit()))return failure(QStringLiteral("cancelled"));
	const auto digest=QString::fromLatin1(hash.result().toHex());
#ifdef Q_OS_WIN
	// Reopen by the pinned directory handle; a final symlink/reparse point is rejected.
	bool destinationExists=false;
	if(!publishWindows(file,root.directory,filename,overwrite,destinationExists))return failure(destinationExists?QStringLiteral("destination_exists"):QStringLiteral("publication_failed"));
	if(!overwrite){const auto set=reinterpret_cast<NtSetInformationFileFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetInformationFile"));struct DispositionInfo{BOOLEAN remove;};DispositionInfo remove{TRUE};IO_STATUS_BLOCK io{};if(set)set(file,&io,&remove,sizeof(remove),static_cast<FILE_INFORMATION_CLASS>(13));}
	cleanup.active=false;
#else
	const auto target=QFile::encodeName(filename);
	if(overwrite)
	{
		struct stat existing{};
		if(::fstatat(root.directory,target.constData(),&existing,AT_SYMLINK_NOFOLLOW)==0)
		{
			if(!S_ISREG(existing.st_mode)||existing.st_nlink!=1)return failure(QStringLiteral("destination_denied"));
		}
		else if(errno!=ENOENT)return failure(QStringLiteral("publication_failed"));
	}
	const int published=overwrite?::renameat(staging,stagedName.constData(),root.directory,target.constData())
		: ::linkat(staging,stagedName.constData(),root.directory,target.constData(),0);
	if(published!=0)return failure(errno==EEXIST?QStringLiteral("destination_exists"):QStringLiteral("publication_failed"));
	if(::fsync(root.directory)!=0)return failure(QStringLiteral("publication_failed"));
#endif
	return {true,QStringLiteral("ok"),QDir(root.path).filePath(filename),digest,written.bytesWritten};
}

} // namespace lmms
