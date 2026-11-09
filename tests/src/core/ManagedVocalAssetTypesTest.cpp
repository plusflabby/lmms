#include <QtTest>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include "ManagedVocalAssetTypes.h"

using namespace lmms;

class ManagedVocalAssetTypesTest : public QObject
{
	Q_OBJECT

	static QString digest(char value)
	{
		return QString(64, QChar(value));
	}

	static ManagedVocalManifest manifest()
	{
		ManagedVocalManifest value;
		value.assetId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		value.projectId = QStringLiteral("11111111-1111-4111-8111-111111111111");
		value.assetRootId = digest('e');
		value.media = {48000, 48000, 1, digest('a')};
		value.media.rms = 0.25;
		value.media.rmsKnown = true;
		value.recipeHash = digest('b');
		value.rendererId = QStringLiteral("native-float-renderer");
		value.rendererVersion = QStringLiteral("1");
		value.recipe = QJsonObject{{"recipe_version", 1}, {"operation", "gain_envelope"}};
		value.sources.push_back({"clip-a", digest('c'), digest('d'), QStringLiteral("C:/Authorized/Vox/lead.wav"),
			{0, 48000}, 48000, 48000, 1, ManagedVocalSourceBasis::AuthorizedPcm, {}, {}});
		return value;
	}

private slots:
	void roundTripsVersionedPrivateProvenanceAndPathFreeReference()
	{
		const auto original = manifest();
		const auto encoded = encodeManagedVocalManifest(original);
		QVERIFY(!encoded.isEmpty());
		QVERIFY(encoded.contains("C:/Authorized/Vox/lead.wav"));
		const auto decoded = decodeManagedVocalManifest(encoded);
		QCOMPARE(decoded.code, QStringLiteral("ok"));
		QVERIFY(decoded.value);
		QCOMPARE(decoded.value->assetId, original.assetId);
		QCOMPARE(decoded.value->sources.front().sourceSha256, digest('c'));
		QCOMPARE(decoded.value->sources.front().privateSourceReference, QStringLiteral("C:/Authorized/Vox/lead.wav"));

		const auto reference = makeManagedVocalVariantReference(original, encoded);
		QCOMPARE(reference.code, QStringLiteral("ok"));
		QVERIFY(reference.value);
		QCOMPARE(reference.value->assetId, original.assetId);
		QCOMPARE(reference.value->assetRootId, original.assetRootId);
		QCOMPARE(reference.value->manifestSha256,
			QString::fromLatin1(QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex()));
		QVERIFY(!reference.value->manifestSha256.contains('/'));
	}

	void rejectsTamperingUnknownKeysAndNoncanonicalFrames()
	{
		const auto encoded = encodeManagedVocalManifest(manifest());
		auto object = QJsonDocument::fromJson(encoded).object();
		object.insert(QStringLiteral("absolute_path"), QStringLiteral("C:/arbitrary.wav"));
		QCOMPARE(decodeManagedVocalManifest(QJsonDocument(object).toJson(QJsonDocument::Compact)).code,
			QStringLiteral("invalid_manifest"));

		object = QJsonDocument::fromJson(encoded).object();
		object.insert(QStringLiteral("media"), QJsonObject{{"frames", "048000"}, {"sample_rate", 48000},
			{"channels", 1}, {"pcm_sha256", digest('a')}, {"peak", 0.5}, {"overrange", false}});
		QCOMPARE(decodeManagedVocalManifest(QJsonDocument(object).toJson(QJsonDocument::Compact)).code,
			QStringLiteral("invalid_manifest"));
	}

	void refusesInvalidMediaMetadataAndSourceIdentity()
	{
		auto value = manifest();
		value.media.channels = 6;
		QVERIFY(encodeManagedVocalManifest(value).isEmpty());
		value = manifest();
		value.sources.front().sourceIdentity = QStringLiteral("unknown");
		QVERIFY(encodeManagedVocalManifest(value).isEmpty());
		value = manifest();
		value.sources.front().sourceFrames = {48000, 0};
		QVERIFY(encodeManagedVocalManifest(value).isEmpty());
	}

	void requiresAndPersistsStableProjectIdentity()
	{
		auto value = manifest();
		QVERIFY(!value.projectId.isEmpty());
		QVERIFY(!encodeManagedVocalManifest(value).isEmpty());
		auto missingProject = value;
		missingProject.projectId.clear();
		QCOMPARE(validateManagedVocalManifest(missingProject), QStringLiteral("invalid_manifest"));
		const auto encoded = encodeManagedVocalManifest(value);
		const auto object = QJsonDocument::fromJson(encoded).object();
		QCOMPARE(object.value(QStringLiteral("project_id")).toString(), value.projectId);
		QVERIFY(value.media.rmsKnown);
		auto missingRms = value;
		missingRms.media.rmsKnown = false;
		QVERIFY(encodeManagedVocalManifest(missingRms).isEmpty());
		const auto decoded = decodeManagedVocalManifest(encoded);
		QCOMPARE(decoded.value->media.rms, value.media.rms);
	}

	void readsLegacyVersionOneManifestWithoutProjectIdentity()
	{
		auto legacy = manifest();
		legacy.schemaVersion = 1;
		legacy.projectId.clear();
		legacy.media.rms = 0.0;
		legacy.media.rmsKnown = false;
		const auto encoded = encodeManagedVocalManifest(legacy);
		QVERIFY(!encoded.isEmpty());
		const auto object = QJsonDocument::fromJson(encoded).object();
		QVERIFY(!object.contains(QStringLiteral("project_id")));
		const auto decoded = decodeManagedVocalManifest(encoded);
		QCOMPARE(decoded.code, QStringLiteral("ok"));
		QVERIFY(decoded.value);
		QCOMPARE(decoded.value->schemaVersion, quint32(1));
		QVERIFY(decoded.value->projectId.isEmpty());
	}

	void roundTripsNativeAssociationWithoutPersistingSourcePaths()
	{
		const auto original = manifest();
		const auto encoded = encodeManagedVocalManifest(original);
		const auto ref = makeManagedVocalVariantReference(original, encoded);
		QVERIFY(ref.value);
		ManagedVocalVariantAssociation association;
		association.active = *ref.value;
		association.previousVariant = *ref.value;
		association.playback.tempoBpm = 120;
		association.playback.outputRate = 48000;
		association.playback.ticksPerQuarterNote = 48;
		association.playback.timeSignatureNumerator = 4;
		association.playback.timeSignatureDenominator = 4;
		association.playback.visualLengthTicks = 96;
		association.playback.nativeVisualCoverageFrames = 48000;
		association.playback.endpointResidualNumerator = 0;
		association.playback.endpointResidualDenominator = 1;
		association.playback.worldHash = digest('f');
		association.previousPlayback = association.playback;
		association.originalLengthTicks = 1;
		QCOMPARE(validateManagedVocalVariantAssociation(association), QStringLiteral("ok"));

		QDomDocument document;
		const auto element = writeManagedVocalVariantAssociation(document, association);
		QVERIFY(!element.isNull());
		QVERIFY(!document.toString().contains(QStringLiteral("C:/Authorized")));
		const auto parsed = readManagedVocalVariantAssociation(element);
		QCOMPARE(parsed.code, QStringLiteral("ok"));
		QVERIFY(parsed.value);
		QCOMPARE(parsed.value->active.assetId, association.active.assetId);
		QCOMPARE(parsed.value->previousVariant->manifestSha256, association.previousVariant->manifestSha256);
		QCOMPARE(parsed.value->playback.visualLengthTicks, qint64(96));
		QCOMPARE(parsed.value->playback.nativeVisualCoverageFrames, quint64(48000));

		auto altered = element.cloneNode(true).toElement();
		altered.setAttribute(QStringLiteral("source_path"), QStringLiteral("C:/secret.wav"));
		QCOMPARE(readManagedVocalVariantAssociation(altered).code, QStringLiteral("invalid_managed_variant"));
	}

	void rejectsInconsistentNativeVisualCoverage()
	{
		auto association = ManagedVocalVariantAssociation{};
		const auto source = manifest();
		const auto encoded = encodeManagedVocalManifest(source);
		const auto reference = makeManagedVocalVariantReference(source, encoded);
		QVERIFY(reference.value);
		association.active = *reference.value;
		association.playback.tempoBpm = 120;
		association.playback.outputRate = 48000;
		association.playback.visualLengthTicks = 96;
		association.playback.nativeVisualCoverageFrames = 47999;
		association.playback.endpointResidualNumerator = 0;
		association.playback.endpointResidualDenominator = 1;
		association.playback.worldHash = digest('f');
		association.originalLengthTicks = 1;
		QCOMPARE(validateManagedVocalVariantAssociation(association), QStringLiteral("invalid_managed_variant"));
	}
};

QTEST_APPLESS_MAIN(ManagedVocalAssetTypesTest)
#include "ManagedVocalAssetTypesTest.moc"
