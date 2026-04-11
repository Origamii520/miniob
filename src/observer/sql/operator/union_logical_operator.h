#pragma once

#include "sql/operator/logical_operator.h"

class UnionLogicalOperator : public LogicalOperator
{
public:
  explicit UnionLogicalOperator(bool all = false) : all_(all) {}
  ~UnionLogicalOperator() override = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::UNION; }
  bool                all() const { return all_; }

private:
  bool all_ = false;
};
