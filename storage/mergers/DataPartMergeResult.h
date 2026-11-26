#pragma once

#include "BasicResult.h"
#include "EnumToString.h"

namespace db {

enum class DataPartMergeErrorType : uint8_t {
    MERGE_GRAPH_FAILED,
    BLOCKED_BY_OPEN_CHANGE,
    COULD_NOT_PERSIST,
    _SIZE,
};

using DataPartMergeErrorTypeDescription = EnumToString<DataPartMergeErrorType>::Create<
    EnumStringPair<DataPartMergeErrorType::MERGE_GRAPH_FAILED, "Could Not Build The Merged Graph">,
    EnumStringPair<DataPartMergeErrorType::BLOCKED_BY_OPEN_CHANGE, "Could Not Merge A Graph That Has An Open Change">,
    EnumStringPair<DataPartMergeErrorType::COULD_NOT_PERSIST, "Could Not Write The Merged Graph To Disk">>;

class DataPartMergeError {
public:
    explicit DataPartMergeError(DataPartMergeErrorType type)
        : _type(type)
    {
    }

    [[nodiscard]] DataPartMergeErrorType getType() const { return _type; }
    [[nodiscard]] std::string fmtMessage() const;

    template <typename... T>
    static BadResult<DataPartMergeError> result(DataPartMergeErrorType type) {
        return BadResult<DataPartMergeError>(DataPartMergeError(type));
    }

private:
    DataPartMergeErrorType _type {DataPartMergeErrorType::MERGE_GRAPH_FAILED};
};

template <typename T>
using DataPartMergeResult = BasicResult<T, class DataPartMergeError>;

}
