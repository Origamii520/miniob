/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Meiyi & Wangyunlai on 2021/5/13.
//

#include <algorithm>
#include <climits>
#include <cstring>
#include <cmath>

#include "common/defs.h"
#include "common/lang/string.h"
#include "common/lang/span.h"
#include "common/lang/algorithm.h"
#include "common/lang/bitmap.h"
#include "common/log/log.h"
#include "common/global_context.h"
#include "storage/db/db.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/common/condition_filter.h"
#include "storage/common/meta_util.h"
#include "storage/index/bplus_tree_index.h"
#include "storage/index/index.h"
#include "storage/record/record_manager.h"
#include "storage/table/table.h"
#include "storage/table/vector_data_manager.h"
#include "storage/index/vector_index_meta.h"
#include "storage/index/vector_index.h"
#include "storage/trx/trx.h"
#include "sql/expr/tuple.h"

namespace {

int visible_field_num(const TableMeta &table_meta)
{
  return table_meta.field_num() - table_meta.sys_field_num();
}

RC persist_table_meta_to_disk(const string &base_dir, const char *table_name, const TableMeta &table_meta)
{
  string  tmp_file = table_meta_file(base_dir.c_str(), table_name) + ".tmp";
  fstream fs;
  fs.open(tmp_file, ios_base::out | ios_base::binary | ios_base::trunc);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s", tmp_file.c_str(), strerror(errno));
    return RC::IOERR_OPEN;
  }
  if (table_meta.serialize(fs) < 0) {
    LOG_ERROR("Failed to dump table meta to file: %s. sys err=%d:%s", tmp_file.c_str(), errno, strerror(errno));
    fs.close();
    return RC::IOERR_WRITE;
  }
  fs.close();

  string meta_file = table_meta_file(base_dir.c_str(), table_name);
  int    ret       = rename(tmp_file.c_str(), meta_file.c_str());
  if (ret != 0) {
    LOG_ERROR("Failed to rename tmp meta file (%s) to normal meta file (%s). system error=%d:%s",
        tmp_file.c_str(),
        meta_file.c_str(),
        errno,
        strerror(errno));
    return RC::IOERR_WRITE;
  }
  return RC::SUCCESS;
}

RC materialize_record_with_added_column(
    const TableMeta &old_table_meta, const TableMeta &new_table_meta, const Record &old_record, const AttrInfoSqlNode &attr_info,
    vector<char> &new_record)
{
  new_record.assign(new_table_meta.record_size(), 0);

  for (int i = 0; i < old_table_meta.sys_field_num(); i++) {
    const FieldMeta *old_field = old_table_meta.field(i);
    const FieldMeta *new_field = new_table_meta.field(i);
    memcpy(new_record.data() + new_field->offset(), old_record.data() + old_field->offset(), old_field->len());
  }

  Bitmap old_bitmap(old_record.data() + old_table_meta.null_bitmap_start(), visible_field_num(old_table_meta));
  Bitmap new_bitmap(new_record.data() + new_table_meta.null_bitmap_start(), visible_field_num(new_table_meta));

  for (int i = old_table_meta.sys_field_num(); i < old_table_meta.field_num(); i++) {
    const FieldMeta *old_field = old_table_meta.field(i);
    const FieldMeta *new_field = new_table_meta.field(old_field->name());
    ASSERT(new_field != nullptr, "field must exist in new table meta");
    memcpy(new_record.data() + new_field->offset(), old_record.data() + old_field->offset(), old_field->len());

    const int old_visible_index = old_field->field_id() - old_table_meta.sys_field_num();
    const int new_visible_index = new_field->field_id() - new_table_meta.sys_field_num();
    if (old_bitmap.get_bit(old_visible_index)) {
      new_bitmap.set_bit(new_visible_index);
    }
  }

  if (attr_info.nullable) {
    const FieldMeta *added_field = new_table_meta.field(attr_info.name.c_str());
    ASSERT(added_field != nullptr, "new field should exist");
    new_bitmap.set_bit(added_field->field_id() - new_table_meta.sys_field_num());
  }

  return RC::SUCCESS;
}

RC materialize_record_without_column(
    const TableMeta &old_table_meta, const TableMeta &new_table_meta, const Record &old_record, const char *dropped_field_name,
    vector<char> &new_record)
{
  new_record.assign(new_table_meta.record_size(), 0);

  for (int i = 0; i < old_table_meta.sys_field_num(); i++) {
    const FieldMeta *old_field = old_table_meta.field(i);
    const FieldMeta *new_field = new_table_meta.field(i);
    memcpy(new_record.data() + new_field->offset(), old_record.data() + old_field->offset(), old_field->len());
  }

  Bitmap old_bitmap(old_record.data() + old_table_meta.null_bitmap_start(), visible_field_num(old_table_meta));
  Bitmap new_bitmap(new_record.data() + new_table_meta.null_bitmap_start(), visible_field_num(new_table_meta));

  for (int i = old_table_meta.sys_field_num(); i < old_table_meta.field_num(); i++) {
    const FieldMeta *old_field = old_table_meta.field(i);
    if (strcmp(old_field->name(), dropped_field_name) == 0) {
      continue;
    }

    const FieldMeta *new_field = new_table_meta.field(old_field->name());
    ASSERT(new_field != nullptr, "field must exist in new table meta");
    memcpy(new_record.data() + new_field->offset(), old_record.data() + old_field->offset(), old_field->len());

    const int old_visible_index = old_field->field_id() - old_table_meta.sys_field_num();
    const int new_visible_index = new_field->field_id() - new_table_meta.sys_field_num();
    if (old_bitmap.get_bit(old_visible_index)) {
      new_bitmap.set_bit(new_visible_index);
    }
  }

  return RC::SUCCESS;
}

void cleanup_indexes(vector<Index *> &indexes)
{
  for (Index *index : indexes) {
    delete index;
  }
  indexes.clear();
}

}  // namespace

Table::~Table()
{
  if (record_handler_ != nullptr) {
    delete record_handler_;
    record_handler_ = nullptr;
  }

  if (data_buffer_pool_ != nullptr) {
    data_buffer_pool_->close_file();
    data_buffer_pool_ = nullptr;
  }

  for (vector<Index *>::iterator it = indexes_.begin(); it != indexes_.end(); ++it) {
    Index *index = *it;
    delete index;
  }
  indexes_.clear();

  LOG_INFO("Table has been closed: %s", name());
}

RC Table::create(Db *db, int32_t table_id, const char *path, const char *name, const char *base_dir,
    span<const AttrInfoSqlNode> attributes, StorageFormat storage_format)
{
  if (table_id < 0) {
    LOG_WARN("invalid table id. table_id=%d, table_name=%s", table_id, name);
    return RC::INVALID_ARGUMENT;
  }

  if (common::is_blank(name)) {
    LOG_WARN("Name cannot be empty");
    return RC::INVALID_ARGUMENT;
  }
  LOG_INFO("Begin to create table %s:%s", base_dir, name);

  if (attributes.size() == 0) {
    LOG_WARN("Invalid arguments. table_name=%s, attribute_count=%d", name, attributes.size());
    return RC::INVALID_ARGUMENT;
  }

  RC rc = RC::SUCCESS;

  // 使用 table_name.table记录一个表的元数据
  // 判断表文件是否已经存在
  int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (EEXIST == errno) {
      LOG_ERROR("Failed to create table file, it has been created. %s, EEXIST, %s", path, strerror(errno));
      return RC::SCHEMA_TABLE_EXIST;
    }
    LOG_ERROR("Create table file failed. filename=%s, errmsg=%d:%s", path, errno, strerror(errno));
    return RC::IOERR_OPEN;
  }

  close(fd);

  // 创建文件
  const vector<FieldMeta> *trx_fields = db->trx_kit().trx_fields();
  if ((rc = table_meta_.init(table_id, name, trx_fields, attributes, storage_format)) != RC::SUCCESS) {
    LOG_ERROR("Failed to init table meta. name:%s, ret:%d", name, rc);
    return rc;  // delete table file
  }

  fstream fs;
  fs.open(path, ios_base::out | ios_base::binary);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s", path, strerror(errno));
    return RC::IOERR_OPEN;
  }

  // 记录元数据到文件中
  table_meta_.serialize(fs);
  fs.close();

  db_       = db;
  base_dir_ = base_dir;

  bool has_text = std::any_of(
      attributes.begin(), attributes.end(), [](AttrInfoSqlNode attr) { return attr.type == AttrType::TEXTS; });
  if (has_text) {
    std::string text_file = table_text_data_file(base_dir, name);
    fd                    = ::open(text_file.c_str(), O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      if (EEXIST == errno) {
        LOG_ERROR("Failed to create text data file, it has been created. %s, EEXIST, %s", text_file.c_str(), strerror(errno));
        return RC::SCHEMA_TABLE_EXIST;
      }
      LOG_ERROR("Create text data file failed. filename=%s, errmsg=%d:%s", text_file.c_str(), errno, strerror(errno));
      return RC::IOERR_OPEN;
    }
    close(fd);
  }
  bool has_vector = std::any_of(
      attributes.begin(), attributes.end(), [](AttrInfoSqlNode attr) { return attr.type == AttrType::VECTORS; });
  if (has_vector) {
    std::string vector_file = table_vector_data_file(base_dir, name);
    fd                      = ::open(vector_file.c_str(), O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      if (EEXIST == errno) {
        LOG_ERROR("Failed to create vector data file, it has been created. %s, EEXIST, %s", vector_file.c_str(), strerror(errno));
        return RC::SCHEMA_TABLE_EXIST;
      }
      LOG_ERROR("Create vector data file failed. filename=%s, errmsg=%d:%s", vector_file.c_str(), errno, strerror(errno));
      return RC::IOERR_OPEN;
    }
    close(fd);
    vector_data_manager_ = VectorDataManager::create(this);
  }

  string             data_file = table_data_file(base_dir, name);
  BufferPoolManager &bpm       = db->buffer_pool_manager();
  rc                           = bpm.create_file(data_file.c_str());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to create disk buffer pool of data file. file name=%s", data_file.c_str());
    return rc;
  }

  rc = init_record_handler(base_dir);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to create table %s due to init record handler failed.", data_file.c_str());
    // don't need to remove the data_file
    return rc;
  }

  LOG_INFO("Successfully create table %s:%s", base_dir, name);
  return rc;
}

RC Table::drop(Db *db, const char *table_name, const char *base_dir)
{
  std::string data_file_path = table_data_file(base_dir, table_name);
  std::string meta_file_path = table_meta_file(base_dir, table_name);
  std::string text_file_path   = table_text_data_file(base_dir_.c_str(), table_meta_.name());
  std::string vector_file_path = table_vector_data_file(base_dir_.c_str(), table_meta_.name());
  // TODO: delete index
  data_buffer_pool_->close_file();
  data_buffer_pool_ = nullptr;  // 防止析构函数中再次尝试关闭文件
  if (unlink(meta_file_path.c_str()) == -1) {
    LOG_ERROR("Failed to remove table metadata file for %s due to %s", meta_file_path.c_str(), strerror(errno));
    return RC::INTERNAL;
  }
  if (unlink(data_file_path.c_str()) == -1) {
    LOG_ERROR("Failed to remove table data file for %s due to %s", meta_file_path.c_str(), strerror(errno));
    return RC::INTERNAL;
  }
  if (unlink(text_file_path.c_str()) == -1) {
    if (errno != ENOENT) {
      LOG_ERROR("Failed to remove text data file for %s due to %s", meta_file_path.c_str(), strerror(errno));
      return RC::INTERNAL;
    }
  }
  if (unlink(vector_file_path.c_str()) == -1) {
    if (errno != ENOENT) {
      LOG_ERROR("Failed to remove vector data file for %s due to %s", meta_file_path.c_str(), strerror(errno));
      return RC::INTERNAL;
    }
  }
  return RC::SUCCESS;
}

RC Table::open(Db *db, const char *meta_file, const char *base_dir)
{
  // 加载元数据文件
  fstream fs;
  string  meta_file_path = string(base_dir) + common::FILE_PATH_SPLIT_STR + meta_file;
  fs.open(meta_file_path, ios_base::in | ios_base::binary);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open meta file for read. file name=%s, errmsg=%s", meta_file_path.c_str(), strerror(errno));
    return RC::IOERR_OPEN;
  }
  if (table_meta_.deserialize(fs) < 0) {
    LOG_ERROR("Failed to deserialize table meta. file name=%s", meta_file_path.c_str());
    fs.close();
    return RC::INTERNAL;
  }
  fs.close();

  db_       = db;
  base_dir_ = base_dir;

  // 加载数据文件
  RC rc = init_record_handler(base_dir);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to open table %s due to init record handler failed.", base_dir);
    // don't need to remove the data_file
    return rc;
  }

  const int index_num = table_meta_.index_num();
  for (int i = 0; i < index_num; i++) {
    const IndexMeta *index_meta = table_meta_.index(i);

    BplusTreeIndex *index      = new BplusTreeIndex();
    string          index_file = table_index_file(base_dir, name(), index_meta->name().c_str());

    rc = index->open(this, index_file.c_str(), *index_meta);
    if (rc != RC::SUCCESS) {
      delete index;
      LOG_ERROR("Failed to open index. table=%s, index=%s, file=%s, rc=%s",
                name(), index_meta->name().c_str(), index_file.c_str(), strrc(rc));
      // skip cleanup
      //  do all cleanup action in destructive Table function.
      return rc;
    }
    indexes_.push_back(index);
  }

  auto field_metas = table_meta_.field_metas();
  bool has_vector  = std::any_of(
      field_metas->begin(), field_metas->end(), [](FieldMeta attr) { return attr.type() == AttrType::VECTORS; });
  if (has_vector) {

    vector_data_manager_ = VectorDataManager::create(this);
  }
  return rc;
}

RC Table::insert_record(Record &record)
{
  RC rc = RC::SUCCESS;
  rc    = record_handler_->insert_record(record.data(), table_meta_.record_size(), &record.rid());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Insert record failed. table name=%s, rc=%s", table_meta_.name(), strrc(rc));
    return rc;
  }

  rc = insert_entry_of_indexes(record.data(), record.rid());
  if (rc != RC::SUCCESS) {  // 可能出现了键值重复
    RC rc2 = record_handler_->delete_record(&record.rid());
    if (rc2 != RC::SUCCESS) {
      LOG_PANIC("Failed to rollback record data when insert index entries failed. table name=%s, rc=%d:%s",
                name(), rc2, strrc(rc2));
    }
  }
  return rc;
}

RC Table::visit_record(const RID &rid, function<RC(Record &)> visitor)
{
  return record_handler_->visit_record(rid, visitor);
}

RC Table::get_record(const RID &rid, Record &record)
{
  RC rc = record_handler_->get_record(rid, record);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to visit record. rid=%s, table=%s, rc=%s", rid.to_string().c_str(), name(), strrc(rc));
    return rc;
  }

  return rc;
}

RC Table::recover_insert_record(Record &record)
{
  RC rc = RC::SUCCESS;
  rc    = record_handler_->recover_insert_record(record.data(), table_meta_.record_size(), record.rid());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Insert record failed. table name=%s, rc=%s", table_meta_.name(), strrc(rc));
    return rc;
  }

  rc = insert_entry_of_indexes(record.data(), record.rid());
  if (rc != RC::SUCCESS) {  // 可能出现了键值重复
    RC rc2 = delete_entry_of_indexes(record.data(), record.rid(), false /*error_on_not_exists*/);
    if (rc2 != RC::SUCCESS) {
      LOG_ERROR("Failed to rollback index data when insert index entries failed. table name=%s, rc=%d:%s",
                name(), rc2, strrc(rc2));
    }
    rc2 = record_handler_->delete_record(&record.rid());
    if (rc2 != RC::SUCCESS) {
      LOG_PANIC("Failed to rollback record data when insert index entries failed. table name=%s, rc=%d:%s",
                name(), rc2, strrc(rc2));
    }
  }
  return rc;
}

const char *Table::name() const { return table_meta_.name(); }

const TableMeta &Table::table_meta() const { return table_meta_; }

RC Table::make_record(int value_num, const Value *values, Record &record)
{
  RC rc = RC::SUCCESS;
  // 检查字段类型是否一致
  if (value_num + table_meta_.sys_field_num() != table_meta_.field_num()) {
    LOG_WARN("Input values don't match the table's schema, table name:%s", table_meta_.name());
    return RC::SCHEMA_FIELD_MISSING;
  }

  const int normal_field_start_index = table_meta_.sys_field_num();
  // 复制所有字段的值
  int   record_size = table_meta_.record_size();
  char *record_data = (char *)malloc(record_size);
  memset(record_data, 0, record_size);

  for (int i = 0; i < value_num && OB_SUCC(rc); i++) {
    const FieldMeta *field = table_meta_.field(i + normal_field_start_index);
    const Value &    value = values[i];
    if (field->type() != value.attr_type()) {
      Value real_value;
      rc = Value::cast_to(value, field->type(), real_value);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to cast value. table name:%s,field name:%s,value:%s ",
            table_meta_.name(), field->name(), value.to_string().c_str());
        break;
      }
      rc = set_value_to_record(record_data, real_value, field);
    } else {
      rc = set_value_to_record(record_data, value, field);
    }
  }
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to make record. table name:%s", table_meta_.name());
    free(record_data);
    return rc;
  }

  record.set_data_owner(record_data, record_size);
  return RC::SUCCESS;
}

RC Table::set_value_to_record(char *record_data, const Value &value, const FieldMeta *field)
{
  auto         bitmap   = common::Bitmap(record_data + table_meta_.null_bitmap_start(), table_meta_.field_num());
  if (value.is_null()) {
    bitmap.set_bit(field->field_id() - table_meta_.sys_field_num());
    return RC::SUCCESS;
  }
  bitmap.clear_bit(field->field_id() - table_meta_.sys_field_num());
  ASSERT(field->type() == value.attr_type(), "field type and value type mismatch");
  size_t copy_len = min(value.data_length(), field->len());
  if (field->type() == AttrType::TEXTS) {
    const auto text_data         = reinterpret_cast<const TextData *>(value.data());
    TextData   text_data_updated = *text_data;
    this->dump_text(&text_data_updated);  // 不能原地修改value 中的TextData.offset
    memcpy(record_data + field->offset(), &text_data_updated, copy_len);
  } else if (field->type() == AttrType::VECTORS) {
    const auto vector_data         = reinterpret_cast<const VectorData *>(value.data());
    VectorData vector_data_updated = *vector_data;
    this->dump_vector(&vector_data_updated);
    memcpy(record_data + field->offset(), &vector_data_updated, copy_len);
  } else {
    memcpy(record_data + field->offset(), value.data(), copy_len);
  }
  return RC::SUCCESS;
}

RC Table::init_record_handler(const char *base_dir)
{
  string data_file = table_data_file(base_dir, table_meta_.name());

  BufferPoolManager &bpm = db_->buffer_pool_manager();
  // 此处初始化 data_buffer_pool_
  RC                 rc  = bpm.open_file(db_->log_handler(), data_file.c_str(), data_buffer_pool_);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to open disk buffer pool for file:%s. rc=%d:%s", data_file.c_str(), rc, strrc(rc));
    return rc;
  }

  record_handler_ = new RecordFileHandler(table_meta_.storage_format());

  rc = record_handler_->init(*data_buffer_pool_, db_->log_handler(), &table_meta_);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to init record handler. rc=%s", strrc(rc));
    data_buffer_pool_->close_file();
    data_buffer_pool_ = nullptr;
    delete record_handler_;
    record_handler_ = nullptr;
    return rc;
  }

  return rc;
}

RC Table::get_record_scanner(RecordFileScanner &scanner, Trx *trx, ReadWriteMode mode)
{
  RC rc = scanner.open_scan(this, *data_buffer_pool_, trx, db_->log_handler(), mode, nullptr);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to open scanner. rc=%s", strrc(rc));
  }
  return rc;
}

RC Table::get_chunk_scanner(ChunkFileScanner &scanner, Trx *trx, ReadWriteMode mode)
{
  RC rc = scanner.open_scan_chunk(this, *data_buffer_pool_, db_->log_handler(), mode);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to open scanner. rc=%s", strrc(rc));
  }
  return rc;
}

RC Table::create_index(
    Trx *trx, const std::vector<const FieldMeta *> &field_metas, const char *index_name, bool is_unique)
{
  if (common::is_blank(index_name) || field_metas.empty()) {
    LOG_INFO("Invalid input arguments, table name is %s, index_name is blank or attribute_name is blank", name());
    return RC::INVALID_ARGUMENT;
  }

  for (const auto &field_meta : field_metas) {
    if (nullptr == field_meta) {
      LOG_INFO("Invalid input arguments, table name is %s, index_name is blank or attribute_name is blank", name());
      return RC::INVALID_ARGUMENT;
    }
  }

  IndexMeta new_index_meta;

  RC rc = new_index_meta.init(index_name, field_metas, is_unique);
  if (rc != RC::SUCCESS) {
    std::string field_names;
    for (size_t i = 0; i < field_metas.size(); i++) {
      if (i > 0) {
        field_names += ", ";
      }
      field_names += field_metas[i]->name();
    }
    LOG_INFO("Failed to init IndexMeta in table:%s, index_name:%s, field_names:%s", 
             name(), index_name, field_names.c_str());
    return rc;
  }

  // 创建索引相关数据
  BplusTreeIndex *index      = new BplusTreeIndex();
  string          index_file = table_index_file(base_dir_.c_str(), name(), index_name);

  rc = index->create(this, index_file.c_str(), new_index_meta);
  if (rc != RC::SUCCESS) {
    delete index;
    LOG_ERROR("Failed to create bplus tree index. file name=%s, rc=%d:%s", index_file.c_str(), rc, strrc(rc));
    return rc;
  }

  // 遍历当前的所有数据，插入这个索引
  RecordFileScanner scanner;
  rc = get_record_scanner(scanner, trx, ReadWriteMode::READ_ONLY);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create scanner while creating index. table=%s, index=%s, rc=%s", 
             name(), index_name, strrc(rc));
    return rc;
  }

  Record record;
  while (OB_SUCC(rc = scanner.next(record))) {
    rc = index->insert_entry(record.data(), &record.rid());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to insert record into index while creating index. table=%s, index=%s, rc=%s",
               name(), index_name, strrc(rc));
      return rc;
    }
  }
  if (RC::RECORD_EOF == rc) {
    rc = RC::SUCCESS;
  } else {
    LOG_WARN("failed to insert record into index while creating index. table=%s, index=%s, rc=%s",
             name(), index_name, strrc(rc));
    return rc;
  }
  scanner.close_scan();
  LOG_INFO("inserted all records into new index. table=%s, index=%s", name(), index_name);

  indexes_.push_back(index);

  /// 接下来将这个索引放到表的元数据中
  TableMeta new_table_meta(table_meta_);
  rc = new_table_meta.add_index(new_index_meta);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to add index (%s) on table (%s). error=%d:%s", index_name, name(), rc, strrc(rc));
    return rc;
  }

  /// 内存中有一份元数据，磁盘文件也有一份元数据。修改磁盘文件时，先创建一个临时文件，写入完成后再rename为正式文件
  /// 这样可以防止文件内容不完整
  // 创建元数据临时文件
  string  tmp_file = table_meta_file(base_dir_.c_str(), name()) + ".tmp";
  fstream fs;
  fs.open(tmp_file, ios_base::out | ios_base::binary | ios_base::trunc);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s", tmp_file.c_str(), strerror(errno));
    return RC::IOERR_OPEN;  // 创建索引中途出错，要做还原操作
  }
  if (new_table_meta.serialize(fs) < 0) {
    LOG_ERROR("Failed to dump new table meta to file: %s. sys err=%d:%s", tmp_file.c_str(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }
  fs.close();

  // 覆盖原始元数据文件
  string meta_file = table_meta_file(base_dir_.c_str(), name());

  int ret = rename(tmp_file.c_str(), meta_file.c_str());
  if (ret != 0) {
    LOG_ERROR("Failed to rename tmp meta file (%s) to normal meta file (%s) while creating index (%s) on table (%s). "
              "system error=%d:%s",
              tmp_file.c_str(), meta_file.c_str(), index_name, name(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }

  table_meta_.swap(new_table_meta);

  LOG_INFO("Successfully added a new index (%s) on the table (%s)", index_name, name());
  return rc;
}

RC Table::drop_index(const char *index_name)
{
  if (common::is_blank(index_name)) {
    LOG_WARN("invalid index name");
    return RC::INVALID_ARGUMENT;
  }

  auto iter = std::find_if(indexes_.begin(), indexes_.end(), [index_name](Index *index) {
    return index->index_meta().name() == index_name;
  });
  if (iter == indexes_.end()) {
    LOG_WARN("index does not exist. table=%s, index=%s", name(), index_name);
    return RC::NOT_EXIST;
  }

  TableMeta new_table_meta(table_meta_);
  RC        rc = new_table_meta.drop_index(index_name);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to drop index meta. table=%s, index=%s, rc=%s", name(), index_name, strrc(rc));
    return rc;
  }

  string  tmp_file = table_meta_file(base_dir_.c_str(), name()) + ".tmp";
  fstream fs;
  fs.open(tmp_file, ios_base::out | ios_base::binary | ios_base::trunc);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s", tmp_file.c_str(), strerror(errno));
    return RC::IOERR_OPEN;
  }
  if (new_table_meta.serialize(fs) < 0) {
    LOG_ERROR("Failed to dump new table meta to file: %s. sys err=%d:%s", tmp_file.c_str(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }
  fs.close();

  string meta_file = table_meta_file(base_dir_.c_str(), name());
  int    ret       = rename(tmp_file.c_str(), meta_file.c_str());
  if (ret != 0) {
    LOG_ERROR("Failed to rename tmp meta file (%s) to normal meta file (%s) while dropping index (%s) on table (%s). "
              "system error=%d:%s",
        tmp_file.c_str(),
        meta_file.c_str(),
        index_name,
        name(),
        errno,
        strerror(errno));
    return RC::IOERR_WRITE;
  }

  Index *index = *iter;
  indexes_.erase(iter);
  delete index;
  table_meta_.swap(new_table_meta);

  string index_file = table_index_file(base_dir_.c_str(), name(), index_name);
  if (unlink(index_file.c_str()) == -1 && errno != ENOENT) {
    LOG_WARN("failed to remove index file. file=%s, errno=%d:%s", index_file.c_str(), errno, strerror(errno));
  }

  LOG_INFO("Successfully dropped index (%s) on the table (%s)", index_name, name());
  return RC::SUCCESS;
}

RC Table::delete_record(const RID &rid)
{
  RC     rc = RC::SUCCESS;
  Record record;
  rc = get_record(rid, record);
  if (OB_FAIL(rc)) {
    return rc;
  }

  return delete_record(record);
}

RC Table::delete_record(const Record &record)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->delete_entry(record.data(), &record.rid());
    ASSERT(RC::SUCCESS == rc, 
           "failed to delete entry from index. table name=%s, index name=%s, rid=%s, rc=%s",
           name(), index->index_meta().name().c_str(), record.rid().to_string().c_str(), strrc(rc));
  }
  rc = record_handler_->delete_record(&record.rid());
  return rc;
}

RC Table::insert_entry_of_indexes(const char *record, const RID &rid)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->insert_entry(record, &rid);
    if (rc != RC::SUCCESS) {
      break;
    }
  }
  return rc;
}

RC Table::delete_entry_of_indexes(const char *record, const RID &rid, bool error_on_not_exists)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->delete_entry(record, &rid);
    if (rc != RC::SUCCESS) {
      if (rc != RC::RECORD_INVALID_KEY || !error_on_not_exists) {
        break;
      }
    }
  }
  return rc;
}

Index *Table::find_index(const char *index_name) const
{
  for (Index *index : indexes_) {
    if (index->index_meta().name() == index_name) {
      return index;
    }
  }
  return nullptr;
}

// 根据字段名找到对应的索引，目前仅支持完全匹配的等值查找
Index *Table::find_index_by_fields(const std::vector<const char *> &field_names) const
{
  const TableMeta &table_meta = this->table_meta();
  const IndexMeta *index_meta = table_meta.find_index_by_fields(field_names);

  if (index_meta == nullptr) {
    return nullptr;
  }
  return this->find_index(index_meta->name().c_str());
}

RC Table::sync()
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->sync();
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to flush index's pages. table=%s, index=%s, rc=%d:%s",
          name(),
          index->index_meta().name().c_str(),
          rc,
          strrc(rc));
      return rc;
    }
  }

  rc = data_buffer_pool_->flush_all_pages();
  LOG_INFO("Sync table over. table=%s", name());
  return rc;
}

std::string Table::text_data_file() const { return table_text_data_file(base_dir_.c_str(), table_meta_.name()); }

std::string Table::vector_data_file() const { return table_vector_data_file(base_dir_.c_str(), table_meta_.name()); }

RC Table::update_index(const Record &old_record, const Record &new_record, const std::vector<FieldMeta> &affectedFields)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    bool need_update = false;
    for (const auto &field : affectedFields) {
      if (index->index_meta().has_field(field.name())) {
        need_update = true;
        break;
      }
    }
    if (need_update) {
      rc = index->update_entry(old_record.data(), new_record.data(), &old_record.rid());
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to update index. table=%s, index=%s, rc=%s", 
                 name(), index->index_meta().name().c_str(), strrc(rc));
        return rc;
      }
    }
  }
  return RC::SUCCESS;
}

RC Table::load_text(TextData *data) const
{
  std::string text_file = text_data_file();
  int         fd        = ::open(text_file.c_str(), O_RDONLY);
  if (fd < 0) {
    LOG_WARN("failed to open table text data file %s: %s", text_file.c_str(), strerror(errno));
    return RC::IOERR_OPEN;
  }
  lseek(fd, data->offset, SEEK_SET);
  size_t offset = 0;
  auto   buffer = new char[data->len + 1];
  while (offset < data->len) {
    size_t readed = read(fd, buffer + offset, data->len - offset);
    if (readed < 0) {
      LOG_WARN("failed to read text data: %s", strerror(errno));
    }
    offset += readed;
  }
  close(fd);
  buffer[data->len] = '\0';
  data->str         = buffer;
  return RC::SUCCESS;
}
RC Table::dump_text(TextData *data) const
{
  // 在文件末尾追加写入 text
  std::string text_file = text_data_file();
  int         fd        = ::open(text_file.c_str(), O_RDWR);
  if (fd < 0) {
    LOG_WARN("failed to open table text data file %s: %s", text_file.c_str(), strerror(errno));
    return RC::IOERR_OPEN;
  }
  size_t end    = lseek(fd, 0, SEEK_END);
  size_t offset = 0;
  while (offset < data->len) {
    size_t writed = write(fd, data->str + offset, data->len - offset);
    if (writed < 0) {
      LOG_WARN("failed to write text data %s", strerror(errno));
      return RC::IOERR_WRITE;
    }
    offset += writed;
  }
  close(fd);
  data->offset = end;
  return RC::SUCCESS;
}

RC Table::load_vector(VectorData *data) const
{
  ASSERT(vector_data_manager_ != nullptr, "table %s has no vector attribute", this->name());
  return vector_data_manager_->load_vector(data);
}
RC Table::dump_vector(VectorData *data) const
{
  ASSERT(vector_data_manager_ != nullptr, "table %s has no vector attribute", this->name());
  return vector_data_manager_->dump_vector(data);
}
RC Table::update_vector(const VectorData *old_vector_data, const VectorData *new_vector_data) const
{
  ASSERT(vector_data_manager_ != nullptr, "table %s has no vector attribute", this->name());
  return vector_data_manager_->update_vector(old_vector_data, new_vector_data);
}

RC Table::create_vector_index(Trx *trx, const FieldMeta *field_meta, const std::string &vector_index_name,
    DistanceType distance_type, size_t lists, size_t probes)
{
  VectorIndexMeta new_vector_index_meta;
  RC              rc = new_vector_index_meta.init(vector_index_name, *field_meta, distance_type, lists, probes);
  if (OB_FAIL(rc)) {
    LOG_ERROR("Failed to create vector index. rc=%d:%s", rc, strrc(rc));
    return rc;
  }
  std::string vector_index_file = table_vector_index_file(base_dir_.c_str(), name(), vector_index_name.c_str());
  auto        vector_index =
      new VectorIndex(new_vector_index_meta, field_meta->vector_dim(), distance_type, lists, probes, vector_index_file);
  RecordFileScanner scanner;
  rc = get_record_scanner(scanner, trx, ReadWriteMode::READ_ONLY);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create scanner while creating vector index. table=%s, index=%s, rc=%s",
         name(), vector_index_name, strrc(rc));
    return rc;
  }
  Record   record;
  RowTuple tuple;
  tuple.set_schema(this, table_meta_.field_metas());
  while (OB_SUCC(rc = scanner.next(record))) {
    tuple.set_record(&record);
    Value cell;
    tuple.cell_at(field_meta->field_id(), cell);
    vector_index->add_item(record.rid(), cell.get_vector().vector);
  }
  if (RC::RECORD_EOF == rc) {
    rc = RC::SUCCESS;
  } else {
    LOG_WARN("failed to insert record into index while creating index. table=%s, index=%s, rc=%s",
             name(), vector_index_name, strrc(rc));
    return rc;
  }
  scanner.close_scan();
  rc = vector_index->build_and_save();
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to build and save vector index: %s", strrc(rc));
    return rc;
  }
  vector_indexes_.push_back(vector_index);

  /// 接下来将这个索引放到表的元数据中
  TableMeta new_table_meta(table_meta_);
  rc = new_table_meta.add_vector_index(new_vector_index_meta);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to add vector index (%s) on table (%s). error=%d:%s", vector_index_name, name(), rc, strrc(rc));
    return rc;
  }

  /// 内存中有一份元数据，磁盘文件也有一份元数据。修改磁盘文件时，先创建一个临时文件，写入完成后再rename为正式文件
  /// 这样可以防止文件内容不完整
  // 创建元数据临时文件
  string  tmp_file = table_meta_file(base_dir_.c_str(), name()) + ".tmp";
  fstream fs;
  fs.open(tmp_file, ios_base::out | ios_base::binary | ios_base::trunc);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s", tmp_file.c_str(), strerror(errno));
    return RC::IOERR_OPEN;  // 创建索引中途出错，要做还原操作
  }
  if (new_table_meta.serialize(fs) < 0) {
    LOG_ERROR("Failed to dump new table meta to file: %s. sys err=%d:%s", tmp_file.c_str(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }
  fs.close();

  // 覆盖原始元数据文件
  string meta_file = table_meta_file(base_dir_.c_str(), name());

  int ret = rename(tmp_file.c_str(), meta_file.c_str());
  if (ret != 0) {
    LOG_ERROR("Failed to rename tmp meta file (%s) to normal meta file (%s) while creating vector index (%s) on table (%s). "
              "system error=%d:%s",
              tmp_file.c_str(), meta_file.c_str(), vector_index_name, name(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }

  table_meta_.swap(new_table_meta);

  LOG_INFO("Successfully added a new vector index (%s) on the table (%s)", vector_index_name.c_str(), name());
  return RC::SUCCESS;
}

RC Table::add_column(const AttrInfoSqlNode &attr_info)
{
  if (table_meta_.field(attr_info.name.c_str()) != nullptr) {
    LOG_WARN("field already exists. table=%s, field=%s", name(), attr_info.name.c_str());
    return RC::INVALID_ARGUMENT;
  }

  if (table_meta_.vector_index_num() > 0) {
    LOG_WARN("alter table add column does not support tables with vector indexes yet. table=%s", name());
    return RC::UNIMPLEMENTED;
  }

  TableMeta old_table_meta(table_meta_);
  TableMeta new_table_meta(table_meta_);
  RC        rc = new_table_meta.add_field(attr_info);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to append field to table meta. table=%s, field=%s, rc=%s",
        name(),
        attr_info.name.c_str(),
        strrc(rc));
    return rc;
  }

  vector<IndexMeta> index_metas;
  index_metas.reserve(old_table_meta.index_num());
  for (int i = 0; i < old_table_meta.index_num(); i++) {
    index_metas.push_back(*old_table_meta.index(i));
  }
  new_table_meta.clear_indexes();

  vector<vector<char>> materialized_records;
  Trx                 *scan_trx = db_->trx_kit().create_trx(db_->log_handler());
  RecordFileScanner    scanner;
  rc = get_record_scanner(scanner, scan_trx, ReadWriteMode::READ_ONLY);
  if (OB_FAIL(rc)) {
    if (scan_trx != nullptr) {
      db_->trx_kit().destroy_trx(scan_trx);
    }
    LOG_WARN("failed to open scanner while altering table. table=%s, rc=%s", name(), strrc(rc));
    return rc;
  }

  Record record;
  while (OB_SUCC(rc = scanner.next(record))) {
    vector<char> new_record;
    rc = materialize_record_with_added_column(old_table_meta, new_table_meta, record, attr_info, new_record);
    if (OB_FAIL(rc)) {
      scanner.close_scan();
      if (scan_trx != nullptr) {
        db_->trx_kit().destroy_trx(scan_trx);
      }
      LOG_WARN("failed to materialize altered record. table=%s, rc=%s", name(), strrc(rc));
      return rc;
    }
    materialized_records.emplace_back(std::move(new_record));
  }
  scanner.close_scan();
  if (scan_trx != nullptr) {
    db_->trx_kit().destroy_trx(scan_trx);
  }

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("failed to scan table records while altering table. table=%s, rc=%s", name(), strrc(rc));
    return rc;
  }

  rc = sync();
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to sync table before altering. table=%s, rc=%s", name(), strrc(rc));
    return rc;
  }

  vector<string> index_names;
  index_names.reserve(index_metas.size());
  for (const IndexMeta &index_meta : index_metas) {
    index_names.push_back(index_meta.name());
  }

  cleanup_indexes(indexes_);
  if (record_handler_ != nullptr) {
    delete record_handler_;
    record_handler_ = nullptr;
  }
  if (data_buffer_pool_ != nullptr) {
    data_buffer_pool_->close_file();
    data_buffer_pool_ = nullptr;
  }

  const string data_file = table_data_file(base_dir_.c_str(), name());
  if (unlink(data_file.c_str()) == -1) {
    LOG_WARN("failed to remove old data file. file=%s, errno=%d:%s", data_file.c_str(), errno, strerror(errno));
    return RC::FILE_REMOVE;
  }
  for (const string &index_name : index_names) {
    const string index_file = table_index_file(base_dir_.c_str(), name(), index_name.c_str());
    if (unlink(index_file.c_str()) == -1 && errno != ENOENT) {
      LOG_WARN("failed to remove old index file. file=%s, errno=%d:%s", index_file.c_str(), errno, strerror(errno));
      return RC::FILE_REMOVE;
    }
  }

  if (attr_info.type == AttrType::TEXTS) {
    const string text_file = table_text_data_file(base_dir_.c_str(), name());
    int          fd        = ::open(text_file.c_str(), O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0) {
      close(fd);
    } else if (errno != EEXIST) {
      LOG_WARN("failed to create text data file. file=%s, errno=%d:%s", text_file.c_str(), errno, strerror(errno));
      return RC::IOERR_OPEN;
    }
  }
  if (attr_info.type == AttrType::VECTORS) {
    const string vector_file = table_vector_data_file(base_dir_.c_str(), name());
    int          fd          = ::open(vector_file.c_str(), O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0) {
      close(fd);
    } else if (errno != EEXIST) {
      LOG_WARN("failed to create vector data file. file=%s, errno=%d:%s", vector_file.c_str(), errno, strerror(errno));
      return RC::IOERR_OPEN;
    }
  }

  BufferPoolManager &bpm = db_->buffer_pool_manager();
  rc                     = bpm.create_file(data_file.c_str());
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create new data file while altering table. file=%s, rc=%s", data_file.c_str(), strrc(rc));
    return rc;
  }

  table_meta_.swap(new_table_meta);
  if (attr_info.type == AttrType::VECTORS && vector_data_manager_ == nullptr) {
    vector_data_manager_ = VectorDataManager::create(this);
  }

  rc = init_record_handler(base_dir_.c_str());
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to reinit record handler while altering table. table=%s, rc=%s", name(), strrc(rc));
    return rc;
  }

  for (const vector<char> &new_record_data : materialized_records) {
    Record new_record;
    rc = new_record.copy_data(new_record_data.data(), static_cast<int>(new_record_data.size()));
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to copy rewritten record. table=%s, rc=%s", name(), strrc(rc));
      return rc;
    }
    rc = insert_record(new_record);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to insert rewritten record. table=%s, rc=%s", name(), strrc(rc));
      return rc;
    }
  }

  rc = persist_table_meta_to_disk(base_dir_, name(), table_meta_);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to persist altered table meta. table=%s, rc=%s", name(), strrc(rc));
    return rc;
  }

  for (const IndexMeta &index_meta : index_metas) {
    vector<const FieldMeta *> field_metas;
    field_metas.reserve(index_meta.field_metas().size());
    for (const FieldMeta &field_meta : index_meta.field_metas()) {
      const FieldMeta *new_field_meta = table_meta_.field(field_meta.name());
      if (new_field_meta == nullptr) {
        LOG_WARN("failed to find field for rebuilding index. table=%s, field=%s", name(), field_meta.name());
        return RC::SCHEMA_FIELD_NOT_EXIST;
      }
      field_metas.push_back(new_field_meta);
    }

    rc = create_index(nullptr, field_metas, index_meta.name().c_str(), index_meta.is_unique());
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to rebuild index while altering table. table=%s, index=%s, rc=%s",
          name(),
          index_meta.name().c_str(),
          strrc(rc));
      return rc;
    }
  }

  LOG_INFO("Successfully added column (%s) on table (%s)", attr_info.name.c_str(), name());
  return RC::SUCCESS;
}

RC Table::drop_column(const char *field_name)
{
  const FieldMeta *field_meta = table_meta_.field(field_name);
  if (field_meta == nullptr || !field_meta->visible()) {
    LOG_WARN("field does not exist. table=%s, field=%s", name(), field_name);
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  if (visible_field_num(table_meta_) <= 1) {
    LOG_WARN("cannot drop the last visible field. table=%s, field=%s", name(), field_name);
    return RC::INVALID_ARGUMENT;
  }

  if (table_meta_.vector_index_num() > 0) {
    LOG_WARN("alter table drop column does not support tables with vector indexes yet. table=%s", name());
    return RC::UNIMPLEMENTED;
  }

  TableMeta old_table_meta(table_meta_);
  TableMeta new_table_meta(table_meta_);
  RC        rc = new_table_meta.drop_field(field_name);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to drop field from table meta. table=%s, field=%s, rc=%s", name(), field_name, strrc(rc));
    return rc;
  }

  vector<IndexMeta> remained_index_metas;
  vector<string>    index_names;
  remained_index_metas.reserve(old_table_meta.index_num());
  index_names.reserve(old_table_meta.index_num());
  for (int i = 0; i < old_table_meta.index_num(); i++) {
    const IndexMeta &index_meta = *old_table_meta.index(i);
    index_names.push_back(index_meta.name());
    if (!index_meta.has_field(field_name)) {
      remained_index_metas.push_back(index_meta);
    }
  }
  new_table_meta.clear_indexes();

  vector<vector<char>> materialized_records;
  Trx                 *scan_trx = db_->trx_kit().create_trx(db_->log_handler());
  RecordFileScanner    scanner;
  rc = get_record_scanner(scanner, scan_trx, ReadWriteMode::READ_ONLY);
  if (OB_FAIL(rc)) {
    if (scan_trx != nullptr) {
      db_->trx_kit().destroy_trx(scan_trx);
    }
    LOG_WARN("failed to open scanner while altering table. table=%s, rc=%s", name(), strrc(rc));
    return rc;
  }

  Record record;
  while (OB_SUCC(rc = scanner.next(record))) {
    vector<char> new_record;
    rc = materialize_record_without_column(old_table_meta, new_table_meta, record, field_name, new_record);
    if (OB_FAIL(rc)) {
      scanner.close_scan();
      if (scan_trx != nullptr) {
        db_->trx_kit().destroy_trx(scan_trx);
      }
      LOG_WARN("failed to materialize altered record while dropping column. table=%s, rc=%s", name(), strrc(rc));
      return rc;
    }
    materialized_records.emplace_back(std::move(new_record));
  }
  scanner.close_scan();
  if (scan_trx != nullptr) {
    db_->trx_kit().destroy_trx(scan_trx);
  }

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("failed to scan table records while dropping column. table=%s, rc=%s", name(), strrc(rc));
    return rc;
  }

  rc = sync();
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to sync table before dropping column. table=%s, rc=%s", name(), strrc(rc));
    return rc;
  }

  cleanup_indexes(indexes_);
  if (record_handler_ != nullptr) {
    delete record_handler_;
    record_handler_ = nullptr;
  }
  if (data_buffer_pool_ != nullptr) {
    data_buffer_pool_->close_file();
    data_buffer_pool_ = nullptr;
  }

  const string data_file = table_data_file(base_dir_.c_str(), name());
  if (unlink(data_file.c_str()) == -1) {
    LOG_WARN("failed to remove old data file. file=%s, errno=%d:%s", data_file.c_str(), errno, strerror(errno));
    return RC::FILE_REMOVE;
  }
  for (const string &index_name : index_names) {
    const string index_file = table_index_file(base_dir_.c_str(), name(), index_name.c_str());
    if (unlink(index_file.c_str()) == -1 && errno != ENOENT) {
      LOG_WARN("failed to remove old index file. file=%s, errno=%d:%s", index_file.c_str(), errno, strerror(errno));
      return RC::FILE_REMOVE;
    }
  }

  BufferPoolManager &bpm = db_->buffer_pool_manager();
  rc                     = bpm.create_file(data_file.c_str());
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create new data file while dropping column. file=%s, rc=%s", data_file.c_str(), strrc(rc));
    return rc;
  }

  table_meta_.swap(new_table_meta);
  rc = init_record_handler(base_dir_.c_str());
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to reinit record handler while dropping column. table=%s, rc=%s", name(), strrc(rc));
    return rc;
  }

  for (const vector<char> &new_record_data : materialized_records) {
    Record new_record;
    rc = new_record.copy_data(new_record_data.data(), static_cast<int>(new_record_data.size()));
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to copy rewritten record while dropping column. table=%s, rc=%s", name(), strrc(rc));
      return rc;
    }
    rc = insert_record(new_record);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to insert rewritten record while dropping column. table=%s, rc=%s", name(), strrc(rc));
      return rc;
    }
  }

  rc = persist_table_meta_to_disk(base_dir_, name(), table_meta_);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to persist altered table meta while dropping column. table=%s, rc=%s", name(), strrc(rc));
    return rc;
  }

  for (const IndexMeta &index_meta : remained_index_metas) {
    vector<const FieldMeta *> field_metas;
    field_metas.reserve(index_meta.field_metas().size());
    for (const FieldMeta &indexed_field_meta : index_meta.field_metas()) {
      const FieldMeta *new_field_meta = table_meta_.field(indexed_field_meta.name());
      if (new_field_meta == nullptr) {
        LOG_WARN("failed to find field for rebuilding index while dropping column. table=%s, field=%s",
            name(),
            indexed_field_meta.name());
        return RC::SCHEMA_FIELD_NOT_EXIST;
      }
      field_metas.push_back(new_field_meta);
    }

    rc = create_index(nullptr, field_metas, index_meta.name().c_str(), index_meta.is_unique());
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to rebuild index while dropping column. table=%s, index=%s, rc=%s",
          name(),
          index_meta.name().c_str(),
          strrc(rc));
      return rc;
    }
  }

  LOG_INFO("Successfully dropped column (%s) on table (%s)", field_name, name());
  return RC::SUCCESS;
}

RC Table::change_column(const char *field_name, const AttrInfoSqlNode &attr_info)
{
  const FieldMeta *old_field_meta = table_meta_.field(field_name);
  if (old_field_meta == nullptr || !old_field_meta->visible()) {
    LOG_WARN("field does not exist. table=%s, field=%s", name(), field_name);
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  if (strcmp(field_name, attr_info.name.c_str()) != 0 && table_meta_.field(attr_info.name.c_str()) != nullptr) {
    LOG_WARN("target field already exists. table=%s, field=%s", name(), attr_info.name.c_str());
    return RC::INVALID_ARGUMENT;
  }

  const int expected_len = static_cast<int>(attr_info.arr_len * attr_type_size(attr_info.type));
  if (old_field_meta->type() != attr_info.type || old_field_meta->len() != expected_len) {
    LOG_WARN("change column only supports compatible rename now. table=%s, old=%s, new=%s", name(), field_name,
        attr_info.name.c_str());
    return RC::UNIMPLEMENTED;
  }

  TableMeta new_table_meta(table_meta_);
  RC        rc = new_table_meta.rename_field(field_name, attr_info.name.c_str());
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to rename field in table meta. table=%s, old=%s, new=%s, rc=%s", name(), field_name,
        attr_info.name.c_str(), strrc(rc));
    return rc;
  }

  std::vector<IndexMeta> renamed_index_metas;
  renamed_index_metas.reserve(table_meta_.index_num());
  for (int i = 0; i < table_meta_.index_num(); i++) {
    const IndexMeta &index_meta = *table_meta_.index(i);
    std::vector<FieldMeta> renamed_field_metas;
    renamed_field_metas.reserve(index_meta.field_metas().size());
    for (const FieldMeta &indexed_field_meta : index_meta.field_metas()) {
      FieldMeta new_indexed_field_meta(indexed_field_meta);
      if (strcmp(indexed_field_meta.name(), field_name) == 0) {
        new_indexed_field_meta.set_name(attr_info.name.c_str());
      }
      renamed_field_metas.push_back(new_indexed_field_meta);
    }

    IndexMeta renamed_index_meta;
    rc = renamed_index_meta.init(index_meta.name(), renamed_field_metas, index_meta.is_unique());
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to rebuild renamed index meta. table=%s, index=%s, rc=%s", name(), index_meta.name().c_str(),
          strrc(rc));
      return rc;
    }
    renamed_index_metas.push_back(std::move(renamed_index_meta));
  }

  new_table_meta.clear_indexes();
  for (const IndexMeta &renamed_index_meta : renamed_index_metas) {
    rc = new_table_meta.add_index(renamed_index_meta);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }

  rc = persist_table_meta_to_disk(base_dir_, name(), new_table_meta);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to persist changed table meta. table=%s, old=%s, new=%s, rc=%s", name(), field_name,
        attr_info.name.c_str(), strrc(rc));
    return rc;
  }

  cleanup_indexes(indexes_);
  table_meta_.swap(new_table_meta);
  for (int i = 0; i < table_meta_.index_num(); i++) {
    const IndexMeta *index_meta = table_meta_.index(i);
    auto            *index      = new BplusTreeIndex();
    std::string      index_file = table_index_file(base_dir_.c_str(), name(), index_meta->name().c_str());
    rc                          = index->open(this, index_file.c_str(), *index_meta);
    if (OB_FAIL(rc)) {
      delete index;
      LOG_WARN("failed to reopen index after changing column. table=%s, index=%s, rc=%s", name(),
          index_meta->name().c_str(), strrc(rc));
      return rc;
    }
    indexes_.push_back(index);
  }

  LOG_INFO("Successfully changed column (%s -> %s) on table (%s)", field_name, attr_info.name.c_str(), name());
  return RC::SUCCESS;
}

VectorIndex *Table::find_vector_index(const char *index_name) const
{
  for (auto index : vector_indexes_) {
    if (index->meta().name() == index_name) {
      return index;
    }
  }
  return nullptr;
}

// 根据字段名找到对应的索引，目前仅支持完全匹配的等值查找
VectorIndex *Table::find_vector_index_by_fields(const char *field_names) const
{
  const TableMeta       &table_meta        = this->table_meta();
  const VectorIndexMeta *vector_index_meta = table_meta.find_vector_index_by_fields(field_names);

  if (vector_index_meta == nullptr) {
    return nullptr;
  }
  return this->find_vector_index(vector_index_meta->name().c_str());
}
