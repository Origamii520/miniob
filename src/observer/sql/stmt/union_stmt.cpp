#include "sql/stmt/union_stmt.h"

#include "common/log/log.h"
#include "storage/db/db.h"

namespace {

RC collect_value_types(const Stmt &stmt, std::vector<AttrType> &value_types)
{
  value_types.clear();
  if (stmt.type() == StmtType::SELECT) {
    const auto &select_stmt = static_cast<const SelectStmt &>(stmt);
    for (const auto &expression : select_stmt.query_expressions_) {
      value_types.push_back(expression->value_type());
    }
    return RC::SUCCESS;
  }

  if (stmt.type() == StmtType::UNION) {
    const auto &union_stmt = static_cast<const UnionStmt &>(stmt);
    value_types            = union_stmt.value_types();
    return RC::SUCCESS;
  }

  LOG_WARN("unsupported stmt type for union value types. type=%d", static_cast<int>(stmt.type()));
  return RC::INVALID_ARGUMENT;
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

  if ((left_stmt->type() != StmtType::SELECT && left_stmt->type() != StmtType::UNION) ||
      (right_stmt->type() != StmtType::SELECT && right_stmt->type() != StmtType::UNION)) {
    LOG_WARN("union only supports select/union statements");
    delete left_stmt;
    delete right_stmt;
    return RC::INVALID_ARGUMENT;
  }

  std::unique_ptr<Stmt> left_union_stmt(left_stmt);
  std::unique_ptr<Stmt> right_union_stmt(right_stmt);

  std::vector<AttrType> left_types;
  std::vector<AttrType> right_types;
  RC                    type_rc = collect_value_types(*left_union_stmt, left_types);
  if (OB_FAIL(type_rc)) {
    return type_rc;
  }
  type_rc = collect_value_types(*right_union_stmt, right_types);
  if (OB_FAIL(type_rc)) {
    return type_rc;
  }

  if (left_types.size() != right_types.size()) {
    LOG_WARN("union select list size mismatch. left=%zu, right=%zu",
        left_types.size(),
        right_types.size());
    return RC::INVALID_ARGUMENT;
  }

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
      std::move(left_union_stmt), std::move(right_union_stmt), std::move(left_types), sql_node.all);
  return RC::SUCCESS;
}
