#include "drop_table_executor.h"

#include <event/session_event.h>
#include <event/sql_event.h>
#include <session/session.h>
#include <sql/stmt/drop_table_stmt.h>
#include <storage/db/db.h>

RC DropTableExecutor::execute(SQLStageEvent *sql_event) {
    //通过 SQL 事件（sql_event）获取会话事件（session_event()），进而获取会话（session()）。
    Db* db =  sql_event->session_event()->session()->get_current_db();
    //从 SQL 事件中提取 SQL 语句对象
    Stmt* t = sql_event->stmt();
    // DropTableStmt* stmt = (DropTableStmt *)t;
    //使用 dynamic_cast 将通用的 Stmt 指针转换为具体的 DropTableStmt 指针，确保 SQL 语句的类型是 DropTableStmt（表示删除表的操作）
    DropTableStmt* stmt = dynamic_cast<DropTableStmt*>(t);
    //使用断言（ASSERT）检查类型转换是否成功。
    ASSERT(stmt != nullptr, "DropTableStmt is nullptr");
    //
    db->drop_table(stmt->table_name().c_str());
    return RC::SUCCESS;
}
