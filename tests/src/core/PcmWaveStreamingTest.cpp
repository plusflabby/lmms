#include <QtTest>
#include <QBuffer>
#include <QCryptographicHash>
#include <QtEndian>
#include "SampleDecoder.h"
#include <cstring>
#include <cstdlib>
#include <new>

namespace {
thread_local bool measureAllocations=false;
thread_local size_t largestAllocation=0;
}
void* operator new(std::size_t size)
{
	if(measureAllocations)largestAllocation=std::max(largestAllocation,size);
	if(auto* p=std::malloc(size ? size : 1))return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}

using namespace lmms;
class PcmWaveStreamingTest : public QObject
{
	Q_OBJECT
	using D=SampleDecoder;
	QByteArray wave(int channels,int rate,quint32 frames=4099)
	{
		QByteArray bytes(44+frames*channels*2,0);
		std::memcpy(bytes.data(),"RIFF",4);std::memcpy(bytes.data()+8,"WAVEfmt ",8);std::memcpy(bytes.data()+36,"data",4);
		auto p16=[&](int off,quint16 v){qToLittleEndian(v,bytes.data()+off);};
		auto p32=[&](int off,quint32 v){qToLittleEndian(v,bytes.data()+off);};
		p32(4,bytes.size()-8);p32(16,16);p16(20,1);p16(22,channels);p32(24,rate);
		p32(28,rate*channels*2);p16(32,channels*2);p16(34,16);p32(40,frames*channels*2);
		for(quint32 i=0;i<frames*channels;++i)qToLittleEndian<qint16>(i%2 ? 32767 : -32768,bytes.data()+44+i*2);
		return bytes;
	}
private slots:
	void scannerMatchesSecureDecoderAcrossChunks()
	{
		class OddReads:public QBuffer{public:using QBuffer::QBuffer;
			qint64 readData(char* p,qint64 n)override{return QBuffer::readData(p,pos()<44?n:std::min<qint64>(n,17));}};
		for(int channels:{1,2})for(int rate:{44100,48000})
		{
			auto bytes=wave(channels,rate);QBuffer baseline(&bytes);QVERIFY(baseline.open(QIODevice::ReadOnly));
			const auto decoded=D::decodePcmWave(baseline,{1000000,1000000});QCOMPARE(decoded.code,QString("ok"));
			OddReads source(&bytes);QVERIFY(source.open(QIODevice::ReadOnly|QIODevice::Unbuffered));
			std::atomic_bool cancel=false;quint64 next=0;
			const auto scanned=D::scanPcmWave(source,{1000000,1000000},[&](quint64 first,std::span<const SampleFrame> frames){
				if(first!=next || frames.size()>2048)return PcmWaveVisitDecision::ResourceLimit;
				for(size_t j=0;j<frames.size();++j)for(int c=0;c<2;++c)
					if(frames[j][c]!=decoded.sample->data[first+j][c])return PcmWaveVisitDecision::ResourceLimit;
				next+=frames.size();return PcmWaveVisitDecision::Continue;
			},cancel);
			QCOMPARE(scanned.code,QString("ok"));QCOMPARE(next,quint64(4099));
			QCOMPARE(scanned.info.sourceSha256,decoded.info.sourceSha256);
			QCOMPARE(scanned.info.sourceSha256,QCryptographicHash::hash(bytes,QCryptographicHash::Sha256));
		}
	}
	void hugeSourceSmallSelectionUsesFixedScratch()
	{
		// A virtual 100MB source: no source-sized storage in either fixture or consumer.
		class VirtualWave:public QIODevice{public:QByteArray header; qint64 readMax=0;
			qint64 size()const override{return 100000044;}
			bool isSequential()const override{return false;}
			qint64 readData(char* p,qint64 n)override{readMax=std::max(readMax,n);auto at=pos();
				n=std::min(n,size()-at);std::memset(p,0,n);if(at<44)std::memcpy(p,header.constData()+at,std::min(n,44-at));return n;}
			qint64 writeData(const char*,qint64)override{return -1;}} source;
		source.header=wave(1,44100,1).left(44);
		qToLittleEndian<quint32>(100000036,source.header.data()+4);qToLittleEndian<quint32>(100000000,source.header.data()+40);
		QVERIFY(source.open(QIODevice::ReadOnly|QIODevice::Unbuffered));std::atomic_bool cancel=false;
		quint64 selected=0;largestAllocation=0;measureAllocations=true;
		const auto result=D::scanPcmWave(source,{200000000,500000000},[&](quint64 first,std::span<const SampleFrame> frames){
			if(first<10)selected+=std::min<quint64>(10-first,frames.size());
			return PcmWaveVisitDecision::Continue;
		},cancel);
		measureAllocations=false;
		QCOMPARE(result.code,QString("ok"));QCOMPARE(selected,quint64(10));
		QVERIFY2(largestAllocation<1024*1024,qPrintable(QString::number(largestAllocation)));
		QVERIFY(source.readMax<=4096);QCOMPARE(result.info.frames,quint64(50000000));
	}
	void shortReadCancelAndAllocationFailureNeverReturnSuccess()
	{
		auto bytes=wave(2,48000);QBuffer source(&bytes);QVERIFY(source.open(QIODevice::ReadOnly));
		std::atomic_bool cancel=true;
		auto consume=[](quint64,std::span<const SampleFrame>){return PcmWaveVisitDecision::Continue;};
		QCOMPARE(D::scanPcmWave(source,{1000000,1000000},consume,cancel).code,QString("cancelled"));
		cancel=false;
		auto result=D::scanPcmWave(source,{1000000,1000000},[&](quint64,std::span<const SampleFrame>){cancel=true;return PcmWaveVisitDecision::Continue;},cancel);
		QCOMPARE(result.code,QString("cancelled"));QVERIFY(result.info.sourceSha256.isEmpty());
		cancel=false;
		result=D::scanPcmWave(source,{1000000,1000000},[](quint64,std::span<const SampleFrame>)->PcmWaveVisitDecision{throw std::bad_alloc();},cancel);
		QCOMPARE(result.code,QString("source_too_large"));QVERIFY(result.info.sourceSha256.isEmpty());
		class Short:public QBuffer{public:using QBuffer::QBuffer;qint64 readData(char* p,qint64 n)override{
			if(pos()>=100)return 0;
			return QBuffer::readData(p,std::min(n,100-pos()));}} shortSource(&bytes);
		QVERIFY(shortSource.open(QIODevice::ReadOnly|QIODevice::Unbuffered));
		result=D::scanPcmWave(shortSource,{1000000,1000000},consume,cancel);
		QCOMPARE(result.code,QString("audio_decode_failed"));QVERIFY(result.info.sourceSha256.isEmpty());
	}
};
QTEST_GUILESS_MAIN(PcmWaveStreamingTest)
#include "PcmWaveStreamingTest.moc"
