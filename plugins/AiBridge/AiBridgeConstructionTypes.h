#ifndef LMMS_AIBRIDGE_CONSTRUCTION_TYPES_H
#define LMMS_AIBRIDGE_CONSTRUCTION_TYPES_H

#include <QString>
#include <QVector>

namespace lmms
{

struct AiBridgeInstrumentValue
{
	QString instrumentId;
	QString displayName;
	QString description;
	bool available = false;
};

struct AiBridgeTrackConstructionValue
{
	QString trackId;
	QString name;
	QString type;
	int index = 0;
	int volume = 0;
	int panning = 0;
};

struct AiBridgeClipConstructionValue
{
	QString clipId;
	QString trackId;
	QString name;
	QString type;
	qint64 startTicks = 0;
	qint64 lengthTicks = 0;
	bool autoResize = false;
	QString contentHash;
};

struct AiBridgeTrackPropertyValue
{
	QString trackId;
	int volume = 0;
	int panning = 0;
};

struct AiBridgeClipPropertyValue
{
	QString clipId;
	QString trackId;
	qint64 startTicks = 0;
	qint64 lengthTicks = 0;
	bool autoResize = false;
	QString contentHash;
};

struct AiBridgeConstructionPage
{
	int total = 0;
	int offset = 0;
	int limit = 0;
	bool hasMore = false;
	QVector<AiBridgeInstrumentValue> items;
};

} // namespace lmms

#endif
