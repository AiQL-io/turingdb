#include "SquashCommitsQuery.h"

#include "CypherAST.h"
#include "decl/DeclContext.h"

using namespace db;

SquashCommitsQuery::SquashCommitsQuery(DeclContext* declContext,
                                       std::optional<std::string_view> baseHashStr,
                                       std::optional<std::string_view> tipHashStr)
    : QueryCommand(declContext),
    _baseHashStr(baseHashStr),
    _tipHashStr(tipHashStr)
{
}

SquashCommitsQuery::~SquashCommitsQuery() {
}

SquashCommitsQuery* SquashCommitsQuery::create(CypherAST* ast,
                                               std::optional<std::string_view> baseHashStr,
                                               std::optional<std::string_view> tipHashStr) {
    DeclContext* declContext = DeclContext::create(ast, nullptr);

    SquashCommitsQuery* squashCommits = new SquashCommitsQuery(declContext, baseHashStr, tipHashStr);
    ast->addQuery(squashCommits);

    return squashCommits;
}
