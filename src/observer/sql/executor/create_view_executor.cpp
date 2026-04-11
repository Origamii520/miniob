/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/executor/create_view_executor.h"

#include <string>
#include <vector>

#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"
#include "sql/stmt/create_view_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/table/view.h"
#include "storage/trx/trx.h"

namespace {

constexpr const char *SYS_VIEW_TABLE_NAME = "__miniob_views__";

std::string join_attrs(const std::vector<std::string> &attrs_name)
{
  std::string result;
  for (size_t i = 0; i < attrs_name.size(); i++) {
    if (i != 0) {
      result += ",";
    }
    result += attrs_name[i];
  }
  return result;
}

RC ensure_view_meta_table(Db *db)
{
  if (db->find_table(SYS_VIEW_TABLE_NAME) != nullptr) {
    return RC::SUCCESS;
  }

  std::vector<AttrInfoSqlNode> attr_infos;
  attr_infos.push_back({AttrType::CHARS, "view_name", 256, 0, false});
  attr_infos.push_back({AttrType::TEXTS, "view_description", 1, 0, false});
  attr_infos.push_back({AttrType::INTS, "is_updatable", 1, 0, false});
  attr_infos.push_back({AttrType::TEXTS, "attrs_str", 1, 0, false});
  return db->create_table(SYS_VIEW_TABLE_NAME, attr_infos);
}

}  // namespace

RC CreateViewExecutor::execute(SQLStageEvent *sql_event)
{
  Stmt    *stmt    = sql_event->stmt();
  Session *session = sql_event->session_event()->session();
  ASSERT(stmt->type() == StmtType::CREATE_VIEW,
      "create view executor can not run this command: %d",
      static_cast<int>(stmt->type()));

  auto *create_view_stmt = static_cast<CreateViewStmt *>(stmt);
  Db   *db               = session->get_current_db();

  RC rc = ensure_view_meta_table(db);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create system view table. rc=%s", strrc(rc));
    return rc;
  }

  Table *view_meta_table = db->find_table(SYS_VIEW_TABLE_NAME);
  ASSERT(view_meta_table != nullptr, "view meta table should exist");

  std::string attrs_str = join_attrs(create_view_stmt->attrs_name());
  Value       values[]  = {
      Value(create_view_stmt->view_name().c_str()),
      Value(create_view_stmt->view_definition().c_str()),
      Value(static_cast<int>(create_view_stmt->is_view_updatable())),
      Value(attrs_str.c_str()),
  };

  Record record;
  rc = view_meta_table->make_record(static_cast<int>(sizeof(values) / sizeof(values[0])), values, record);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to make record for view meta. rc=%s", strrc(rc));
    return rc;
  }

  Trx *trx = session->current_trx();
  rc       = trx->start_if_need();
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to start trx for create view. rc=%s", strrc(rc));
    return rc;
  }

  rc = trx->insert_record(view_meta_table, record);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to persist view meta. rc=%s", strrc(rc));
    return rc;
  }

  rc = db->add_view(create_view_stmt->view_name().c_str(),
      create_view_stmt->attrs_name(),
      create_view_stmt->view_definition().c_str(),
      create_view_stmt->is_view_updatable());
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to add view to db. rc=%s", strrc(rc));
    return rc;
  }

  View *view = db->find_view(create_view_stmt->view_name().c_str());
  ASSERT(view != nullptr, "view should exist after add_view");
  view->init_table_meta(create_view_stmt->query_fields_meta());
  view->set_base_tables(create_view_stmt->select_stmt()->tables());
  return RC::SUCCESS;
}
