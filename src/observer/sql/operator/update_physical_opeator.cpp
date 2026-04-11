#include "update_physical_opeator.h"
#include "common/log/log.h"
#include "storage/table/view.h"

// 注意，update 的时候不会使用索引
RC UpdatePhysicalOperator::open(Trx *trx)
{
  auto &child = children_[0];
  RC    rc    = child->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("child operator open failed: %s", strrc(rc));
    return rc;
  }
  struct UpdateTarget
  {
    Table           *table      = nullptr;
    const FieldMeta *field_meta = nullptr;
  };
  struct PendingUpdate
  {
    Table                 *table = nullptr;
    RID                    rid;
    std::vector<Value>     cells;
    std::vector<FieldMeta> affected_fields;
    std::vector<int>       field_ids;
  };

  unordered_map<string, Table *> base_table_map;
  vector<UpdateTarget>           update_targets;
  vector<PendingUpdate>          pending_updates;
  update_targets.reserve(field_metas_.size());

  if (table_->is_view()) {
    auto *view = static_cast<View *>(table_);
    for (Table *base_table : view->base_tables()) {
      base_table_map[base_table->name()] = base_table;
    }

    for (const FieldMeta &field_meta : field_metas_) {
      string view_field_name = field_meta.name();
      string base_table_name = view->find_base_table_name(view_field_name);
      string base_field_name = view->find_base_table_field_name(view_field_name);
      if (base_field_name.empty()) {
        base_field_name = view_field_name;
      }

      auto iter = base_table_map.find(base_table_name);
      if (iter == base_table_map.end()) {
        LOG_WARN("failed to find base table for view field. view=%s, field=%s", table_->name(), view_field_name.c_str());
        child->close();
        return RC::SCHEMA_TABLE_NOT_EXIST;
      }

      const FieldMeta *base_field_meta = iter->second->table_meta().field(base_field_name.c_str());
      if (base_field_meta == nullptr) {
        LOG_WARN("failed to find base field for view field. table=%s, field=%s",
            iter->second->name(),
            base_field_name.c_str());
        child->close();
        return RC::SCHEMA_FIELD_NOT_EXIST;
      }

      update_targets.push_back({iter->second, base_field_meta});
    }
  } else {
    for (const FieldMeta &field_meta : field_metas_) {
      update_targets.push_back({table_, &field_meta});
    }
  }

  auto build_pending_update = [this, &update_targets](RowTuple &tuple, Table *target_table, const RID &target_rid,
                                 PendingUpdate &pending_update) -> RC {
    pending_update.table = target_table;
    pending_update.rid   = target_rid;
    pending_update.cells.clear();
    pending_update.affected_fields.clear();
    pending_update.field_ids.clear();
    pending_update.cells.reserve(exprs_.size());
    pending_update.affected_fields.reserve(exprs_.size());
    pending_update.field_ids.reserve(exprs_.size());

    for (size_t i = 0; i < exprs_.size(); i++) {
      const FieldMeta &target_field_meta = *update_targets[i].field_meta;

      Value cell;
      RC    rc = exprs_[i]->get_value(tuple, cell);

      if (rc == RC::RECORD_EOF && target_field_meta.nullable()) {
        cell.set_null();
      } else if (OB_FAIL(rc)) {
        LOG_WARN("cannot get value from expression: %s", strrc(rc));
        return rc;
      }

      if (cell.is_null() && !target_field_meta.nullable()) {
        LOG_WARN("field %s is not nullable, but the value is null", target_field_meta.name());
        return RC::INVALID_ARGUMENT;
      }

      if (exprs_[i]->type() == ExprType::SUB_QUERY) {
        Value test_cell_;
        RC    rc_ = exprs_[i]->get_value(tuple, test_cell_);
        if (rc_ != RC::RECORD_EOF) {
          LOG_WARN("update: subquery should return only one value");
          return RC::INVALID_ARGUMENT;
        }
      }

      if (!cell.is_null() && cell.attr_type() != target_field_meta.type()) {
        Value casted_cell;
        rc = Value::cast_to(cell, target_field_meta.type(), casted_cell);
        if (OB_FAIL(rc)) {
          LOG_WARN("cannot cast from %s to %s",
              attr_type_to_string(cell.attr_type()),
              attr_type_to_string(target_field_meta.type()));
          return RC::INVALID_ARGUMENT;
        }
        cell = casted_cell;
      }

      if (cell.attr_type() == AttrType::VECTORS &&
          static_cast<size_t>(cell.length()) != target_field_meta.vector_dim() * sizeof(float)) {
        LOG_WARN("vector length exceeds limit: %d != %d", cell.data_length() / 4, target_field_meta.vector_dim());
        return RC::INVALID_ARGUMENT;
      }

      pending_update.cells.push_back(cell);
      pending_update.affected_fields.push_back(target_field_meta);
      pending_update.field_ids.push_back(target_field_meta.field_id());
    }

    return RC::SUCCESS;
  };

  while (OB_SUCC(rc = child->next())) {
    Tuple *tuple_ = child->current_tuple();  // 获得当前正在更新的 tuple
    auto   tuple  = dynamic_cast<RowTuple *>(tuple_);
    ASSERT(tuple != nullptr, "tuple cannot cast to RowTuple here!");
    Table *target_table = update_targets.empty() ? table_ : update_targets.front().table;
    if (table_->is_view()) {
      for (const auto &target : update_targets) {
        if (target.table != target_table) {
          LOG_WARN("updating multi-table views is unsupported. view=%s", table_->name());
          child->close();
          return RC::UNIMPLEMENTED;
        }
      }
    }

    RID target_rid = tuple->record().rid();
    if (table_->is_view()) {
      bool found_rid = false;
      for (size_t i = 0; i < tuple->rid_list_.size() && i < tuple->table_name_list_.size(); i++) {
        if (tuple->table_name_list_[i] == target_table->name()) {
          target_rid = tuple->rid_list_[i];
          found_rid  = true;
          break;
        }
      }
      if (!found_rid) {
        LOG_WARN("failed to find base rid for view update. view=%s, base_table=%s", table_->name(), target_table->name());
        child->close();
        return RC::RECORD_NOT_EXIST;
      }
    }

    if (table_->is_view()) {
      PendingUpdate pending_update;
      rc = build_pending_update(*tuple, target_table, target_rid, pending_update);
      if (OB_FAIL(rc)) {
        child->close();
        return rc;
      }
      pending_updates.emplace_back(std::move(pending_update));
      continue;
    }

    rc = target_table->visit_record(target_rid, [this, &update_targets, &build_pending_update, tuple, target_table, target_rid](
                                                   Record &record) {
      PendingUpdate pending_update;
      RC            rc = build_pending_update(*tuple, target_table, target_rid, pending_update);
      if (OB_FAIL(rc)) {
        return rc;
      }

      Record   old_record(record);
      RowTuple base_tuple;
      base_tuple.set_schema(target_table, target_table->table_meta().field_metas());
      base_tuple.set_record(&record);

      for (size_t i = 0; i < pending_update.cells.size(); ++i) {
        base_tuple.set_cell_at(pending_update.field_ids[i], pending_update.cells[i], record.data());
      }

      return target_table->update_index(old_record, record, pending_update.affected_fields);
    });

    if (rc != RC::SUCCESS) {
      child->close();
      return rc;
    }
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }

  child->close();
  if (OB_FAIL(rc)) {
    LOG_WARN("update index failed: %s", strrc(rc));
    return rc;
  }

  for (PendingUpdate &pending_update : pending_updates) {
    rc = pending_update.table->visit_record(pending_update.rid, [&pending_update](Record &record) {
      Record   old_record(record);
      RowTuple base_tuple;
      base_tuple.set_schema(pending_update.table, pending_update.table->table_meta().field_metas());
      base_tuple.set_record(&record);

      for (size_t i = 0; i < pending_update.cells.size(); ++i) {
        base_tuple.set_cell_at(pending_update.field_ids[i], pending_update.cells[i], record.data());
      }

      return pending_update.table->update_index(old_record, record, pending_update.affected_fields);
    });
    if (OB_FAIL(rc)) {
      LOG_WARN("update index failed: %s", strrc(rc));
      return rc;
    }
  }

  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::close() { return RC::SUCCESS; }

RC UpdatePhysicalOperator::next() { return RC::RECORD_EOF; }
