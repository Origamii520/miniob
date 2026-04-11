#pragma once

#include "common/rc.h"

class SQLStageEvent;

class AlterTableExecutor
{
public:
  AlterTableExecutor()          = default;
  virtual ~AlterTableExecutor() = default;

  RC execute(SQLStageEvent *sql_event);
};
