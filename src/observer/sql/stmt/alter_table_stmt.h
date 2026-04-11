#pragma once

#include "sql/stmt/stmt.h"

class Db;
class Table;

class AlterTableStmt : public Stmt
{
public:
  AlterTableStmt(Table *table, AttrInfoSqlNode attr_info) : table_(table), attr_info_(std::move(attr_info)) {}
  ~AlterTableStmt() override = default;

  StmtType type() const override { return StmtType::ALTER_TABLE; }

  Table                    *table() const { return table_; }
  const AttrInfoSqlNode    &attr_info() const { return attr_info_; }

  static RC create(Db *db, const AlterTableSqlNode &sql_node, Stmt *&stmt);

private:
  Table          *table_ = nullptr;
  AttrInfoSqlNode attr_info_;
};
