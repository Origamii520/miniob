#include "sql/stmt/drop_index_stmt.h"

#include "common/log/log.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

RC DropIndexStmt::create(Db *db, const DropIndexSqlNode &sql_node, Stmt *&stmt)
{
  stmt = nullptr;

  Table *table = db->find_table(sql_node.relation_name.c_str());
  if (table == nullptr) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), sql_node.relation_name.c_str());
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  Index *index = table->find_index(sql_node.index_name.c_str());
  if (index == nullptr) {
    LOG_WARN("no such index. table=%s, index=%s", table->name(), sql_node.index_name.c_str());
    return RC::NOT_EXIST;
  }

  stmt = new DropIndexStmt(table, sql_node.index_name);
  return RC::SUCCESS;
}
