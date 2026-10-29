#include "SquashHistoryResult.h"

#include <string>

using namespace db;

std::string SquashHistoryError::fmtMessage() const {
    return std::string {SquashHistoryErrorTypeDescription::value(_type)};
}
