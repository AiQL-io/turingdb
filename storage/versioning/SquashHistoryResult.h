#pragma once

#include "BasicResult.h"
#include "EnumToString.h"

namespace db {

enum class SquashHistoryErrorType : uint8_t {
    GRAPH_NOT_FOUND,
    COMMIT_NOT_FOUND,
    RANGE_NOT_ORDERED,
    COMMIT_NOT_LOADED,
    RANGE_HAS_MERGE_COMMIT,
    BLOCKED_BY_OPEN_CHANGE,
    COULD_NOT_LOAD_COMMITS,
    COULD_NOT_PERSIST,
    _SIZE,
};

using SquashHistoryErrorTypeDescription = EnumToString<SquashHistoryErrorType>::Create<
    EnumStringPair<SquashHistoryErrorType::GRAPH_NOT_FOUND, "Graph does not exist">,
    EnumStringPair<SquashHistoryErrorType::COMMIT_NOT_FOUND, "Commit does not exist">,
    EnumStringPair<SquashHistoryErrorType::RANGE_NOT_ORDERED, "The commit to squash after must come before the last commit to squash">,
    EnumStringPair<SquashHistoryErrorType::COMMIT_NOT_LOADED, "A commit of the range is not loaded">,
    EnumStringPair<SquashHistoryErrorType::RANGE_HAS_MERGE_COMMIT, "The range holds a commit made by MERGE_DATAPARTS, which renumbers entity IDs">,
    EnumStringPair<SquashHistoryErrorType::BLOCKED_BY_OPEN_CHANGE, "An open change is based on a commit that would be squashed away">,
    EnumStringPair<SquashHistoryErrorType::COULD_NOT_LOAD_COMMITS, "Could not load the commits of the range from disk">,
    EnumStringPair<SquashHistoryErrorType::COULD_NOT_PERSIST, "The squash was applied in memory but could not be written to disk, so a restart brings the old history back">>;

class SquashHistoryError {
public:
    explicit SquashHistoryError(SquashHistoryErrorType type)
        : _type(type)
    {
    }

    [[nodiscard]] SquashHistoryErrorType getType() const { return _type; }
    [[nodiscard]] std::string fmtMessage() const;

    template <typename... T>
    static BadResult<SquashHistoryError> result(SquashHistoryErrorType type) {
        return BadResult<SquashHistoryError>(SquashHistoryError(type));
    }

private:
    SquashHistoryErrorType _type {SquashHistoryErrorType::GRAPH_NOT_FOUND};
};

template <typename T>
using SquashHistoryResult = BasicResult<T, class SquashHistoryError>;

}
