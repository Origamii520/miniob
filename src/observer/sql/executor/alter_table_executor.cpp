#include "sql/executor/alter_table_executor.h"

#include "common/log/log.h"
#include "event/sql_event.h"
#include "sql/stmt/alter_table_stmt.h"
#include "storage/table/table.h"

RC AlterTableExecutor::execute(SQLStageEvent *sql_event)
{
  Stmt *stmt = sql_event->stmt();
  ASSERT(stmt->type() == StmtType::ALTER_TABLE,
      "alter table executor can not run this command: %d",
      static_cast<int>(stmt->type()));

  auto *alter_table_stmt = static_cast<AlterTableStmt *>(stmt);
  if (alter_table_stmt->operation() == AlterTableStmt::Operation::ADD_COLUMN) {
    return alter_table_stmt->table()->add_column(alter_table_stmt->attr_info());
  }
  if (alter_table_stmt->operation() == AlterTableStmt::Operation::DROP_COLUMN) {
    return alter_table_stmt->table()->drop_column(alter_table_stmt->column_name().c_str());
  }
  return alter_table_stmt->table()->change_column(
      alter_table_stmt->column_name().c_str(), alter_table_stmt->attr_info());
}
