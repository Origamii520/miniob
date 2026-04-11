#pragma once

#include "sql/operator/physical_operator.h"

class UnionPhysicalOperator : public PhysicalOperator
{
public:
  UnionPhysicalOperator()          = default;
  ~UnionPhysicalOperator() override = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::UNION; }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  Tuple *current_tuple() override;
  RC tuple_schema(TupleSchema &schema) const override;

private:
  RC consume_child(PhysicalOperator &child);

private:
  std::vector<std::unique_ptr<Tuple>> tuples_;
  size_t                              tuple_idx_ = 0;
};
