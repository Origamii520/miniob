

// 实现了 UpdateStmt 类，用于处理 SQL UPDATE 语句的逻辑。
//通过验证目标表和字段的合法性，绑定更新表达式，处理更新值中的子查询，以及根据 WHERE 子句生成过滤条件，确保更新操作的语义正确性和执行安全性。
//代码采用模块化设计，结合智能指针和详细的错误处理机制，提高了内存管理的安全性和系统的可维护性。
#include "sql/stmt/update_stmt.h"
#include "sql/parser/expression_binder.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"
#include "sql/stmt/select_stmt.h"

UpdateStmt::UpdateStmt(
    Table *table, std::vector<FieldMeta> attrs, std::vector<std::unique_ptr<Expression>> exprs, FilterStmt *stmt)
    : table_(table), field_metas_(std::move(attrs)), exprs_(std::move(exprs)), filter_stmt_(stmt)
{}

RC UpdateStmt::create(Db *db, UpdateSqlNode &update, Stmt *&stmt)
{
  // 检查表名是否存在
  Table *table = db->find_table(update.relation_name.c_str());
  if (table == nullptr) {
    LOG_WARN("table %s doesn't exit", update.relation_name.c_str());
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  // 带聚合等的 View 不可插入
  if (table->is_view()) {
    auto *view = static_cast<View *>(table);
    if (!view->is_updatable()) {
      LOG_WARN("the target table(view) of the INSERT is not updatable");
      return RC::INVALID_ARGUMENT;
    }
  }

  TableMeta     meta = table->table_meta();
  BinderContext context;
  context.add_table(table);
  ExpressionBinder               binder(context);
  vector<unique_ptr<Expression>> bound_expressions;
  std::vector<FieldMeta>         field_metas;
  for (const auto &[attr, expr] : update.update_infos) {

    // 检查要更新的字段是否存在
    auto field_meta = meta.field(attr.c_str());
    if (field_meta == nullptr) {
      LOG_WARN("field %s not found in table %s", attr.c_str(), table->name());
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }

    // 子查询（update-select）。创建 stmt。
    if (expr->type() == ExprType::SUB_QUERY) {
      SubqueryExpr *subquery_expr = static_cast<SubqueryExpr *>(expr);
      Stmt         *stmt          = nullptr;
      RC            rc            = SelectStmt::create(db, subquery_expr->sub_query_sn()->selection, stmt);
      if (rc != RC::SUCCESS) {
        LOG_WARN("update: cannot construct subquery stmt");
        return rc;
      }
      // 检查子查询的合法性：子查询的查询的属性只能有一个
      RC rc_ = check_sub_select_legal(db, subquery_expr->sub_query_sn());
      if (rc_ != RC::SUCCESS) {
        return rc_;
      }
      subquery_expr->set_stmt(unique_ptr<SelectStmt>(static_cast<SelectStmt *>(stmt)));
    }

    field_metas.push_back(*field_meta);
    std::unique_ptr<Expression> exprp(expr);
    RC                          rc = binder.bind_expression(exprp, bound_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }
  std::unordered_map<std::string, Table *> table_map   = {{update.relation_name, table}};
  FilterStmt                              *filter_stmt = nullptr;
  RC rc = FilterStmt::create(db, table, &table_map, update.conditions, filter_stmt);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("cannot construct filter stmt");
    return rc;
  }
  stmt = new UpdateStmt(table, std::move(field_metas), std::move(bound_expressions), filter_stmt);
  return RC::SUCCESS;
}
