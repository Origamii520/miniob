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
#include "sql/expr/composite_tuple.h"
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
    table_  = source.table();
    fields_ = source.table()->table_meta().field_metas();
    null_bitmap_start_ = 0;
    for (const auto &trx_field : table_->table_meta().trx_fields()) {
      null_bitmap_start_ += trx_field.len();
    }
    bitmap_ = std::make_unique<Bitmap>(record_.data() + null_bitmap_start_, fields_->size());

    set_rid(source.raw_rid());
    set_table_name(source.raw_table_name());
    table_alias_     = source.table_alias_;
    rid_list_        = source.rid_list_;
    table_name_list_ = source.table_name_list_;
  }

  int cell_num() const override { return static_cast<int>(fields_->size()); }

  RC cell_at(int index, Value &cell) const override
  {
    if (index < 0 || index >= static_cast<int>(fields_->size())) {
      LOG_WARN("invalid argument. index=%d", index);
      return RC::INVALID_ARGUMENT;
    }

    const FieldMeta &field_meta = (*fields_)[index];
    if (bitmap_->get_bit(index)) {
      cell.set_null();
      return RC::SUCCESS;
    }
    if (field_meta.type() == AttrType::TEXTS) {
      TextData text_data;
      memcpy(&text_data, record_.data() + field_meta.offset(), field_meta.len());
      table_->load_text(&text_data);
      cell.set_type(AttrType::TEXTS);
      cell.set_text(text_data.str, text_data.len, true);
    } else if (field_meta.type() == AttrType::VECTORS) {
      VectorData vector_data;
      memcpy(&vector_data, record_.data() + field_meta.offset(), field_meta.len());
      table_->load_vector(&vector_data);
      cell.set_type(AttrType::VECTORS);
      cell.set_vector(vector_data, true);
    } else {
      cell.set_type(field_meta.type());
      cell.set_data(record_.data() + field_meta.offset(), field_meta.len());
    }

    cell.view_set_info(raw_rid().page_num, raw_rid().slot_num, raw_table_name());
    return RC::SUCCESS;
  }

  RC spec_at(int index, TupleCellSpec &spec) const override
  {
    if (index < 0 || index >= static_cast<int>(fields_->size())) {
      LOG_WARN("invalid argument. index=%d", index);
      return RC::INVALID_ARGUMENT;
    }
    spec = TupleCellSpec(table_->name(), (*fields_)[index].name());
    return RC::SUCCESS;
  }

  RC find_cell(const TupleCellSpec &spec, Value &cell) const override
  {
    const char *table_name = spec.table_name();
    const char *field_name = spec.field_name();

    if (0 != strcmp(table_name, table_->name())) {
      return RC::NOTFOUND;
    }

    auto spec_table_alias = spec.table_alias();
    if (!spec_table_alias.empty() && !table_alias_.empty() && table_alias_ != spec_table_alias) {
      return RC::NOTFOUND;
    }

    for (size_t i = 0; i < fields_->size(); ++i) {
      if (0 == strcmp(field_name, (*fields_)[i].name())) {
        return cell_at(static_cast<int>(i), cell);
      }
    }
    return RC::NOTFOUND;
  }

private:
  const Table                  *table_ = nullptr;
  const std::vector<FieldMeta> *fields_ = nullptr;
  int                           null_bitmap_start_ = 0;
  std::unique_ptr<Bitmap>       bitmap_;
  Record                        record_;

public:
  std::vector<RID>         rid_list_;
  std::vector<std::string> table_name_list_;
};

RC append_materialized_tuple(const Tuple &tuple, CompositeTuple &composite_tuple)
{
  if (const auto *row_tuple = dynamic_cast<const RowTuple *>(&tuple); row_tuple != nullptr) {
    composite_tuple.add_tuple(make_unique<MaterializedRowTuple>(*row_tuple));
    return RC::SUCCESS;
  }

  if (const auto *joined_tuple = dynamic_cast<const JoinedTuple *>(&tuple); joined_tuple != nullptr) {
    RC rc = append_materialized_tuple(*joined_tuple->left(), composite_tuple);
    if (OB_FAIL(rc)) {
      return rc;
    }
    return append_materialized_tuple(*joined_tuple->right(), composite_tuple);
  }

  ValueListTuple value_list_tuple;
  RC             rc = ValueListTuple::make(tuple, value_list_tuple);
  if (OB_FAIL(rc)) {
    return rc;
  }
  composite_tuple.add_tuple(make_unique<ValueListTuple>(std::move(value_list_tuple)));
  return RC::SUCCESS;
}

unique_ptr<Tuple> materialize_tuple(const Tuple &tuple)
{
  if (const auto *row_tuple = dynamic_cast<const RowTuple *>(&tuple); row_tuple != nullptr) {
    return make_unique<MaterializedRowTuple>(*row_tuple);
  }

  if (dynamic_cast<const JoinedTuple *>(&tuple) != nullptr) {
    auto composite_tuple = make_unique<CompositeTuple>();
    RC   rc              = append_materialized_tuple(tuple, *composite_tuple);
    if (OB_FAIL(rc)) {
      LOG_PANIC("failed to materialize tuple for order by. rc=%s", strrc(rc));
      return nullptr;
    }
    return composite_tuple;
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
