#pragma once

#include <optional>
#include <string_view>

#include "QueryCommand.h"

namespace db {

class CypherAST;
class DeclContext;

class SquashCommitsQuery : public QueryCommand {
public:
    static SquashCommitsQuery* create(CypherAST* ast,
                                      std::optional<std::string_view> baseHashStr,
                                      std::optional<std::string_view> tipHashStr);

    Kind getKind() const override { return Kind::SQUASH_COMMITS_QUERY; }

    std::optional<std::string_view> getBaseHashStr() const { return _baseHashStr; }
    std::optional<std::string_view> getTipHashStr() const { return _tipHashStr; }

private:
    std::optional<std::string_view> _baseHashStr;
    std::optional<std::string_view> _tipHashStr;

    SquashCommitsQuery(DeclContext* declContext,
                       std::optional<std::string_view> baseHashStr,
                       std::optional<std::string_view> tipHashStr);
    ~SquashCommitsQuery() override;
};

}
