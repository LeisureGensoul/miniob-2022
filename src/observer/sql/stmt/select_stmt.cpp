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
// Created by Wangyunlai on 2022/6/6.
//

#include "sql/stmt/select_stmt.h"
#include "sql/stmt/groupby_stmt.h"
#include "sql/expr/expression.h"
#include "sql/parser/parse_defs.h"
#include "sql/stmt/orderby_stmt.h"
#include "sql/stmt/filter_stmt.h"
#include "common/log/log.h"
#include "common/lang/string.h"
#include "storage/common/db.h"
#include "storage/common/table.h"

SelectStmt::~SelectStmt()
{
  if (nullptr != filter_stmt_) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
  for (auto expr : projects_) {
    delete expr;
  }
  projects_.clear();

  if (nullptr != orderby_stmt_) {
    delete orderby_stmt_;
    orderby_stmt_ = nullptr;
  }
  
  if (nullptr != groupby_stmt_) {
    delete groupby_stmt_;
    groupby_stmt_ = nullptr;
  }

}

// static void wildcard_fields(Table *table, std::vector<Field> &field_metas)
static void wildcard_fields(Table *table, std::vector<Expression *> &projects)
{
  const TableMeta &table_meta = table->table_meta();
  // const int field_num = table_meta.field_num();
  const int field_num = table_meta.field_num() - table_meta.extra_filed_num();
  // TODO ignore the last field: __null -> Finished.
  for (int i = table_meta.sys_field_num(); i < field_num; i++) {
  //   field_metas.push_back(Field(table, table_meta.field(i)));
  // }
    // projects.emplace_back(new FieldExpr(table, table_meta.field(i)));
    if (table_meta.field(i)->visible()) {
      projects.emplace_back(new FieldExpr(table, table_meta.field(i)));
    }
  }
}

RC gen_project_expression(Expr *expr, const std::unordered_map<std::string, Table *> &table_map,
    const std::vector<Table *> &tables, Expression *&res_expr)
{
  bool with_brace = expr->with_brace;
  if (expr->type == UNARY) {
    UnaryExpr *uexpr = expr->uexp;
    if (uexpr->is_attr) {
      const char *table_name = uexpr->attr.relation_name;
      const char *field_name = uexpr->attr.attribute_name;
      if (common::is_blank(table_name)) {
        if (tables.size() != 1) {
          LOG_WARN("invalid. I do not know the attr's table. attr=%s", field_name);
          return RC::SCHEMA_FIELD_MISSING;
        }
        Table *table = tables[0];
        const FieldMeta *field_meta = table->table_meta().field(field_name);
        if (nullptr == field_meta) {
          LOG_WARN("no such field. field=%s.%s", table->name(), field_name);
          return RC::SCHEMA_FIELD_MISSING;
        }
        res_expr = new FieldExpr(table, field_meta, with_brace);
        return RC::SUCCESS;
      } else {
        auto iter = table_map.find(table_name);
        if (iter == table_map.end()) {
          LOG_WARN("no such table in from list: %s", table_name);
          return RC::SCHEMA_FIELD_MISSING;
        }

        Table *table = iter->second;
        const FieldMeta *field_meta = table->table_meta().field(field_name);
        if (nullptr == field_meta) {
          LOG_WARN("no such field. field=%s.%s", table->name(), field_name);
          return RC::SCHEMA_FIELD_MISSING;
        }
        res_expr = new FieldExpr(table, field_meta, with_brace);
        return RC::SUCCESS;
      }
    } else {
      res_expr = new ValueExpr(uexpr->value, with_brace);
      return RC::SUCCESS;
    }

  } else if (expr->type == BINARY) {
    Expression *left_expr;
    Expression *right_expr;
    RC rc = gen_project_expression(expr->bexp->left, table_map, tables, left_expr);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    rc = gen_project_expression(expr->bexp->right, table_map, tables, right_expr);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    res_expr = new BinaryExpression(expr->bexp->op, left_expr, right_expr, with_brace, expr->bexp->minus);
    return RC::SUCCESS;
  } else if (expr->type == FUNC) {
    // TO DO FUNC
    Expression *param_expr1;
    Expression *param_expr2;
    switch (expr->fexp->type) {
      case FUNC_LENGTH: {
        RC rc = gen_project_expression(expr->fexp->params[0], table_map, tables, param_expr1);
        if (rc != RC::SUCCESS) {
          return rc;
        }
        // res_expr = new FuncExpression(expr->fexp->type, expr->fexp->param_size, param_expr1, param_expr2, with_brace);
        break;
      }
      case FUNC_ROUND: {
        RC rc = gen_project_expression(expr->fexp->params[0], table_map, tables, param_expr1);
        if (rc != RC::SUCCESS) {
          return rc;
        }
        if (expr->fexp->param_size == 2) {
          rc = gen_project_expression(expr->fexp->params[1], table_map, tables, param_expr2);
          if (rc != RC::SUCCESS) {
            return rc;
          }
        }
        // res_expr = new FuncExpression(expr->fexp->type, expr->fexp->param_size, param_expr1, param_expr2, with_brace);
        break;
      }
      case FUNC_DATE_FORMAT: {
        RC rc = gen_project_expression(expr->fexp->params[0], table_map, tables, param_expr1);
        if (rc != RC::SUCCESS) {
          return rc;
        }
        rc = gen_project_expression(expr->fexp->params[1], table_map, tables, param_expr2);
        if (rc != RC::SUCCESS) {
          return rc;
        }
        // res_expr = new FuncExpression(expr->fexp->type, expr->fexp->param_size, param_expr1, param_expr2, with_brace);
        break;
      }
    }
  } else if (AGGRFUNC == expr->type) {
    // TODO(wbj)
    if (UNARY == expr->afexp->param->type && 0 == expr->afexp->param->uexp->is_attr) {
      // count(*) count(1) count(Value)
      assert(AggrFuncType::COUNT == expr->afexp->type);
      // substitue * or 1 with some field
      Expression *tmp_value_exp = nullptr;
      RC rc = gen_project_expression(expr->afexp->param, table_map, tables, tmp_value_exp);
      if (rc != RC::SUCCESS) {
        return rc;
      }
      assert(ExprType::VALUE == tmp_value_exp->type());
      auto aggr_func_expr = new AggrFuncExpression(
          AggrFuncType::COUNT, new FieldExpr(tables[0], tables[0]->table_meta().field(1)), with_brace);
      aggr_func_expr->set_param_value((ValueExpr *)tmp_value_exp);
      res_expr = aggr_func_expr;
      return RC::SUCCESS;
    }
    Expression *param = nullptr;
    RC rc = gen_project_expression(expr->afexp->param, table_map, tables, param);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    assert(nullptr != param && ExprType::FIELD == param->type());
    res_expr = new AggrFuncExpression(expr->afexp->type, (FieldExpr *)param, with_brace);
    return RC::SUCCESS;
  }
  return RC::SUCCESS;
}

RC SelectStmt::create(Db *db, const Selects &select_sql, Stmt *&stmt)
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }

  // collect tables in `from` statement
  std::vector<Table *> tables;
  std::unordered_map<std::string, Table *> table_map;
  for (size_t i = 0; i < select_sql.relation_num; i++) {
    const char *table_name = select_sql.relations[i];
    if (nullptr == table_name) {
      LOG_WARN("invalid argument. relation name is null. index=%d", i);
      return RC::INVALID_ARGUMENT;
    }

    Table *table = db->find_table(table_name);
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    tables.push_back(table);
    table_map.insert(std::pair<std::string, Table*>(table_name, table));
  }
  
  // collect query fields in `select` statement
  std::vector<Expression *> projects;
  for (int i = select_sql.project_num - 1; i >= 0; i--) {
    const ProjectCol &project_col = select_sql.projects[i];
    // only *
    if (common::is_blank(project_col.relation_name) && project_col.is_star) {
      for (auto it = tables.rbegin(); it != tables.rend(); it++) {
        wildcard_fields(*it, projects);
      }
    } else if (!common::is_blank(project_col.relation_name) && project_col.is_star)  // table_id.*
    {
      const char *table_name = project_col.relation_name;

      if (0 == strcmp(table_name, "*")) {
        for (Table *table : tables) {
          wildcard_fields(table, projects);
        }
      } else {
        auto iter = table_map.find(table_name);
        if (iter == table_map.end()) {
          LOG_WARN("no such table in from list: %s", table_name);
          return RC::SCHEMA_FIELD_MISSING;
        }

        Table *table = iter->second;
        wildcard_fields(table, projects);
      }
    } else  // expression
    {
      Expression *res_project;
      RC rc = gen_project_expression(project_col.expr, table_map, tables, res_project);
      if (rc != RC::SUCCESS) {
        return rc;
      }

      projects.emplace_back(res_project);
    }
  }

  LOG_INFO("got %d tables in from stmt and %d projects in query stmt", tables.size(), projects.size());

  Table *default_table = nullptr;
  if (tables.size() == 1) {
    default_table = tables[0];
  }

  // create filter statement in `where` statement
  FilterStmt *filter_stmt = nullptr;
  RC rc = FilterStmt::create(db, default_table, &table_map,
           select_sql.conditions, select_sql.condition_num, filter_stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("cannot construct filter stmt");
    return rc;
  }

  OrderByStmt *orderby_stmt = nullptr;
  // rc = OrderByStmt::create(db, default_table, &table_map, select_sql.orderbys, select_sql.orderby_num, orderby_stmt);
  // if (rc != RC::SUCCESS) {
  //   LOG_WARN("cannot construct order by stmt");
  //   return rc;
  // }
  if (0 != select_sql.orderby_num) { // 注意对 empty sort operator 的处理
    rc = OrderByStmt::create(db, default_table, &table_map, select_sql.orderbys, select_sql.orderby_num, orderby_stmt);
    if (rc != RC::SUCCESS) {
      LOG_WARN("cannot construct order by stmt");
      return rc;
    }
  }

  OrderByStmt *orderby_stmt_for_groupby = nullptr;
  GroupByStmt *groupby_stmt = nullptr;
  if (0 != select_sql.groupby_num) {
    rc = OrderByStmt::create(
        db, default_table, &table_map, select_sql.groupbys, select_sql.groupby_num, orderby_stmt_for_groupby);
    if (rc != RC::SUCCESS) {
      LOG_WARN("cannot construct order by stmt for groupby");
      return rc;
    }
    rc = GroupByStmt::create(db, default_table, &table_map, select_sql.groupbys, select_sql.groupby_num, groupby_stmt);
    if (rc != RC::SUCCESS) {
      LOG_WARN("cannot construct group by stmt");
      return rc;
    }
  }

  // everything alright
  SelectStmt *select_stmt = new SelectStmt();
  select_stmt->tables_.swap(tables);
  select_stmt->projects_.swap(projects);
  select_stmt->filter_stmt_ = filter_stmt;
  select_stmt->orderby_stmt_ = orderby_stmt;
  select_stmt->orderby_stmt_for_groupby_ = orderby_stmt_for_groupby;
  select_stmt->groupby_stmt_ = groupby_stmt;
  stmt = select_stmt;
  return RC::SUCCESS;
}
