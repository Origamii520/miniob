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
  return alter_table_stmt->table()->add_column(alter_table_stmt->attr_info());
}
