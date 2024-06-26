#pragma once

#include <stdint.h>

typedef struct SToken {
    const char* z;
    unsigned int n;
} SToken;

typedef enum ENodeType {
    QUERY_NODE_COLUMN = 1,
    QUERY_NODE_VALUE,
    QUERY_NODE_OPERATOR,
    QUERY_NODE_LOGIC_CONDITION,
    QUERY_NODE_FUNCTION,
    QUERY_NODE_REAL_TABLE,
    QUERY_NODE_TEMP_TABLE,
    QUERY_NODE_JOIN_TABLE,
    QUERY_NODE_GROUPING_SET,
    QUERY_NODE_ORDER_BY_EXPR,
    QUERY_NODE_LIMIT,
    QUERY_NODE_STATE_WINDOW,
    QUERY_NODE_SESSION_WINDOW,
    QUERY_NODE_INTERVAL_WINDOW,
    QUERY_NODE_NODE_LIST,
    QUERY_NODE_FILL,
    QUERY_NODE_RAW_EXPR,
    QUERY_NODE_TARGET,
    QUERY_NODE_DATABLOCK_DESC,
    QUERY_NODE_SLOT_DESC,
    QUERY_NODE_COLUMN_DEF,
    QUERY_NODE_DOWNSTREAM_SOURCE,
    QUERY_NODE_DATABASE_OPTIONS,
    QUERY_NODE_TABLE_OPTIONS,
    QUERY_NODE_INDEX_OPTIONS,
    QUERY_NODE_EXPLAIN_OPTIONS,
    QUERY_NODE_STREAM_OPTIONS,
    QUERY_NODE_LEFT_VALUE,
    QUERY_NODE_COLUMN_REF,
    QUERY_NODE_WHEN_THEN,
    QUERY_NODE_CASE_WHEN,
    QUERY_NODE_EVENT_WINDOW,
} ENodeType;

typedef struct SNode {
    ENodeType type;
} SNode;

typedef struct SListCell {
    struct SListCell* pPrev;
    struct SListCell* pNext;
    SNode* pNode;
} SListCell;

typedef struct SNodeList {
    int32_t length;
    SListCell* pHead;
    SListCell* pTail;
} SNodeList;

typedef struct SAstCreateContex {
} SAstCreateContex;

void nodesDestroyNode(SNode* pNode);
