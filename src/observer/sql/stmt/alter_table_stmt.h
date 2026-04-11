#pragma once

#include "sql/stmt/stmt.h"

class Db;
class Table;

class AlterTableStmt : public Stmt
{
public:
  using Operation = AlterTableSqlNode::Operation;

  AlterTableStmt(Table *table, Operation operation, AttrInfoSqlNode attr_info, std::string column_name)
      : table_(table), operation_(operation), attr_info_(std::move(attr_info)), column_name_(std::move(column_name))
  {}
  ~AlterTableStmt() override = default;

  StmtType type() const override { return StmtType::ALTER_TABLE; }

  Table                    *table() const { return table_; }
  Operation                 operation() const { return operation_; }
  const AttrInfoSqlNode    &attr_info() const { return attr_info_; }
  const std::string        &column_name() const { return column_name_; }

  static RC create(Db *db, const AlterTableSqlNode &sql_node, Stmt *&stmt);

private:
  Table          *table_ = nullptr;
  Operation       operation_ = Operation::ADD_COLUMN;
  AttrInfoSqlNode attr_info_;
  std::string     column_name_;
};
