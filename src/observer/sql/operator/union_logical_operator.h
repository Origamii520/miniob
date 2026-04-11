#pragma once

#include "sql/operator/logical_operator.h"

class UnionLogicalOperator : public LogicalOperator
{
public:
  UnionLogicalOperator()          = default;
  ~UnionLogicalOperator() override = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::UNION; }
};
