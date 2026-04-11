#include "sql/stmt/union_stmt.h"

#include "common/log/log.h"
#include "storage/db/db.h"

namespace {

RC collect_value_types(const SelectStmt &stmt, std::vector<AttrType> &value_types)
{
  value_types.clear();
  for (const auto &expression : stmt.query_expressions_) {
    value_types.push_back(expression->value_type());
  }
  return RC::SUCCESS;
}

}  // namespace

RC UnionStmt::create(Db *db, const UnionSqlNode &sql_node, Stmt *&stmt)
{
  stmt = nullptr;

  if (sql_node.left_select == nullptr || sql_node.right_select == nullptr) {
    LOG_WARN("invalid union sql node");
    return RC::INVALID_ARGUMENT;
  }

  Stmt *left_stmt  = nullptr;
  Stmt *right_stmt = nullptr;
  RC    rc         = Stmt::create_stmt(db, *sql_node.left_select, left_stmt);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create left stmt for union. rc=%s", strrc(rc));
    return rc;
  }

  rc = Stmt::create_stmt(db, *sql_node.right_select, right_stmt);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create right stmt for union. rc=%s", strrc(rc));
    delete left_stmt;
    return rc;
  }

  if (left_stmt->type() != StmtType::SELECT || right_stmt->type() != StmtType::SELECT) {
    LOG_WARN("union only supports select statements");
    delete left_stmt;
    delete right_stmt;
    return RC::INVALID_ARGUMENT;
  }

  auto left_select_stmt  = std::unique_ptr<SelectStmt>(static_cast<SelectStmt *>(left_stmt));
  auto right_select_stmt = std::unique_ptr<SelectStmt>(static_cast<SelectStmt *>(right_stmt));

  if (left_select_stmt->query_expressions_.size() != right_select_stmt->query_expressions_.size()) {
    LOG_WARN("union select list size mismatch. left=%zu, right=%zu",
        left_select_stmt->query_expressions_.size(),
        right_select_stmt->query_expressions_.size());
    return RC::INVALID_ARGUMENT;
  }

  std::vector<AttrType> left_types;
  std::vector<AttrType> right_types;
  collect_value_types(*left_select_stmt, left_types);
  collect_value_types(*right_select_stmt, right_types);

  for (size_t i = 0; i < left_types.size(); i++) {
    if (left_types[i] != right_types[i]) {
      LOG_WARN("union select list type mismatch at index=%zu. left=%d, right=%d",
          i,
          static_cast<int>(left_types[i]),
          static_cast<int>(right_types[i]));
      return RC::INVALID_ARGUMENT;
    }
  }

  stmt = new UnionStmt(
      std::move(left_select_stmt), std::move(right_select_stmt), std::move(left_types), sql_node.all);
  return RC::SUCCESS;
}
