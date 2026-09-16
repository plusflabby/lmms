#ifndef LMMS_AIBRIDGE_VALIDATION_TYPES_H
#define LMMS_AIBRIDGE_VALIDATION_TYPES_H

#include <QString>

namespace lmms
{

struct AiBridgeValidationResult
{
	QString code;
	QString message;
	bool success() const { return code.isEmpty(); }
};

} // namespace lmms

#endif
