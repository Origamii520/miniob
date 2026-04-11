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
// Created by Soulter on 2024/11/5.
//

#include "sql/stmt/create_view_stmt.h"

#include <cstddef>

#include "common/log/log.h"
#include "event/sql_debug.h"
#include "sql/parser/parse_defs.h"
#include "storage/db/db.h"
#include "sql/stmt/select_stmt.h"

namespace {

bool check_is_updatable(SelectStmt *select_stmt)
{
  if (select_stmt->has_special_queries()) {
    return false;
  }
  if (select_stmt->has_join()) {
    return false;
  }
  return true;
}

}  // namespace

RC CreateViewStmt::create(Db *db, CreateViewSqlNode &create_view, Stmt *&stmt)
{
  stmt = nullptr;

  if (create_view.sub_select == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  if (db->find_table(create_view.view_name.c_str()) != nullptr) {
    LOG_WARN("view name already exists. view name=%s", create_view.view_name.c_str());
    return RC::SCHEMA_TABLE_EXIST;
  }

  Stmt *stmt_ = nullptr;
  RC    rc    = Stmt::create_stmt(db, *create_view.sub_select, stmt_);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create stmt. rc=%d:%s", rc, strrc(rc));
    return rc;
  }

  auto *select_stmt  = static_cast<SelectStmt *>(stmt_);
  auto  query_fields = select_stmt->get_query_fields();

  if (!create_view.attrs_name.empty() && query_fields.size() != create_view.attrs_name.size()) {
    LOG_WARN("select query expr num count doesn't match attr count");
    return RC::INVALID_ARGUMENT;
  }

  unordered_map<string, std::nullptr_t> attr_name_map;
  for (auto &attr_name : create_view.attrs_name) {
    if (attr_name_map.find(attr_name) != attr_name_map.end()) {
      LOG_WARN("duplicate column name in view definition");
      return RC::INVALID_ARGUMENT;
    }
    attr_name_map[attr_name] = nullptr;
  }

  stmt = new CreateViewStmt(create_view.view_name, create_view.attrs_name);
  auto *create_view_stmt = static_cast<CreateViewStmt *>(stmt);
  create_view_stmt->set_view_updatable(check_is_updatable(select_stmt));
  create_view_stmt->set_select_stmt(select_stmt);
  create_view_stmt->set_query_fields(query_fields);

  if (create_view_stmt->has_duplicate_column_name()) {
    LOG_WARN("duplicate column name in view definition(ERROR 1060)");
    return RC::INVALID_ARGUMENT;
  }

  create_view_stmt->set_view_definition(create_view.description);
  sql_debug("create view statement: view name %s", create_view.view_name.c_str());
  return RC::SUCCESS;
}

bool CreateViewStmt::has_duplicate_column_name()
{
  if (!attrs_name_.empty()) {
    return false;
  }

  std::unordered_map<std::string, int> column_name_map;
  for (auto &field : query_fields_meta_) {
    if (column_name_map.find(field.name()) != column_name_map.end()) {
      return true;
    }
    column_name_map[field.name()] = 1;
  }
  return false;
}
