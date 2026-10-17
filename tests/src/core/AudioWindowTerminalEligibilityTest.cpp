// Bounded endpoint characterization only. No candidate renderer or library fix.
// All library probes retain initialized prefix/suffix padding inside their
// allocations; no invalid memory is deliberately supplied or dereferenced.
#include <QtTest>
#include <samplerate.h>
#include <algorithm>
#include <memory>
#include <vector>
#include "AudioDevice.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "Sample.h"
#include "SamplePlayHandle.h"
#include "AudioEndpointContextModel.h"

namespace {
float sourceValue(int index, int channel)
{
	return float((index % 101 - 50) * (channel + 1)) / 256.0f;
}
struct ProbeResult
{
	std::vector<float> output;
	int error = 0;
};
ProbeResult streamProbe(int quantum, int count, int outputCapacity, int channels,
	int eof, int converter, float prefix, bool sourceBackedPrefix = false, int sourceOrigin = 0)
{
	ProbeResult result;
	std::unique_ptr<SRC_STATE, decltype(&src_delete)> state(src_new(converter, channels, &result.error), src_delete);
	if (!state) { result.error = result.error ? result.error : -1; return result; }
	// One complete prefix and suffix frame. Each refill is a fresh view at
	// allocation[channels], exactly like a scratch refill but safely padded.
	std::vector<float> allocation((quantum + 2) * channels, 0.0f);
	std::vector<float> output(outputCapacity * channels, 0.0f);
	int loaded = 0, used = 0, available = 0;
	while (int(result.output.size()) < count * channels)
	{
		if (used == available)
		{
			available = std::min(quantum, count - loaded); used = 0;
			if (!available) { result.error = -2; return result; }
			for (int ch = 0; ch < channels; ++ch)
				allocation[ch] = sourceBackedPrefix ? sourceValue(sourceOrigin + std::max(0, loaded - 1), ch) : prefix;
			for (int i = 0; i < available; ++i) for (int ch = 0; ch < channels; ++ch)
				allocation[(i + 1) * channels + ch] = sourceValue(sourceOrigin + loaded + i, ch);
			loaded += available;
		}
		SRC_DATA data{};
		data.data_in = allocation.data() + (1 + used) * channels;
		data.input_frames = available - used;
		data.data_out = output.data();
		data.output_frames = std::min(outputCapacity, count - int(result.output.size()) / channels);
		data.src_ratio = 1.0;
		data.end_of_input = loaded == count ? eof : 0;
		result.error = src_process(state.get(), &data);
		if (result.error || data.input_frames_used < 0 || data.input_frames_used > data.input_frames
			|| data.output_frames_gen < 0 || data.output_frames_gen > data.output_frames)
		{ result.error = result.error ? result.error : -3; return result; }
		if (!data.input_frames_used && !data.output_frames_gen) { result.error = -4; return result; }
		used += int(data.input_frames_used);
		result.output.insert(result.output.end(), output.begin(), output.begin() + data.output_frames_gen * channels);
	}
	return result;
}
class ProbeDevice final : public lmms::AudioDevice
{
public:
	explicit ProbeDevice(int rate) : AudioDevice(2, lmms::Engine::audioEngine()) { setSampleRate(rate); }
private:
	void startProcessingImpl() override {}
	void stopProcessingImpl() override {}
};
}

class AudioWindowTerminalEligibilityTest : public QObject
{
	Q_OBJECT
	QString oldBufferSetting;
	int requestedPeriod = 256;
private slots:
	void initTestCase()
	{
		using namespace lmms;
		bool ok = false; const int configured = qEnvironmentVariableIntValue("LMMS_TERMINAL_TEST_BUFFER", &ok);
		if (ok) { requestedPeriod = configured; }
		oldBufferSetting = ConfigManager::inst()->value("audioengine", "framesperaudiobuffer", "terminal-test-absent");
		ConfigManager::inst()->setValue("audioengine", "framesperaudiobuffer", QString::number(requestedPeriod));
		Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing();
		qInfo() << "PROFILE" << src_get_version() << "scratch_quantum" << DEFAULT_BUFFER_SIZE
			<< "requested_device_buffer" << requestedPeriod << "engine_period" << Engine::audioEngine()->framesPerPeriod();
	}
	void cleanupTestCase()
	{
		using namespace lmms;
		if (oldBufferSetting == "terminal-test-absent") { ConfigManager::inst()->deleteValue("audioengine", "framesperaudiobuffer"); }
		else { ConfigManager::inst()->setValue("audioengine", "framesperaudiobuffer", oldBufferSetting); }
		Engine::destroy();
	}
	void refillQuantumControlsFreshSingleton_data()
	{
		QTest::addColumn<int>("quantum"); QTest::addColumn<int>("channels"); QTest::addColumn<int>("eof");
		for (int q : {1, 2, 3, 7, 32, 64, 128, 256, 511, 512})
			for (int c : {1, 2}) for (int e : {0, 1})
				QTest::newRow(qPrintable(QString("q%1-c%2-eof%3").arg(q).arg(c).arg(e))) << q << c << e;
	}
	void refillQuantumControlsFreshSingleton()
	{
		QFETCH(int, quantum); QFETCH(int, channels); QFETCH(int, eof);
		int cases = 0, sensitive = 0;
		for (int n = 1; n <= 2 * quantum + 2; ++n) for (int capacity : {1, 7, 32, 64, 128, 256, 512})
		{
			const auto a = streamProbe(quantum, n, capacity, channels, eof, SRC_LINEAR, 0.875f);
			const auto b = streamProbe(quantum, n, capacity, channels, eof, SRC_LINEAR, -0.875f);
			QCOMPARE(a.error, 0); QCOMPARE(b.error, 0);
			QCOMPARE(int(a.output.size()), n * channels); QCOMPARE(a.output.size(), b.output.size());
			int differences = 0;
			for (int i = 0; i < n; ++i) for (int ch = 0; ch < channels; ++ch)
			{
				if (a.output[i * channels + ch] != b.output[i * channels + ch]) { ++differences; }
				if (quantum > 1 && n % quantum != 1)
					QCOMPARE(a.output[i * channels + ch], sourceValue(std::max(0, i - 1), ch));
			}
			const bool freshSingleton = quantum == 1 || n % quantum == 1;
			QCOMPARE(differences, freshSingleton ? (quantum == 1 ? n : 1) * channels : 0);
			if (freshSingleton) { ++sensitive; }
			++cases;
		}
		qInfo() << "BOUNDED_MATRIX" << quantum << channels << eof << "cases" << cases << "prefix_sensitive" << sensitive;
	}
	void layoutAndInterpolationAreIndependentVariables_data()
	{
		QTest::addColumn<int>("channels"); QTest::addColumn<int>("eof");
		for (int c : {1, 2}) for (int e : {0, 1})
			QTest::newRow(qPrintable(QString("c%1-eof%2").arg(c).arg(e))) << c << e;
	}
	void layoutAndInterpolationAreIndependentVariables()
	{
		QFETCH(int, channels); QFETCH(int, eof);
		for (int quantum : {1, 2, 3, 7, 64, 128, 256, 511, 512}) for (int n : {1, quantum + 1, 2 * quantum + 1})
			for (int capacity : {1, 7, 256, 512})
			{
				const auto padded = streamProbe(quantum, n, capacity, channels, eof, SRC_LINEAR, 0.875f);
				const auto backed = streamProbe(quantum, n, capacity, channels, eof, SRC_LINEAR, 0.0f, true);
				const auto zoh = streamProbe(quantum, n, capacity, channels, eof, SRC_ZERO_ORDER_HOLD, 0.875f);
				QCOMPARE(padded.error, 0); QCOMPARE(backed.error, 0); QCOMPARE(zoh.error, 0);
				QCOMPARE(int(backed.output.size()), n * channels); QCOMPARE(int(zoh.output.size()), n * channels);
				QVERIFY(padded.output != backed.output);
				for (int i = 0; i < n; ++i) for (int ch = 0; ch < channels; ++ch)
				{
					const auto expected = sourceValue(std::max(0, i - 1), ch);
					QCOMPARE(backed.output[i * channels + ch], expected);
					QCOMPARE(zoh.output[i * channels + ch], expected);
				}
			}
	}
	void realNativePeriodDoesNotChangeScratchQuantum_data()
	{
		QTest::addColumn<int>("rate"); QTest::newRow("44100") << 44100; QTest::newRow("48000") << 48000;
	}
	void sourceReferenceIntervals_data()
	{
		QTest::addColumn<int>("rate"); QTest::addColumn<int>("originTicks");
		for (int rate : {44100, 48000}) for (int origin : {0, 10})
			QTest::newRow(qPrintable(QString("rate%1-origin%2").arg(rate).arg(origin))) << rate << origin;
	}
	void sourceReferenceIntervals()
	{
		QFETCH(int, rate); QFETCH(int, originTicks);
		const int q = rate / 100, origin = originTicks * q;
		int cases = 0, rejected = 0;
		for (int ticks : {1, 30, 80, 100, 137, 199, 200}) for (int quantum : {64, 128, 256, 511})
			for (int capacity : {1, 7, 32, 256, 512})
			{
				const int n = ticks * q;
				const auto a = streamProbe(quantum, n, capacity, 2, 0, SRC_LINEAR, 0.875f, false, origin);
				const auto b = streamProbe(quantum, n, capacity, 2, 0, SRC_LINEAR, -0.875f, false, origin);
				QCOMPARE(a.error, 0); QCOMPARE(b.error, 0); QCOMPARE(int(a.output.size()), n * 2);
				const bool unsafe = n % quantum == 1;
				QCOMPARE(a.output != b.output, unsafe);
				if (unsafe) { ++rejected; }
				else for (int i = 0; i < n; ++i) for (int ch = 0; ch < 2; ++ch)
					QCOMPARE(a.output[i * 2 + ch], sourceValue(origin + std::max(0, i - 1), ch));
				++cases;
			}
		qInfo() << "SOURCE_REFERENCE_MATRIX" << rate << origin << "cases" << cases << "unsafe" << rejected;
	}
	void realNativePeriodDoesNotChangeScratchQuantum()
	{
		using namespace lmms; QFETCH(int, rate);
		auto* engine = Engine::audioEngine(); engine->setAudioDevice(new ProbeDevice(rate), false);
		const int period = engine->framesPerPeriod();
		QCOMPARE(period, std::min(std::clamp(requestedPeriod, int(MINIMUM_BUFFER_SIZE), int(MAXIMUM_BUFFER_SIZE)), int(DEFAULT_BUFFER_SIZE)));
		// The existing investigation oracle stays unchanged. Its initial V2
		// profile must agree with the source-span/scratch predicate at every
		// device period, including periods that do not divide the scratch size.
		const int q = rate / 100;
		for (int ticks : {1, 30, 80, 100, 137, 199, 200})
		{
			test::EndpointContextProposal p{{0, ticks * q, 0, 200 * q, rate, 125, rate, 48, q, ticks, 0}, ticks * q};
			const bool eligible = DEFAULT_BUFFER_SIZE > 1 && (p.referenceEndFrame - p.base.phaseOriginFrame) % DEFAULT_BUFFER_SIZE != 1;
			QCOMPARE(test::proposalEligibility(p), eligible ? QString("ok") : QString("unrepresentable_audio_boundary"));
		}
		int streams = 0;
		// Never ask the native unpadded scratch path to render an unsafe refill.
		for (int origin : {0, rate / 10}) for (int n : {255, 256, 258, 511, 512, 514, 60416, 60418, 88200})
		{
			QVERIFY(n % DEFAULT_BUFFER_SIZE != 1);
			std::vector<SampleFrame> source(origin + n);
			for (int i = 0; i < origin + n; ++i) { source[i] = SampleFrame{sourceValue(i, 0), sourceValue(i, 1)}; }
			Sample sample(source.data(), source.size(), rate); sample.setStartFrame(origin);
			SamplePlayHandle handle(&sample, false);
			std::vector<SampleFrame> block(period);
			for (int base = 0; base < n + 2 * period; base += period)
			{
				handle.play(block.data());
				for (int j = 0; j < period; ++j) for (int ch = 0; ch < 2; ++ch)
					QCOMPARE(block[j][ch], base + j < n ? sourceValue(origin + std::max(0, base + j - 1), ch) : 0.0f);
			}
			++streams;
		}
		qInfo() << "NATIVE_PERIOD_CONTROL" << "rate" << rate << "period" << period << "scratch" << DEFAULT_BUFFER_SIZE << "streams" << streams;
	}
};
QTEST_MAIN(AudioWindowTerminalEligibilityTest)
#include "AudioWindowTerminalEligibilityTest.moc"
