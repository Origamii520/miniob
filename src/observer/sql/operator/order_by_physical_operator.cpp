/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by WangYunlai on 2022/07/01.
//

#include "sql/operator/order_by_physical_operator.h"

#include <utility>

#include "common/log/log.h"
#include "storage/record/record.h"
#include "storage/table/table.h"

using namespace std;

namespace {

class MaterializedRowTuple final : public Tuple
{
public:
  explicit MaterializedRowTuple(const RowTuple &source)
  {
    record_ = source.record();
    tuple_.set_schema(source.table(), source.table()->table_meta().field_metas());
    tuple_.set_record(&record_);
    tuple_.set_rid(source.raw_rid());
    tuple_.set_table_name(source.raw_table_name());
    tuple_.table_alias_     = source.table_alias_;
    tuple_.rid_list_        = source.rid_list_;
    tuple_.table_name_list_ = source.table_name_list_;
  }

  int cell_num() const override { return tuple_.cell_num(); }
  RC  cell_at(int index, Value &cell) const override { return tuple_.cell_at(index, cell); }
  RC  spec_at(int index, TupleCellSpec &spec) const override { return tuple_.spec_at(index, spec); }
  RC  find_cell(const TupleCellSpec &spec, Value &cell) const override { return tuple_.find_cell(spec, cell); }

private:
  Record   record_;
  RowTuple tuple_;
};

class MaterializedJoinedTuple final : public Tuple
{
public:
  MaterializedJoinedTuple(unique_ptr<Tuple> left, unique_ptr<Tuple> right)
      : left_(std::move(left)), right_(std::move(right))
  {
    joined_.set_left(left_.get());
    joined_.set_right(right_.get());
  }

  int cell_num() const override { return joined_.cell_num(); }
  RC  cell_at(int index, Value &cell) const override { return joined_.cell_at(index, cell); }
  RC  spec_at(int index, TupleCellSpec &spec) const override { return joined_.spec_at(index, spec); }
  RC  find_cell(const TupleCellSpec &spec, Value &cell) const override { return joined_.find_cell(spec, cell); }

private:
  unique_ptr<Tuple> left_;
  unique_ptr<Tuple> right_;
  JoinedTuple       joined_;
};

unique_ptr<Tuple> materialize_tuple(const Tuple &tuple)
{
  if (const auto *row_tuple = dynamic_cast<const RowTuple *>(&tuple); row_tuple != nullptr) {
    return make_unique<MaterializedRowTuple>(*row_tuple);
  }

  if (const auto *joined_tuple = dynamic_cast<const JoinedTuple *>(&tuple); joined_tuple != nullptr) {
    return make_unique<MaterializedJoinedTuple>(
        materialize_tuple(*joined_tuple->left()), materialize_tuple(*joined_tuple->right()));
  }

  ValueListTuple value_list_tuple;
  RC             rc = ValueListTuple::make(tuple, value_list_tuple);
  if (OB_FAIL(rc)) {
    LOG_PANIC("failed to materialize tuple for order by. rc=%s", strrc(rc));
    return nullptr;
  }
  return make_unique<ValueListTuple>(std::move(value_list_tuple));
}

}  // namespace

OrderByPhysicalOperator::OrderByPhysicalOperator(vector<unique_ptr<Expression>> &&expressions,
                                                 vector<bool> &&order_by_descs)
  : order_by_exprs_(std::move(expressions)), order_by_descs_(std::move(order_by_descs))
{
}

RC OrderByPhysicalOperator::open(Trx *trx)
{
  tuple_idx_ = 0;
  tuples_.clear();
  if (children_.empty()) {
    return RC::SUCCESS;
  }

  PhysicalOperator *child = children_[0].get();

  if (outer_tuple != nullptr) {
    LOG_DEBUG("msg from order_by_phy_oper: we are in subquery");
    child->set_outer_tuple(outer_tuple);
  }

  RC                rc    = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child operator: %s", strrc(rc));
    return rc;
  }

  // while (child->next() == RC::SUCCESS) {
  // 不屏蔽下层算子传回来的 rc
  while ((rc = child->next()) == RC::SUCCESS) {
    auto tuple = materialize_tuple(*child->current_tuple());
    if (tuple == nullptr) {
      return RC::NOMEM;
    }

    SortEntry entry;
    entry.tuple = std::move(tuple);
    entry.order_keys.reserve(order_by_exprs_.size());
    for (const auto &order_by_expr : order_by_exprs_) {
      Value value;
      rc = order_by_expr->get_value(*entry.tuple, value);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to evaluate order-by expression. rc=%s", strrc(rc));
        return rc;
      }
      entry.order_keys.push_back(value);
    }
    tuples_.push_back(std::move(entry));
  }

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("ORDER-BY: failed to get next tuple. rc=%s", strrc(rc));
    return rc;
  }

  // 由于 null 在排序中被当成最小值，所以需要对 null 值进行特殊处理
  std::sort(tuples_.begin(), tuples_.end(), [this](const SortEntry &left, const SortEntry &right) {
    for (size_t i = 0; i < order_by_exprs_.size(); i++) {
      const Value &left_value  = left.order_keys[i];
      const Value &right_value = right.order_keys[i];
      // 如果 left 和 right 都是 null，则认为它们相等
      if (left_value.is_null() && right_value.is_null()) {
        continue;
      }
      // 如果 left 是 null，则认为它小于 right
      if (left_value.is_null()) {
        return order_by_descs_[i] ? false : true;
      }
      // 如果 right 是 null，则认为它大于 left
      if (right_value.is_null()) {
        return order_by_descs_[i] ? true : false;
      }
      // 如果 left 和 right 都不为 null，则比较它们的值
      if (left_value.compare(right_value) != 0) {
        return order_by_descs_[i] ? left_value.compare(right_value) > 0 : left_value.compare(right_value) < 0;
      }
    }
    return false;
  });

  return RC::SUCCESS;
}

RC OrderByPhysicalOperator::next()
{
  tuple_idx_++;
  return tuple_idx_ <= tuples_.size() ? RC::SUCCESS : RC::RECORD_EOF;
}

RC OrderByPhysicalOperator::close()
{
  if (children_.empty()) {
    return RC::SUCCESS;
  }
  // close 的时候养成习惯，清理资源，关闭子算子！！！！！！spent 30mins hereee
  children_[0]->close();
  tuples_.clear();
  tuple_idx_ = 0;
  return RC::SUCCESS;
}
Tuple *OrderByPhysicalOperator::current_tuple()
{
  return tuples_[tuple_idx_ - 1].tuple.get();
}

RC OrderByPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  for (const unique_ptr<Expression> &expression : order_by_exprs_) {
    schema.append_cell(expression->display_name());
  }
  return RC::SUCCESS;
}
