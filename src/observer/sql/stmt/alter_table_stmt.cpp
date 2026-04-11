#include "sql/stmt/alter_table_stmt.h"

#include "common/log/log.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

RC AlterTableStmt::create(Db *db, const AlterTableSqlNode &sql_node, Stmt *&stmt)
{
  stmt = nullptr;

  Table *table = db->find_table(sql_node.relation_name.c_str());
  if (table == nullptr) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), sql_node.relation_name.c_str());
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  AttrInfoSqlNode attr_info = sql_node.attr_info;
  if (attr_info.type == AttrType::VECTORS) {
    attr_info.dim     = attr_info.arr_len;
    attr_info.arr_len = 1;
  }

  if (table->table_meta().field(attr_info.name.c_str()) != nullptr) {
    LOG_WARN("field already exists. table=%s, field=%s", table->name(), attr_info.name.c_str());
    return RC::INVALID_ARGUMENT;
  }

  stmt = new AlterTableStmt(table, std::move(attr_info));
  return RC::SUCCESS;
}
