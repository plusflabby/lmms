#include <QtTest>
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <vector>
#include "ManagedVocalPlayback.h"
#include "TimePos.h"

using namespace lmms;

class ManagedVocalPlaybackTest : public QObject
{
	Q_OBJECT

	static ManagedVocalPlaybackWorld world(int rate = 48000, bool tempoAutomation = false)
	{
		return {120, rate, 48, tempoAutomation};
	}

private slots:
	void mapsMonoFramesAndStopsAtTheExactMediaEndpoint()
	{
		auto guard = std::make_shared<ManagedVocalPlaybackGuard>();
		const auto created = makeManagedVocalPlaybackView({0.25f, -0.5f, 1.25f},
			48000, 1, world(), false, false, guard, guard->epoch.load());
		QCOMPARE(created.code, QStringLiteral("ok"));
		QVERIFY(created.view);
		QCOMPARE(created.view->frames, quint64(3));

		std::array<SampleFrame, 5> output;
		std::fill(output.begin(), output.end(), SampleFrame(9.0f));
		renderManagedVocalPlayback(*created.view, 0, output);
		QCOMPARE(output[0].left(), 0.25f);
		QCOMPARE(output[0].right(), 0.25f);
		QCOMPARE(output[2].left(), 1.25f);
		QCOMPARE(output[2].right(), 1.25f);
		QCOMPARE(output[3].left(), 0.0f);
		QCOMPARE(output[4].right(), 0.0f);
	}

	void seeksAndInvalidatesAlreadyPreparedPlayback()
	{
		auto guard = std::make_shared<ManagedVocalPlaybackGuard>();
		const auto created = makeManagedVocalPlaybackView({0.1f, -0.1f, 0.2f, -0.2f},
			44100, 2, world(44100), false, false, guard, guard->epoch.load());
		QCOMPARE(created.code, QStringLiteral("ok"));
		QVERIFY(created.view);
		std::array<SampleFrame, 3> output{};
		renderManagedVocalPlayback(*created.view, 1, output);
		QCOMPARE(output[0].left(), 0.2f);
		QCOMPARE(output[0].right(), -0.2f);
		QCOMPARE(output[1].left(), 0.0f);

		guard->epoch.fetch_add(1, std::memory_order_acq_rel);
		std::fill(output.begin(), output.end(), SampleFrame(4.0f));
		renderManagedVocalPlayback(*created.view, 0, output);
		for (const auto& frame : output)
		{
			QCOMPARE(frame.left(), 0.0f);
			QCOMPARE(frame.right(), 0.0f);
		}

		const auto restarted = makeManagedVocalPlaybackView(
			created.view->interleavedSamples, 44100, 2, world(44100), false, false,
			guard, guard->epoch.load(std::memory_order_acquire));
		QCOMPARE(restarted.code, QStringLiteral("ok"));
		QVERIFY(restarted.view);
		std::fill(output.begin(), output.end(), SampleFrame{});
		renderManagedVocalPlayback(*restarted.view, 0, output);
		QCOMPARE(output[0].left(), 0.1f);
		QCOMPARE(output[0].right(), -0.1f);
	}

	void rejectsUnsupportedWorldAndClipProperties()
	{
		auto guard = std::make_shared<ManagedVocalPlaybackGuard>();
		const std::vector<float> samples{0.1f, 0.2f};
		QCOMPARE(makeManagedVocalPlaybackView(samples, 48000, 1, world(44100), false, false,
			guard, guard->epoch.load()).code, QStringLiteral("unsupported_playback_world"));
		QCOMPARE(makeManagedVocalPlaybackView(samples, 48000, 1, world(48000, true), false, false,
			guard, guard->epoch.load()).code, QStringLiteral("unsupported_playback_world"));
		QCOMPARE(makeManagedVocalPlaybackView(samples, 48000, 1, world(), true, false,
			guard, guard->epoch.load()).code, QStringLiteral("unsupported_clip_state"));
		QCOMPARE(makeManagedVocalPlaybackView(samples, 48000, 1, world(), false, true,
			guard, guard->epoch.load()).code, QStringLiteral("unsupported_clip_state"));
	}

	void rejectsNonfiniteAndMismatchedSampleCount()
	{
		auto guard = std::make_shared<ManagedVocalPlaybackGuard>();
		const auto epoch = guard->epoch.load();
		QCOMPARE(makeManagedVocalPlaybackView({0.1f}, 48000, 2, world(), false, false,
			guard, epoch).code, QStringLiteral("invalid_audio_samples"));
		QCOMPARE(makeManagedVocalPlaybackView({std::numeric_limits<float>::infinity(), 0.0f},
			48000, 2, world(), false, false, guard, epoch).code, QStringLiteral("invalid_audio_samples"));
	}

	void choosesTheSmallestNativeTickExtentThatCoversExactMediaFrames()
	{
		for (const auto rate : {44100, 48000})
		{
			for (const auto tempo : {127, 140, 333})
			{
				const auto mediaFrames = quint64(rate) * 73 + 17;
				const auto extent = managedVocalVisualExtent(mediaFrames, rate, tempo);
				QCOMPARE(extent.code, QStringLiteral("ok"));
				QVERIFY(extent.nativeCoverageFrames >= mediaFrames);
				QCOMPARE(extent.visualResidualFrames, extent.nativeCoverageFrames - mediaFrames);
				const float nativeFramesPerTick = rate * 60.0f * 4 / DefaultTicksPerBar / tempo;
				const auto realized = TimePos(int(extent.visualLengthTicks)).frames(nativeFramesPerTick);
				QCOMPARE(quint64(realized), extent.nativeCoverageFrames);
				if (extent.visualLengthTicks > 0)
				{
					const auto previous = TimePos(int(extent.visualLengthTicks - 1)).frames(nativeFramesPerTick);
					QVERIFY(previous < mediaFrames);
				}
			}
		}
	}
};

QTEST_APPLESS_MAIN(ManagedVocalPlaybackTest)
#include "ManagedVocalPlaybackTest.moc"
