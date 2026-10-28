#include "PatternData.h"

#include <algorithm>

#include "CypherAST.h"
#include "LabelExpr.h"

using namespace db;

namespace {

bool isTypeList(const LabelExpr* typeExpr) {
    if (typeExpr->getKind() == LabelExpr::Kind::OR) {
        return std::ranges::all_of(typeExpr->getOperands(), &LabelExpr::namesOneLabel);
    }

    return typeExpr->namesOneLabel();
}

}

// PatternData
PatternData::PatternData()
{
}

PatternData::~PatternData() {
}

void PatternData::addExprConstraint(std::string_view typeName, ValueType valueType, Expr* expr) {
    _exprConstraints.emplace_back(typeName, valueType, expr);
}

// NodePatternData
NodePatternData::NodePatternData()
{
}

NodePatternData::~NodePatternData() {
}

NodePatternData* NodePatternData::create(CypherAST* ast) {
    NodePatternData* data = new NodePatternData();
    ast->addNodePatternData(data);
    return data;
}

void NodePatternData::addLabelConstraints(const SymbolChain* labels) {
    collectSchemaNames(labels, _labelConstraints);
}

void NodePatternData::addLabelExpr(const LabelExpr* labelExpr) {
    const LabelExpr::Kind kind = labelExpr->getKind();

    if (kind == LabelExpr::Kind::LABELS) {
        addLabelConstraints(labelExpr->getLabels());
    } else if (kind == LabelExpr::Kind::AND) {
        for (const LabelExpr* conjunct : labelExpr->getOperands()) {
            addLabelExpr(conjunct);
        }
    } else {
        _labelExprConstraints.push_back(labelExpr);
    }
}

// EdgePatternData
EdgePatternData::EdgePatternData()
{
}

EdgePatternData::~EdgePatternData() {
}

EdgePatternData* EdgePatternData::create(CypherAST* ast) {
    EdgePatternData* data = new EdgePatternData();
    ast->addEdgePatternData(data);
    return data;
}

void EdgePatternData::addEdgeTypeConstraints(const SymbolChain* edgeTypes) {
    collectSchemaNames(edgeTypes, _edgeTypeConstraints);
}

void EdgePatternData::addEdgeTypeExpr(const LabelExpr* typeExpr) {
    const LabelExpr::Kind kind = typeExpr->getKind();
    const bool listsTheTypes = _edgeTypeConstraints.empty() && isTypeList(typeExpr);

    if (kind == LabelExpr::Kind::AND) {
        for (const LabelExpr* conjunct : typeExpr->getOperands()) {
            addEdgeTypeExpr(conjunct);
        }
    } else if (listsTheTypes && kind == LabelExpr::Kind::OR) {
        for (const LabelExpr* alternative : typeExpr->getOperands()) {
            addEdgeTypeConstraints(alternative->getLabels());
        }
    } else if (listsTheTypes) {
        addEdgeTypeConstraints(typeExpr->getLabels());
    } else {
        _edgeTypeExprConstraints.push_back(typeExpr);
    }
}
