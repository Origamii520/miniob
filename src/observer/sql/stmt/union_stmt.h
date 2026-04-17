#pragma once

#include <memory>
#include <vector>

#include "common/type/attr_type.h"
#include "sql/stmt/select_stmt.h"

class Db;

class UnionStmt : public Stmt
{
public:
  UnionStmt(std::unique_ptr<Stmt> left_stmt, std::unique_ptr<Stmt> right_stmt,
      std::vector<AttrType> value_types, bool all)
      : left_stmt_(std::move(left_stmt)), right_stmt_(std::move(right_stmt)), value_types_(std::move(value_types)), all_(all)
  {}
  ~UnionStmt() override = default;

  StmtType type() const override { return StmtType::UNION; }

  Stmt                        *left_stmt() const { return left_stmt_.get(); }
  Stmt                        *right_stmt() const { return right_stmt_.get(); }
  const std::vector<AttrType> &value_types() const { return value_types_; }
  bool                         all() const { return all_; }

  static RC create(Db *db, const UnionSqlNode &sql_node, Stmt *&stmt);

private:
  std::unique_ptr<Stmt>       left_stmt_;
  std::unique_ptr<Stmt>       right_stmt_;
  std::vector<AttrType>       value_types_;
  bool                        all_ = false;
};
