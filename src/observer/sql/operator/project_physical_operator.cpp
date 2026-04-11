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

#include "sql/operator/project_physical_operator.h"
#include "common/log/log.h"
#include "storage/record/record.h"
#include "storage/table/table.h"

using namespace std;

ProjectPhysicalOperator::ProjectPhysicalOperator(vector<unique_ptr<Expression>> &&expressions, int limit)
    : expressions_(std::move(expressions)), tuple_(expressions_), limit_(limit)
{
}

RC ProjectPhysicalOperator::open(Trx *trx)
{
  count_                = 0;
  emitted_constant_row_ = false;

  if (children_.empty()) {
    return RC::SUCCESS;
  }

  PhysicalOperator *child = children_[0].get();

  if (outer_tuple != nullptr) {
    LOG_DEBUG("msg from project_phy_oper: we are in subquery");
    child->set_outer_tuple(outer_tuple);
  }

  RC                rc    = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child operator: %s", strrc(rc));
    return rc;
  }

  return RC::SUCCESS;
}

RC ProjectPhysicalOperator::next()
{
  if (limit_ >= 0 && count_ >= limit_) {
    return RC::RECORD_EOF;
  }

  if (children_.empty()) {
    if (emitted_constant_row_) {
      return RC::RECORD_EOF;
    }
    emitted_constant_row_ = true;
    count_++;
    return RC::SUCCESS;
  }

  RC rc = children_[0]->next();
  if (rc == RC::SUCCESS) {
    count_++;
  }
  return rc;
}

RC ProjectPhysicalOperator::close()
{
  if (!children_.empty()) {
    children_[0]->close();
  }
  return RC::SUCCESS;
}
Tuple *ProjectPhysicalOperator::current_tuple()
{
  if (children_.empty()) {
    tuple_.set_tuple(nullptr);
    return &tuple_;
  }

  tuple_.set_tuple(children_[0]->current_tuple());
  return &tuple_;
}

RC ProjectPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  for (const unique_ptr<Expression> &expression : expressions_) {
    schema.append_cell(expression->display_name());
  }
  return RC::SUCCESS;
}
