#include "sql/operator/union_physical_operator.h"

#include "common/log/log.h"
#include "sql/expr/tuple.h"

namespace {

bool tuple_exists(const std::vector<std::unique_ptr<Tuple>> &tuples, const Tuple &candidate)
{
  for (const auto &tuple : tuples) {
    int result = 0;
    if (tuple->compare(candidate, result) == RC::SUCCESS && result == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

RC UnionPhysicalOperator::open(Trx *trx)
{
  tuple_idx_ = 0;
  tuples_.clear();

  if (children_.size() != 2) {
    LOG_WARN("union expects exactly two children. size=%zu", children_.size());
    return RC::INTERNAL;
  }

  for (auto &child : children_) {
    RC rc = child->open(trx);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to open union child. rc=%s", strrc(rc));
      return rc;
    }

    rc = consume_child(*child);
    RC close_rc = child->close();
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (OB_FAIL(close_rc)) {
      return close_rc;
    }
  }

  return RC::SUCCESS;
}

RC UnionPhysicalOperator::consume_child(PhysicalOperator &child)
{
  RC rc = RC::SUCCESS;
  while (OB_SUCC(rc = child.next())) {
    ValueListTuple value_list_tuple;
    rc = ValueListTuple::make(*child.current_tuple(), value_list_tuple);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to materialize union tuple. rc=%s", strrc(rc));
      return rc;
    }

    if (!tuple_exists(tuples_, value_list_tuple)) {
      tuples_.emplace_back(std::make_unique<ValueListTuple>(std::move(value_list_tuple)));
    }
  }

  if (rc == RC::RECORD_EOF) {
    return RC::SUCCESS;
  }
  LOG_WARN("failed to fetch union child tuple. rc=%s", strrc(rc));
  return rc;
}

RC UnionPhysicalOperator::next()
{
  tuple_idx_++;
  return tuple_idx_ <= tuples_.size() ? RC::SUCCESS : RC::RECORD_EOF;
}

RC UnionPhysicalOperator::close()
{
  tuples_.clear();
  tuple_idx_ = 0;
  return RC::SUCCESS;
}

Tuple *UnionPhysicalOperator::current_tuple()
{
  if (tuple_idx_ == 0 || tuple_idx_ > tuples_.size()) {
    return nullptr;
  }
  return tuples_[tuple_idx_ - 1].get();
}

RC UnionPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  if (tuples_.empty()) {
    if (!children_.empty()) {
      return children_.front()->tuple_schema(schema);
    }
    return RC::SUCCESS;
  }

  const Tuple &tuple = *tuples_.front();
  for (int i = 0; i < tuple.cell_num(); i++) {
    TupleCellSpec spec;
    RC            rc = tuple.spec_at(i, spec);
    if (OB_FAIL(rc)) {
      return rc;
    }
    schema.append_cell(spec);
  }
  return RC::SUCCESS;
}
