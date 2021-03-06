/*
 * Copyright 2020 Bitnine Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * I/O routines for agtype type
 *
 * Portions Copyright (c) 2014-2018, PostgreSQL Global Development Group
 */

#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/typcache.h"

#include "utils/agtype.h"
#include "utils/agtype_parser.h"
#include "utils/ag_float8_supp.h"
#include "catalog/ag_graph.h"
#include "catalog/ag_label.h"
#include "utils/graphid.h"

typedef struct agtype_in_state
{
    agtype_parse_state *parse_state;
    agtype_value *res;
} agtype_in_state;

typedef enum /* type categories for datum_to_agtype */
{
    AGT_TYPE_NULL, /* null, so we didn't bother to identify */
    AGT_TYPE_BOOL, /* boolean (built-in types only) */
    AGT_TYPE_INTEGER, /* Cypher Integer type */
    AGT_TYPE_FLOAT, /* Cypher Float type */
    AGT_TYPE_NUMERIC, /* numeric (ditto) */
    AGT_TYPE_DATE, /* we use special formatting for datetimes */
    AGT_TYPE_TIMESTAMP, /* we use special formatting for timestamp */
    AGT_TYPE_TIMESTAMPTZ, /* ... and timestamptz */
    AGT_TYPE_AGTYPE, /* AGTYPE */
    AGT_TYPE_JSON, /* JSON */
    AGT_TYPE_JSONB, /* JSONB */
    AGT_TYPE_ARRAY, /* array */
    AGT_TYPE_COMPOSITE, /* composite */
    AGT_TYPE_JSONCAST, /* something with an explicit cast to JSON */
    AGT_TYPE_VERTEX,
    AGT_TYPE_OTHER /* all else */
} agt_type_category;

static inline Datum agtype_from_cstring(char *str, int len);
size_t check_string_length(size_t len);
static void agtype_in_agtype_annotation(void *pstate, char *annotation);
static void agtype_in_object_start(void *pstate);
static void agtype_in_object_end(void *pstate);
static void agtype_in_array_start(void *pstate);
static void agtype_in_array_end(void *pstate);
static void agtype_in_object_field_start(void *pstate, char *fname,
                                         bool isnull);
static void agtype_put_escaped_value(StringInfo out, agtype_value *scalar_val);
static void escape_agtype(StringInfo buf, const char *str);
bool is_decimal_needed(char *numstr);
static void agtype_in_scalar(void *pstate, char *token,
                             agtype_token_type tokentype,
                             char *annotation);
static void agtype_categorize_type(Oid typoid, agt_type_category *tcategory,
                                   Oid *outfuncoid);
static void composite_to_agtype(Datum composite, agtype_in_state *result);
static void array_dim_to_agtype(agtype_in_state *result, int dim, int ndims,
                                int *dims, Datum *vals, bool *nulls,
                                int *valcount, agt_type_category tcategory,
                                Oid outfuncoid);
static void array_to_agtype_internal(Datum array, agtype_in_state *result);
static void datum_to_agtype(Datum val, bool is_null, agtype_in_state *result,
                            agt_type_category tcategory, Oid outfuncoid,
                            bool key_scalar);
static void add_agtype(Datum val, bool is_null, agtype_in_state *result,
                       Oid val_type, bool key_scalar);
static char *agtype_to_cstring_worker(StringInfo out, agtype_container *in,
                                      int estimated_len, bool indent);
static void add_indent(StringInfo out, bool indent, int level);
static void cannot_cast_agtype_value(enum agtype_value_type type,
                                     const char *sqltype);
static bool agtype_extract_scalar(agtype_container *agtc, agtype_value *res);
static agtype *execute_array_access_operator(agtype *array, agtype *element);
static agtype *execute_map_access_operator(agtype *map, agtype *key);
/* typecast functions */
static void agtype_typecast_object(agtype_in_state *state, char *annotation);
static void agtype_typecast_array(agtype_in_state *state, char *annotation);
/* validation functions */
static bool is_object_vertex(agtype_value *agtv);
static bool is_object_edge(agtype_value *agtv);
static bool is_array_path(agtype_value *agtv);
/* helper functions */
static bool is_agtype_null(agtype *agt);
static agtype_value *string_to_agtype_value(char *s);
static uint64 get_edge_uniqueness_value(Datum d, Oid type, bool is_null,
                                        int index);
static agtype_value *get_agtype_value_object_value(agtype_value *agtv_object,
                                             char *key);
/* graph entity retrieval */
static Datum get_vertex(const char *graph, const char *vertex_label,
                         int64 graphid);
static Datum column_get_datum(TupleDesc tupdesc, HeapTuple tuple, int column,
                        const char *attname, Oid typid, bool isnull);
static char *get_label_name(const char *graph_name, int64 graph_id);

PG_FUNCTION_INFO_V1(agtype_in);

/*
 * agtype type input function
 */
Datum agtype_in(PG_FUNCTION_ARGS)
{
    char *str = PG_GETARG_CSTRING(0);

    return agtype_from_cstring(str, strlen(str));
}

PG_FUNCTION_INFO_V1(agtype_out);

/*
 * agtype type output function
 */
Datum agtype_out(PG_FUNCTION_ARGS)
{
    agtype *agt = AG_GET_ARG_AGTYPE_P(0);
    char *out;

    out = agtype_to_cstring(NULL, &agt->root, VARSIZE(agt));

    PG_RETURN_CSTRING(out);
}

/*
 * agtype_from_cstring
 *
 * Turns agtype string into an agtype Datum.
 *
 * Uses the agtype parser (with hooks) to construct an agtype.
 */
static inline Datum agtype_from_cstring(char *str, int len)
{
    agtype_lex_context *lex;
    agtype_in_state state;
    agtype_sem_action sem;

    memset(&state, 0, sizeof(state));
    memset(&sem, 0, sizeof(sem));
    lex = make_agtype_lex_context_cstring_len(str, len, true);

    sem.semstate = (void *)&state;

    sem.object_start = agtype_in_object_start;
    sem.array_start = agtype_in_array_start;
    sem.object_end = agtype_in_object_end;
    sem.array_end = agtype_in_array_end;
    sem.scalar = agtype_in_scalar;
    sem.object_field_start = agtype_in_object_field_start;
    /* callback for annotation (typecasts) */
    sem.agtype_annotation = agtype_in_agtype_annotation;


    parse_agtype(lex, &sem);

    /* after parsing, the item member has the composed agtype structure */
    PG_RETURN_POINTER(agtype_value_to_agtype(state.res));
}

size_t check_string_length(size_t len)
{
    if (len > AGTENTRY_OFFLENMASK)
    {
        ereport(
            ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("string too long to represent as agtype string"),
             errdetail(
                 "Due to an implementation restriction, agtype strings cannot exceed %d bytes.",
                 AGTENTRY_OFFLENMASK)));
    }

    return len;
}

static void agtype_in_object_start(void *pstate)
{
    agtype_in_state *_state = (agtype_in_state *)pstate;

    _state->res = push_agtype_value(&_state->parse_state, WAGT_BEGIN_OBJECT,
                                    NULL);
}

static void agtype_in_object_end(void *pstate)
{
    agtype_in_state *_state = (agtype_in_state *)pstate;

    _state->res = push_agtype_value(&_state->parse_state, WAGT_END_OBJECT,
                                    NULL);
}

static void agtype_in_array_start(void *pstate)
{
    agtype_in_state *_state = (agtype_in_state *)pstate;

    _state->res = push_agtype_value(&_state->parse_state, WAGT_BEGIN_ARRAY,
                                    NULL);
}

static void agtype_in_array_end(void *pstate)
{
    agtype_in_state *_state = (agtype_in_state *)pstate;

    _state->res = push_agtype_value(&_state->parse_state, WAGT_END_ARRAY,
                                    NULL);
}

static void agtype_in_object_field_start(void *pstate, char *fname,
                                         bool isnull)
{
    agtype_in_state *_state = (agtype_in_state *)pstate;
    agtype_value v;

    Assert(fname != NULL);
    v.type = AGTV_STRING;
    v.val.string.len = check_string_length(strlen(fname));
    v.val.string.val = fname;

    _state->res = push_agtype_value(&_state->parse_state, WAGT_KEY, &v);
}

/* main in function to process annotations */
static void agtype_in_agtype_annotation(void *pstate, char *annotation)
{
    agtype_in_state *_state = (agtype_in_state *)pstate;

    /* verify that our required params are not null */
    Assert(pstate != NULL);
    Assert(annotation != NULL);

    /* pass to the appropriate typecast routine */
    switch (_state->res->type)
    {
    case AGTV_OBJECT:
        agtype_typecast_object(_state, annotation);
        break;
    case AGTV_ARRAY:
        agtype_typecast_array(_state, annotation);
        break;

    /*
     * Maybe we need to eventually move scalar annotations here. However,
     * we need to think about how an actual scalar value may be incorporated
     * into another object. Remember, the scalar is copied in on close, before
     * we would apply the annotation.
     */
    default:
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("unsupported type to annotate")));
        break;
    }
}

/* function to handle object typecasts */
static void agtype_typecast_object(agtype_in_state *state, char *annotation)
{
    agtype_value *agtv;
    agtype_value *last_updated_value;
    int len;
    bool top = true;

    /* verify that our required params are not null */
    Assert(annotation != NULL);
    Assert(state != NULL);

    len = strlen(annotation);
    agtv = state->res;

    /*
     * If the parse_state is not NULL, then we are not at the top level
     * and the following must be valid for a nested object with a typecast
     * at the end.
     */
    if (state->parse_state != NULL)
    {
        top = false;
        last_updated_value = state->parse_state->last_updated_value;
        /* make sure there is a value just copied in */
        Assert(last_updated_value != NULL);
        /* and that it is of type object */
        Assert(last_updated_value->type == AGTV_OBJECT);
    }

    /* check for a cast to a vertex */
    if (len == 6 && pg_strncasecmp(annotation, "vertex", len) == 0)
    {
        /* verify that the structure conforms to a valid vertex */
        if (is_object_vertex(agtv))
        {
            agtv->type = AGTV_VERTEX;
            /* if it isn't the top, we need to adjust the copied value */
            if (!top)
                last_updated_value->type = AGTV_VERTEX;
        }
        else
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("object is not a vertex")));

    }
    /* check for a cast to an edge */
    else if (len == 4 && pg_strncasecmp(annotation, "edge", len) == 0)
    {
        /* verify that the structure conforms to a valid edge */
        if (is_object_edge(agtv))
        {
            agtv->type = AGTV_EDGE;
            /* if it isn't the top, we need to adjust the copied value */
            if (!top)
                last_updated_value->type = AGTV_EDGE;
        }
        else
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("object is not a edge")));
    }
    /* otherwise this isn't a supported typecast */
    else
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid annotation value for object")));
}

/* function to handle array typecasts */
static void agtype_typecast_array(agtype_in_state *state, char *annotation)
{
    agtype_value *agtv;
    agtype_value *last_updated_value;
    int len;
    bool top = true;

    /* verify that our required params are not null */
    Assert(annotation != NULL);
    Assert(state != NULL);

    len = strlen(annotation);
    agtv = state->res;

    /*
     * If the parse_state is not NULL, then we are not at the top level
     * and the following must be valid for a nested array with a typecast
     * at the end.
     */
    if (state->parse_state != NULL)
    {
        top = false;
        last_updated_value = state->parse_state->last_updated_value;
        /* make sure there is a value just copied in */
        Assert(last_updated_value != NULL);
        /* and that it is of type object */
        Assert(last_updated_value->type == AGTV_ARRAY);
    }

    /* check for a cast to a path */
    if (len == 4 && pg_strncasecmp(annotation, "path", len) == 0)
    {
        /* verify that the array conforms to a valid path */
        if (is_array_path(agtv))
        {
            agtv->type = AGTV_PATH;
            /* if it isn't the top, we need to adjust the copied value */
            if (!top)
                last_updated_value->type = AGTV_PATH;
        }
        else
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("array is not a valid path")));

    }
    /* otherwise this isn't a supported typecast */
    else
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid annotation value for object")));

}

/* helper function to check if an object fits a vertex */
static bool is_object_vertex(agtype_value *agtv)
{
    bool has_id = false;
    bool has_label = false;
    bool has_properties = false;
    int i;

    /* we require a valid object */
    Assert(agtv != NULL);
    Assert(agtv->type == AGTV_OBJECT);

    /* we need 3 pairs for a vertex */
    if (agtv->val.object.num_pairs != 3)
        return false;

    /* iterate through all pairs */
    for (i = 0; i < agtv->val.object.num_pairs; i++)
    {
        agtype_value *key = &agtv->val.object.pairs[i].key;
        agtype_value *value = &agtv->val.object.pairs[i].value;

        char *key_val = key->val.string.val;
        int key_len = key->val.string.len;

        Assert(key->type == AGTV_STRING);

        /* check for an id of type integer */
        if (key_len == 2 &&
            pg_strncasecmp(key_val, "id", key_len) == 0 &&
            value->type == AGTV_INTEGER)
            has_id = true;
        /* check for a label of type string */
        else if (key_len == 5 &&
            pg_strncasecmp(key_val, "label", key_len) == 0 &&
            value->type == AGTV_STRING)
            has_label = true;
        /* check for properties of type object */
        else if (key_len == 10 &&
            pg_strncasecmp(key_val, "properties", key_len) == 0 &&
            value->type == AGTV_OBJECT)
            has_properties = true;
        /* if it gets to this point, it can't be a vertex */
        else
            return false;
    }
    return (has_id && has_label && has_properties);
}

/* helper function to check if an object fits an edge */
static bool is_object_edge(agtype_value *agtv)
{
    bool has_id = false;
    bool has_label = false;
    bool has_properties = false;
    bool has_start_id = false;
    bool has_end_id = false;
    int i;

    /* we require a valid object */
    Assert(agtv != NULL);
    Assert(agtv->type == AGTV_OBJECT);

    /* we need 5 pairs for an edge */
    if (agtv->val.object.num_pairs != 5)
        return false;

    /* iterate through the pairs */
    for (i = 0; i < agtv->val.object.num_pairs; i++)
    {
        agtype_value *key = &agtv->val.object.pairs[i].key;
        agtype_value *value = &agtv->val.object.pairs[i].value;

        char *key_val = key->val.string.val;
        int key_len = key->val.string.len;

        Assert(key->type == AGTV_STRING);

        /* check for an id of type integer */
        if (key_len == 2 &&
            pg_strncasecmp(key_val, "id", key_len) == 0 &&
            value->type == AGTV_INTEGER)
            has_id = true;
        /* check for a label of type string */
        else if (key_len == 5 &&
            pg_strncasecmp(key_val, "label", key_len) == 0 &&
            value->type == AGTV_STRING)
            has_label = true;
        /* check for properties of type object */
        else if (key_len == 10 &&
            pg_strncasecmp(key_val, "properties", key_len) == 0 &&
            value->type == AGTV_OBJECT)
            has_properties = true;
        /* check for a start_id of type integer */
        else if (key_len == 8 &&
            pg_strncasecmp(key_val, "start_id", key_len) == 0 &&
            value->type == AGTV_INTEGER)
            has_start_id = true;
        /* check for an end_id of type integer */
        else if (key_len == 6 &&
            pg_strncasecmp(key_val, "end_id", key_len) == 0 &&
            value->type == AGTV_INTEGER)
            has_end_id = true;
        /* if it gets to this point, it can't be an edge */
        else
            return false;
    }
    return (has_id && has_label && has_properties &&
            has_start_id && has_end_id);
}

/* helper function to check if an array fits a path */
static bool is_array_path(agtype_value *agtv)
{
    agtype_value *element = NULL;
    int i;

    /* we require a valid array */
    Assert(agtv != NULL);
    Assert(agtv->type == AGTV_ARRAY);

    /* the array needs to have an odd number of elements greater than 2 */
    if (agtv->val.array.num_elems < 3 ||
        (agtv->val.array.num_elems - 1) % 2 != 0)
        return false;

    /* iterate through all elements */
    for (i = 0; (i + 1) < agtv->val.array.num_elems; i+=2)
    {
        element = &agtv->val.array.elems[i];
        if (element->type != AGTV_VERTEX)
            return false;

        element = &agtv->val.array.elems[i+1];
        if (element->type != AGTV_EDGE)
            return false;
    }

    /* check the last element */
    element = &agtv->val.array.elems[i];
    if (element->type != AGTV_VERTEX)
        return false;

    return true;
}

static void agtype_put_escaped_value(StringInfo out, agtype_value *scalar_val)
{
    char *numstr;

    switch (scalar_val->type)
    {
    case AGTV_NULL:
        appendBinaryStringInfo(out, "null", 4);
        break;
    case AGTV_STRING:
        escape_agtype(out, pnstrdup(scalar_val->val.string.val,
                                    scalar_val->val.string.len));
        break;
    case AGTV_NUMERIC:
        appendStringInfoString(
            out, DatumGetCString(DirectFunctionCall1(
                     numeric_out, PointerGetDatum(scalar_val->val.numeric))));
        appendBinaryStringInfo(out, "::numeric", 9);
        break;
    case AGTV_INTEGER:
        appendStringInfoString(
            out, DatumGetCString(DirectFunctionCall1(
                     int8out, Int64GetDatum(scalar_val->val.int_value))));
        break;
    case AGTV_FLOAT:
        numstr = DatumGetCString(DirectFunctionCall1(
            float8out, Float8GetDatum(scalar_val->val.float_value)));
        appendStringInfoString(out, numstr);

        if (is_decimal_needed(numstr))
            appendBinaryStringInfo(out, ".0", 2);
        break;
    case AGTV_BOOL:
        if (scalar_val->val.boolean)
            appendBinaryStringInfo(out, "true", 4);
        else
            appendBinaryStringInfo(out, "false", 5);
        break;
    case AGTV_VERTEX:
    {
        agtype *prop;
        scalar_val->type = AGTV_OBJECT;
        prop = agtype_value_to_agtype(scalar_val);
        agtype_to_cstring_worker(out, &prop->root, prop->vl_len_, false);
        appendBinaryStringInfo(out, "::vertex", 8);
        break;
    }
    case AGTV_EDGE:
    {
        agtype *prop;
        scalar_val->type = AGTV_OBJECT;
        prop = agtype_value_to_agtype(scalar_val);
        agtype_to_cstring_worker(out, &prop->root, prop->vl_len_, false);
        appendBinaryStringInfo(out, "::edge", 6);
        break;
    }
    case AGTV_PATH:
    {
        agtype *prop;
        scalar_val->type = AGTV_ARRAY;
        prop = agtype_value_to_agtype(scalar_val);
        agtype_to_cstring_worker(out, &prop->root, prop->vl_len_, false);
        appendBinaryStringInfo(out, "::path", 6);
        break;
    }

    default:
        elog(ERROR, "unknown agtype scalar type");
    }
}

/*
 * Produce an agtype string literal, properly escaping characters in the text.
 */
static void escape_agtype(StringInfo buf, const char *str)
{
    const char *p;

    appendStringInfoCharMacro(buf, '"');
    for (p = str; *p; p++)
    {
        switch (*p)
        {
        case '\b':
            appendStringInfoString(buf, "\\b");
            break;
        case '\f':
            appendStringInfoString(buf, "\\f");
            break;
        case '\n':
            appendStringInfoString(buf, "\\n");
            break;
        case '\r':
            appendStringInfoString(buf, "\\r");
            break;
        case '\t':
            appendStringInfoString(buf, "\\t");
            break;
        case '"':
            appendStringInfoString(buf, "\\\"");
            break;
        case '\\':
            appendStringInfoString(buf, "\\\\");
            break;
        default:
            if ((unsigned char)*p < ' ')
                appendStringInfo(buf, "\\u%04x", (int)*p);
            else
                appendStringInfoCharMacro(buf, *p);
            break;
        }
    }
    appendStringInfoCharMacro(buf, '"');
}

bool is_decimal_needed(char *numstr)
{
    int i;

    Assert(numstr);

    i = (numstr[0] == '-') ? 1 : 0;

    while (numstr[i] != '\0')
    {
        if (numstr[i] < '0' || numstr[i] > '9')
            return false;

        i++;
    }

    return true;
}

/*
 * For agtype we always want the de-escaped value - that's what's in token
 */
static void agtype_in_scalar(void *pstate, char *token,
                             agtype_token_type tokentype,
                             char *annotation)
{
    agtype_in_state *_state = (agtype_in_state *)pstate;
    agtype_value v;
    Datum numd;

    /*
     * Process the scalar typecast annotations, if present, but not if the
     * argument is a null. Typecasting a null is a null.
     */
    if (annotation != NULL && tokentype != AGTYPE_TOKEN_NULL)
    {
        int len = strlen(annotation);

        if (len == 7 && pg_strcasecmp(annotation, "numeric") == 0)
            tokentype = AGTYPE_TOKEN_NUMERIC;
        else if (len == 7 && pg_strcasecmp(annotation, "integer") == 0)
            tokentype = AGTYPE_TOKEN_INTEGER;
        else if (len == 5 && pg_strcasecmp(annotation, "float") == 0)
            tokentype = AGTYPE_TOKEN_FLOAT;
        else
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("invalid annotation value for scalar")));
    }

    switch (tokentype)
    {
    case AGTYPE_TOKEN_STRING:
        Assert(token != NULL);
        v.type = AGTV_STRING;
        v.val.string.len = check_string_length(strlen(token));
        v.val.string.val = token;
        break;
    case AGTYPE_TOKEN_INTEGER:
        Assert(token != NULL);
        v.type = AGTV_INTEGER;
        scanint8(token, false, &v.val.int_value);
        break;
    case AGTYPE_TOKEN_FLOAT:
        Assert(token != NULL);
        v.type = AGTV_FLOAT;
        v.val.float_value = float8in_internal(token, NULL, "double precision",
                                              token);
        break;
    case AGTYPE_TOKEN_NUMERIC:
        Assert(token != NULL);
        v.type = AGTV_NUMERIC;
        numd = DirectFunctionCall3(numeric_in,
                                   CStringGetDatum(token),
                                   ObjectIdGetDatum(InvalidOid),
                                   Int32GetDatum(-1));
        v.val.numeric = DatumGetNumeric(numd);
        break;

    case AGTYPE_TOKEN_TRUE:
        v.type = AGTV_BOOL;
        v.val.boolean = true;
        break;
    case AGTYPE_TOKEN_FALSE:
        v.type = AGTV_BOOL;
        v.val.boolean = false;
        break;
    case AGTYPE_TOKEN_NULL:
        v.type = AGTV_NULL;
        break;
    default:
        /* should not be possible */
        elog(ERROR, "invalid agtype token type");
        break;
    }

    if (_state->parse_state == NULL)
    {
        /* single scalar */
        agtype_value va;

        va.type = AGTV_ARRAY;
        va.val.array.raw_scalar = true;
        va.val.array.num_elems = 1;

        _state->res = push_agtype_value(&_state->parse_state, WAGT_BEGIN_ARRAY,
                                        &va);
        _state->res = push_agtype_value(&_state->parse_state, WAGT_ELEM, &v);
        _state->res = push_agtype_value(&_state->parse_state, WAGT_END_ARRAY,
                                        NULL);
    }
    else
    {
        agtype_value *o = &_state->parse_state->cont_val;

        switch (o->type)
        {
        case AGTV_ARRAY:
            _state->res = push_agtype_value(&_state->parse_state, WAGT_ELEM,
                                            &v);
            break;
        case AGTV_OBJECT:
            _state->res = push_agtype_value(&_state->parse_state, WAGT_VALUE,
                                            &v);
            break;
        default:
            elog(ERROR, "unexpected parent of nested structure");
        }
    }
}

/*
 * agtype_to_cstring
 *     Converts agtype value to a C-string.
 *
 * If 'out' argument is non-null, the resulting C-string is stored inside the
 * StringBuffer.  The resulting string is always returned.
 *
 * A typical case for passing the StringInfo in rather than NULL is where the
 * caller wants access to the len attribute without having to call strlen, e.g.
 * if they are converting it to a text* object.
 */
char *agtype_to_cstring(StringInfo out, agtype_container *in,
                        int estimated_len)
{
    return agtype_to_cstring_worker(out, in, estimated_len, false);
}

/*
 * same thing but with indentation turned on
 */
char *agtype_to_cstring_indent(StringInfo out, agtype_container *in,
                               int estimated_len)
{
    return agtype_to_cstring_worker(out, in, estimated_len, true);
}

/*
 * common worker for above two functions
 */
static char *agtype_to_cstring_worker(StringInfo out, agtype_container *in,
                                      int estimated_len, bool indent)
{
    bool first = true;
    agtype_iterator *it;
    agtype_value v;
    agtype_iterator_token type = WAGT_DONE;
    int level = 0;
    bool redo_switch = false;

    /* If we are indenting, don't add a space after a comma */
    int ispaces = indent ? 1 : 2;

    /*
     * Don't indent the very first item. This gets set to the indent flag at
     * the bottom of the loop.
     */
    bool use_indent = false;
    bool raw_scalar = false;
    bool last_was_key = false;

    if (out == NULL)
        out = makeStringInfo();

    enlargeStringInfo(out, (estimated_len >= 0) ? estimated_len : 64);

    it = agtype_iterator_init(in);

    while (redo_switch ||
           ((type = agtype_iterator_next(&it, &v, false)) != WAGT_DONE))
    {
        redo_switch = false;
        switch (type)
        {
        case WAGT_BEGIN_ARRAY:
            if (!first)
                appendBinaryStringInfo(out, ", ", ispaces);

            if (!v.val.array.raw_scalar)
            {
                add_indent(out, use_indent && !last_was_key, level);
                appendStringInfoCharMacro(out, '[');
            }
            else
            {
                raw_scalar = true;
            }

            first = true;
            level++;
            break;
        case WAGT_BEGIN_OBJECT:
            if (!first)
                appendBinaryStringInfo(out, ", ", ispaces);

            add_indent(out, use_indent && !last_was_key, level);
            appendStringInfoCharMacro(out, '{');

            first = true;
            level++;
            break;
        case WAGT_KEY:
            if (!first)
                appendBinaryStringInfo(out, ", ", ispaces);
            first = true;

            add_indent(out, use_indent, level);

            /* agtype rules guarantee this is a string */
            agtype_put_escaped_value(out, &v);
            appendBinaryStringInfo(out, ": ", 2);

            type = agtype_iterator_next(&it, &v, false);
            if (type == WAGT_VALUE)
            {
                first = false;
                agtype_put_escaped_value(out, &v);
            }
            else
            {
                Assert(type == WAGT_BEGIN_OBJECT || type == WAGT_BEGIN_ARRAY);

                /*
                 * We need to rerun the current switch() since we need to
                 * output the object which we just got from the iterator
                 * before calling the iterator again.
                 */
                redo_switch = true;
            }
            break;
        case WAGT_ELEM:
            if (!first)
                appendBinaryStringInfo(out, ", ", ispaces);
            first = false;

            if (!raw_scalar)
                add_indent(out, use_indent, level);
            agtype_put_escaped_value(out, &v);
            break;
        case WAGT_END_ARRAY:
            level--;
            if (!raw_scalar)
            {
                add_indent(out, use_indent, level);
                appendStringInfoCharMacro(out, ']');
            }
            first = false;
            break;
        case WAGT_END_OBJECT:
            level--;
            add_indent(out, use_indent, level);
            appendStringInfoCharMacro(out, '}');
            first = false;
            break;
        default:
            elog(ERROR, "unknown agtype iterator token type");
        }
        use_indent = indent;
        last_was_key = redo_switch;
    }

    Assert(level == 0);

    return out->data;
}

static void add_indent(StringInfo out, bool indent, int level)
{
    if (indent)
    {
        int i;

        appendStringInfoCharMacro(out, '\n');
        for (i = 0; i < level; i++)
            appendBinaryStringInfo(out, "    ", 4);
    }
}

Datum integer_to_agtype(int64 i)
{
    agtype_value agtv;
    agtype *agt;

    agtv.type = AGTV_INTEGER;
    agtv.val.int_value = i;
    agt = agtype_value_to_agtype(&agtv);

    return AGTYPE_P_GET_DATUM(agt);
}

Datum float_to_agtype(float8 f)
{
    agtype_value agtv;
    agtype *agt;

    agtv.type = AGTV_FLOAT;
    agtv.val.float_value = f;
    agt = agtype_value_to_agtype(&agtv);

    return AGTYPE_P_GET_DATUM(agt);
}

/*
 * s must be a UTF-8 encoded, unescaped, and null-terminated string which is
 * a valid string for internal storage of agtype.
 */
Datum string_to_agtype(char *s)
{
    agtype_value agtv;
    agtype *agt;

    agtv.type = AGTV_STRING;
    agtv.val.string.len = check_string_length(strlen(s));
    agtv.val.string.val = s;
    agt = agtype_value_to_agtype(&agtv);

    return AGTYPE_P_GET_DATUM(agt);
}

Datum boolean_to_agtype(bool b)
{
    agtype_value agtv;
    agtype *agt;

    agtv.type = AGTV_BOOL;
    agtv.val.boolean = b;
    agt = agtype_value_to_agtype(&agtv);

    return AGTYPE_P_GET_DATUM(agt);
}

/*
 * Determine how we want to render values of a given type in datum_to_agtype.
 *
 * Given the datatype OID, return its agt_type_category, as well as the type's
 * output function OID.  If the returned category is AGT_TYPE_JSONCAST,
 * we return the OID of the relevant cast function instead.
 */
static void agtype_categorize_type(Oid typoid, agt_type_category *tcategory,
                                   Oid *outfuncoid)
{
    bool typisvarlena;

    /* Look through any domain */
    typoid = getBaseType(typoid);

    *outfuncoid = InvalidOid;

    /*
     * We need to get the output function for everything except date and
     * timestamp types, booleans, array and composite types, json and jsonb,
     * and non-builtin types where there's a cast to json. In this last case
     * we return the oid of the cast function instead.
     */

    switch (typoid)
    {
    case BOOLOID:
        *tcategory = AGT_TYPE_BOOL;
        break;

    case INT2OID:
    case INT4OID:
    case INT8OID:
        getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
        *tcategory = AGT_TYPE_INTEGER;
        break;

    case FLOAT8OID:
        getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
        *tcategory = AGT_TYPE_FLOAT;
        break;

    case FLOAT4OID:
    case NUMERICOID:
        getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
        *tcategory = AGT_TYPE_NUMERIC;
        break;

    case DATEOID:
        *tcategory = AGT_TYPE_DATE;
        break;

    case TIMESTAMPOID:
        *tcategory = AGT_TYPE_TIMESTAMP;
        break;

    case TIMESTAMPTZOID:
        *tcategory = AGT_TYPE_TIMESTAMPTZ;
        break;

    case JSONBOID:
        *tcategory = AGT_TYPE_JSONB;
        break;

    case JSONOID:
        *tcategory = AGT_TYPE_JSON;
        break;

    default:
        /* Check for arrays and composites */
        if (typoid == AGTYPEOID)
        {
            *tcategory = AGT_TYPE_AGTYPE;
        }
        else if (OidIsValid(get_element_type(typoid)) ||
                 typoid == ANYARRAYOID || typoid == RECORDARRAYOID)
        {
            *tcategory = AGT_TYPE_ARRAY;
        }
        else if (type_is_rowtype(typoid)) /* includes RECORDOID */
        {
            *tcategory = AGT_TYPE_COMPOSITE;
        }
        else if (typoid == GRAPHIDOID)
        {
            getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
            *tcategory = AGT_TYPE_INTEGER;
        }
        else
        {
            /* It's probably the general case ... */
            *tcategory = AGT_TYPE_OTHER;

            /*
             * but first let's look for a cast to json (note: not to
             * jsonb) if it's not built-in.
             */
            if (typoid >= FirstNormalObjectId)
            {
                Oid castfunc;
                CoercionPathType ctype;

                ctype = find_coercion_pathway(JSONOID, typoid,
                                              COERCION_EXPLICIT, &castfunc);
                if (ctype == COERCION_PATH_FUNC && OidIsValid(castfunc))
                {
                    *tcategory = AGT_TYPE_JSONCAST;
                    *outfuncoid = castfunc;
                }
                else
                {
                    /* not a cast type, so just get the usual output func */
                    getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
                }
            }
            else
            {
                /* any other builtin type */
                getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
            }
            break;
        }
    }
}

/*
 * Turn a Datum into agtype, adding it to the result agtype_in_state.
 *
 * tcategory and outfuncoid are from a previous call to agtype_categorize_type,
 * except that if is_null is true then they can be invalid.
 *
 * If key_scalar is true, the value is stored as a key, so insist
 * it's of an acceptable type, and force it to be a AGTV_STRING.
 */
static void datum_to_agtype(Datum val, bool is_null, agtype_in_state *result,
                            agt_type_category tcategory, Oid outfuncoid,
                            bool key_scalar)
{
    char *outputstr;
    bool numeric_error;
    agtype_value agtv;
    bool scalar_agtype = false;

    check_stack_depth();

    /* Convert val to an agtype_value in agtv (in most cases) */
    if (is_null)
    {
        Assert(!key_scalar);
        agtv.type = AGTV_NULL;
    }
    else if (key_scalar &&
             (tcategory == AGT_TYPE_ARRAY || tcategory == AGT_TYPE_COMPOSITE ||
              tcategory == AGT_TYPE_JSON || tcategory == AGT_TYPE_JSONB ||
              tcategory == AGT_TYPE_AGTYPE || tcategory == AGT_TYPE_JSONCAST))
    {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg(
                 "key value must be scalar, not array, composite, or json")));
    }
    else
    {
        if (tcategory == AGT_TYPE_JSONCAST)
            val = OidFunctionCall1(outfuncoid, val);

        switch (tcategory)
        {
        case AGT_TYPE_ARRAY:
            array_to_agtype_internal(val, result);
            break;
        case AGT_TYPE_COMPOSITE:
            composite_to_agtype(val, result);
            break;
        case AGT_TYPE_BOOL:
            if (key_scalar)
            {
                outputstr = DatumGetBool(val) ? "true" : "false";
                agtv.type = AGTV_STRING;
                agtv.val.string.len = strlen(outputstr);
                agtv.val.string.val = outputstr;
            }
            else
            {
                agtv.type = AGTV_BOOL;
                agtv.val.boolean = DatumGetBool(val);
            }
            break;
        case AGT_TYPE_INTEGER:
            outputstr = OidOutputFunctionCall(outfuncoid, val);
            if (key_scalar)
            {
                agtv.type = AGTV_STRING;
                agtv.val.string.len = strlen(outputstr);
                agtv.val.string.val = outputstr;
            }
            else
            {
                Datum intd;

                intd = DirectFunctionCall1(int8in, CStringGetDatum(outputstr));
                agtv.type = AGTV_INTEGER;
                agtv.val.int_value = DatumGetInt64(intd);
                pfree(outputstr);
            }
            break;
        case AGT_TYPE_FLOAT:
            outputstr = OidOutputFunctionCall(outfuncoid, val);
            if (key_scalar)
            {
                agtv.type = AGTV_STRING;
                agtv.val.string.len = strlen(outputstr);
                agtv.val.string.val = outputstr;
            }
            else
            {
                agtv.type = AGTV_FLOAT;
                agtv.val.float_value = DatumGetFloat8(val);
            }
            break;
        case AGT_TYPE_NUMERIC:
            outputstr = OidOutputFunctionCall(outfuncoid, val);
            if (key_scalar)
            {
                /* always quote keys */
                agtv.type = AGTV_STRING;
                agtv.val.string.len = strlen(outputstr);
                agtv.val.string.val = outputstr;
            }
            else
            {
                /*
                 * Make it numeric if it's a valid agtype number, otherwise
                 * a string. Invalid numeric output will always have an
                 * 'N' or 'n' in it (I think).
                 */
                numeric_error = (strchr(outputstr, 'N') != NULL ||
                                 strchr(outputstr, 'n') != NULL);
                if (!numeric_error)
                {
                    Datum numd;

                    agtv.type = AGTV_NUMERIC;
                    numd = DirectFunctionCall3(numeric_in,
                                               CStringGetDatum(outputstr),
                                               ObjectIdGetDatum(InvalidOid),
                                               Int32GetDatum(-1));
                    agtv.val.numeric = DatumGetNumeric(numd);
                    pfree(outputstr);
                }
                else
                {
                    agtv.type = AGTV_STRING;
                    agtv.val.string.len = strlen(outputstr);
                    agtv.val.string.val = outputstr;
                }
            }
            break;
        case AGT_TYPE_DATE:
            agtv.type = AGTV_STRING;
            agtv.val.string.val = agtype_encode_date_time(NULL, val, DATEOID);
            agtv.val.string.len = strlen(agtv.val.string.val);
            break;
        case AGT_TYPE_TIMESTAMP:
            agtv.type = AGTV_STRING;
            agtv.val.string.val = agtype_encode_date_time(NULL, val,
                                                          TIMESTAMPOID);
            agtv.val.string.len = strlen(agtv.val.string.val);
            break;
        case AGT_TYPE_TIMESTAMPTZ:
            agtv.type = AGTV_STRING;
            agtv.val.string.val = agtype_encode_date_time(NULL, val,
                                                          TIMESTAMPTZOID);
            agtv.val.string.len = strlen(agtv.val.string.val);
            break;
        case AGT_TYPE_JSONCAST:
        case AGT_TYPE_JSON:
        {
            /*
             * Parse the json right into the existing result object.
             * We can handle it as an agtype because agtype is currently an
             * extension of json.
             * Unlike AGT_TYPE_JSONB, numbers will be stored as either
             * an integer or a float, not a numeric.
             */
            agtype_lex_context *lex;
            agtype_sem_action sem;
            text *json = DatumGetTextPP(val);

            lex = make_agtype_lex_context(json, true);

            memset(&sem, 0, sizeof(sem));

            sem.semstate = (void *)result;

            sem.object_start = agtype_in_object_start;
            sem.array_start = agtype_in_array_start;
            sem.object_end = agtype_in_object_end;
            sem.array_end = agtype_in_array_end;
            sem.scalar = agtype_in_scalar;
            sem.object_field_start = agtype_in_object_field_start;

            parse_agtype(lex, &sem);
        }
        break;
        case AGT_TYPE_AGTYPE:
        case AGT_TYPE_JSONB:
        {
            agtype *jsonb = DATUM_GET_AGTYPE_P(val);
            agtype_iterator *it;

            /*
             * val is actually jsonb datum but we can handle it as an agtype
             * datum because agtype is currently an extension of jsonb.
             */

            it = agtype_iterator_init(&jsonb->root);

            if (AGT_ROOT_IS_SCALAR(jsonb))
            {
                agtype_iterator_next(&it, &agtv, true);
                Assert(agtv.type == AGTV_ARRAY);
                agtype_iterator_next(&it, &agtv, true);
                scalar_agtype = true;
            }
            else
            {
                agtype_iterator_token type;

                while ((type = agtype_iterator_next(&it, &agtv, false)) !=
                       WAGT_DONE)
                {
                    if (type == WAGT_END_ARRAY || type == WAGT_END_OBJECT ||
                        type == WAGT_BEGIN_ARRAY || type == WAGT_BEGIN_OBJECT)
                    {
                        result->res = push_agtype_value(&result->parse_state,
                                                        type, NULL);
                    }
                    else
                    {
                        result->res = push_agtype_value(&result->parse_state,
                                                        type, &agtv);
                    }
                }
            }
        }
        break;
        default:
            outputstr = OidOutputFunctionCall(outfuncoid, val);
            agtv.type = AGTV_STRING;
            agtv.val.string.len = check_string_length(strlen(outputstr));
            agtv.val.string.val = outputstr;
            break;
        }
    }

    /* Now insert agtv into result, unless we did it recursively */
    if (!is_null && !scalar_agtype && tcategory >= AGT_TYPE_AGTYPE &&
        tcategory <= AGT_TYPE_JSONCAST)
    {
        /* work has been done recursively */
        return;
    }
    else if (result->parse_state == NULL)
    {
        /* single root scalar */
        agtype_value va;

        va.type = AGTV_ARRAY;
        va.val.array.raw_scalar = true;
        va.val.array.num_elems = 1;

        result->res = push_agtype_value(&result->parse_state, WAGT_BEGIN_ARRAY,
                                        &va);
        result->res = push_agtype_value(&result->parse_state, WAGT_ELEM,
                                        &agtv);
        result->res = push_agtype_value(&result->parse_state, WAGT_END_ARRAY,
                                        NULL);
    }
    else
    {
        agtype_value *o = &result->parse_state->cont_val;

        switch (o->type)
        {
        case AGTV_ARRAY:
            result->res = push_agtype_value(&result->parse_state, WAGT_ELEM,
                                            &agtv);
            break;
        case AGTV_OBJECT:
            result->res = push_agtype_value(&result->parse_state,
                                            key_scalar ? WAGT_KEY : WAGT_VALUE,
                                            &agtv);
            break;
        default:
            elog(ERROR, "unexpected parent of nested structure");
        }
    }
}

/*
 * Process a single dimension of an array.
 * If it's the innermost dimension, output the values, otherwise call
 * ourselves recursively to process the next dimension.
 */
static void array_dim_to_agtype(agtype_in_state *result, int dim, int ndims,
                                int *dims, Datum *vals, bool *nulls,
                                int *valcount, agt_type_category tcategory,
                                Oid outfuncoid)
{
    int i;

    Assert(dim < ndims);

    result->res = push_agtype_value(&result->parse_state, WAGT_BEGIN_ARRAY,
                                    NULL);

    for (i = 1; i <= dims[dim]; i++)
    {
        if (dim + 1 == ndims)
        {
            datum_to_agtype(vals[*valcount], nulls[*valcount], result,
                            tcategory, outfuncoid, false);
            (*valcount)++;
        }
        else
        {
            array_dim_to_agtype(result, dim + 1, ndims, dims, vals, nulls,
                                valcount, tcategory, outfuncoid);
        }
    }

    result->res = push_agtype_value(&result->parse_state, WAGT_END_ARRAY,
                                    NULL);
}

/*
 * Turn an array into agtype.
 */
static void array_to_agtype_internal(Datum array, agtype_in_state *result)
{
    ArrayType *v = DatumGetArrayTypeP(array);
    Oid element_type = ARR_ELEMTYPE(v);
    int *dim;
    int ndim;
    int nitems;
    int count = 0;
    Datum *elements;
    bool *nulls;
    int16 typlen;
    bool typbyval;
    char typalign;
    agt_type_category tcategory;
    Oid outfuncoid;

    ndim = ARR_NDIM(v);
    dim = ARR_DIMS(v);
    nitems = ArrayGetNItems(ndim, dim);

    if (nitems <= 0)
    {
        result->res = push_agtype_value(&result->parse_state, WAGT_BEGIN_ARRAY,
                                        NULL);
        result->res = push_agtype_value(&result->parse_state, WAGT_END_ARRAY,
                                        NULL);
        return;
    }

    get_typlenbyvalalign(element_type, &typlen, &typbyval, &typalign);

    agtype_categorize_type(element_type, &tcategory, &outfuncoid);

    deconstruct_array(v, element_type, typlen, typbyval, typalign, &elements,
                      &nulls, &nitems);

    array_dim_to_agtype(result, 0, ndim, dim, elements, nulls, &count,
                        tcategory, outfuncoid);

    pfree(elements);
    pfree(nulls);
}

/*
 * Turn a composite / record into agtype.
 */
static void composite_to_agtype(Datum composite, agtype_in_state *result)
{
    HeapTupleHeader td;
    Oid tup_type;
    int32 tup_typmod;
    TupleDesc tupdesc;
    HeapTupleData tmptup, *tuple;
    int i;

    td = DatumGetHeapTupleHeader(composite);

    /* Extract rowtype info and find a tupdesc */
    tup_type = HeapTupleHeaderGetTypeId(td);
    tup_typmod = HeapTupleHeaderGetTypMod(td);
    tupdesc = lookup_rowtype_tupdesc(tup_type, tup_typmod);

    /* Build a temporary HeapTuple control structure */
    tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
    tmptup.t_data = td;
    tuple = &tmptup;

    result->res = push_agtype_value(&result->parse_state, WAGT_BEGIN_OBJECT,
                                    NULL);

    for (i = 0; i < tupdesc->natts; i++)
    {
        Datum val;
        bool isnull;
        char *attname;
        agt_type_category tcategory;
        Oid outfuncoid;
        agtype_value v;
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);

        if (att->attisdropped)
            continue;

        attname = NameStr(att->attname);

        v.type = AGTV_STRING;
        /*
         * don't need check_string_length here
         * - can't exceed maximum name length
         */
        v.val.string.len = strlen(attname);
        v.val.string.val = attname;

        result->res = push_agtype_value(&result->parse_state, WAGT_KEY, &v);

        val = heap_getattr(tuple, i + 1, tupdesc, &isnull);

        if (isnull)
        {
            tcategory = AGT_TYPE_NULL;
            outfuncoid = InvalidOid;
        }
        else
        {
            agtype_categorize_type(att->atttypid, &tcategory, &outfuncoid);
        }

        datum_to_agtype(val, isnull, result, tcategory, outfuncoid, false);
    }

    result->res = push_agtype_value(&result->parse_state, WAGT_END_OBJECT,
                                    NULL);
    ReleaseTupleDesc(tupdesc);
}

/*
 * Append agtype text for "val" to "result".
 *
 * This is just a thin wrapper around datum_to_agtype.  If the same type
 * will be printed many times, avoid using this; better to do the
 * agtype_categorize_type lookups only once.
 */
static void add_agtype(Datum val, bool is_null, agtype_in_state *result,
                       Oid val_type, bool key_scalar)
{
    agt_type_category tcategory;
    Oid outfuncoid;

    if (val_type == InvalidOid)
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("could not determine input data type")));
    }

    if (is_null)
    {
        tcategory = AGT_TYPE_NULL;
        outfuncoid = InvalidOid;
    }
    else
    {
        agtype_categorize_type(val_type, &tcategory, &outfuncoid);
    }

    datum_to_agtype(val, is_null, result, tcategory, outfuncoid, key_scalar);
}

agtype_value *string_to_agtype_value(char *s)
{
    agtype_value *agtv = palloc(sizeof(agtype_value));

    agtv->type = AGTV_STRING;
    agtv->val.string.len = check_string_length(strlen(s));
    agtv->val.string.val = s;

    return agtv;
}

PG_FUNCTION_INFO_V1(_agtype_build_path);

/*
 * SQL function agtype_build_path(VARIADIC agtype)
 */
Datum _agtype_build_path(PG_FUNCTION_ARGS)
{
    int nargs;
    int i;
    agtype_in_state result;
    Datum *args;
    bool *nulls;
    Oid *types;

    /* build argument values to build the object */
    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

    if (nargs < 3)
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("paths consist of alternating vertices and edges"),
             errhint("paths require at least 2 vertices and 1 edge")));

    if (nargs % 2 == 0)
    {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("paths consist of alternating vertices and edges"),
             errhint("paths require an odd number of elements")));
    }

    memset(&result, 0, sizeof(agtype_in_state));

    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_ARRAY, NULL);

    for (i = 0; i < nargs; i++)
    {
        agtype *agt= (agtype *)args[i];

        if (nulls[i])
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("argument %d must not be null", i + 1)));
        }
        else if (i % 2 == 1 && (types[i] != AGTYPEOID ||
                                !AGTE_IS_AGTYPE(agt->root.children[0]) ||
                                agt->root.children[1] != AGT_HEADER_EDGE))
        {
            ereport(
                ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("paths consist of alternating vertices and edges"),
                 errhint("argument %d must be an edge", i + 1)));
        }
        else if (i % 2 == 0 && (types[i] != AGTYPEOID ||
                                !AGTE_IS_AGTYPE(agt->root.children[0]) ||
                                agt->root.children[1] != AGT_HEADER_VERTEX))
        {
            ereport(
                ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("paths consist of alternating vertices and edges"),
                 errhint("argument %d must be an vertex", i + 1)));
        }

        add_agtype((Datum)agt, false, &result, types[i], false);
    }

    result.res = push_agtype_value(&result.parse_state, WAGT_END_ARRAY, NULL);

    result.res->type = AGTV_PATH;

    PG_RETURN_POINTER(agtype_value_to_agtype(result.res));
}

Datum make_path(List *path)
{
    ListCell *lc;
    agtype_in_state result;
    int i = 1;

    memset(&result, 0, sizeof(agtype_in_state));

    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_ARRAY, NULL);

    if (list_length(path) < 3 || list_length(path) % 2 != 1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("path list is not a valid path")));

    foreach (lc, path)
    {
        agtype *agt= DATUM_GET_AGTYPE_P(PointerGetDatum(lfirst(lc)));
        agtype_value *elem;
        elem = get_ith_agtype_value_from_container(&agt->root, 0);

        if (!agt)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("argument must not be null")));
        }
        else if (i % 2 == 1 && elem->type != AGTV_VERTEX)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("argument %i must be a vertex", i)));
        }
        else if (i % 2 == 0 && elem->type != AGTV_EDGE)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("argument %i must be an edge", i)));
        }

        add_agtype((Datum)agt, false, &result, AGTYPEOID, false);

        i++;
    }

    result.res = push_agtype_value(&result.parse_state, WAGT_END_ARRAY, NULL);

    result.res->type = AGTV_PATH;

    PG_RETURN_POINTER(agtype_value_to_agtype(result.res));
}

PG_FUNCTION_INFO_V1(_agtype_build_vertex);

/*
 * SQL function agtype_build_vertex(graphid, cstring, agtype)
 */
Datum _agtype_build_vertex(PG_FUNCTION_ARGS)
{
    agtype_in_state result;
    graphid id;

    memset(&result, 0, sizeof(agtype_in_state));

    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_OBJECT,
                                   NULL);

    /* process graphid */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("id"));

    if (fcinfo->argnull[0])
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("agtype_build_vertex() graphid cannot be NULL")));

    id = AG_GETARG_GRAPHID(0);
    add_agtype(id, false, &result, GRAPHIDOID, false);

    /* process label */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("label"));

    if (fcinfo->argnull[1])
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("agtype_build_vertex() label cannot be NULL")));

    result.res =
        push_agtype_value(&result.parse_state, WAGT_VALUE,
                          string_to_agtype_value(PG_GETARG_CSTRING(1)));

    /* process properties */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("properties"));

    //if the properties object is null, push an empty object
    if (fcinfo->argnull[2])
    {
        result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_OBJECT,
                                       NULL);
        result.res = push_agtype_value(&result.parse_state, WAGT_END_OBJECT,
                                       NULL);
    }
    else
    {
        agtype *properties = AG_GET_ARG_AGTYPE_P(2);

        if (!AGT_ROOT_IS_OBJECT(properties))
            ereport(
                ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg(
                     "agtype_build_vertex() properties argument must be an object")));

        add_agtype((Datum)properties, false, &result, AGTYPEOID, false);
    }

    result.res = push_agtype_value(&result.parse_state, WAGT_END_OBJECT, NULL);

    result.res->type = AGTV_VERTEX;

    PG_RETURN_POINTER(agtype_value_to_agtype(result.res));
}

Datum make_vertex(Datum id, Datum label, Datum properties)
{
    return DirectFunctionCall3(_agtype_build_vertex,
                     id,
                     label,
                     properties);

}

PG_FUNCTION_INFO_V1(_agtype_build_edge);

/*
 * SQL function agtype_build_edge(graphid, graphid, graphid, cstring, agtype)
 */
Datum _agtype_build_edge(PG_FUNCTION_ARGS)
{
    agtype_in_state result;
    graphid id, start_id, end_id;

    memset(&result, 0, sizeof(agtype_in_state));

    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_OBJECT,
                                   NULL);

    /* process graphid */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("id"));

    if (fcinfo->argnull[0])
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("agtype_build_vertex() graphid cannot be NULL")));

    id = AG_GETARG_GRAPHID(0);
    add_agtype(id, false, &result, GRAPHIDOID, false);

    /* process startid */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("start_id"));

    if (fcinfo->argnull[1])
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("agtype_build_vertex() startid cannot be NULL")));

    start_id = AG_GETARG_GRAPHID(1);
    add_agtype(start_id, false, &result, GRAPHIDOID, false);

    /* process endid */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("end_id"));

    if (fcinfo->argnull[2])
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("agtype_build_vertex() endoid cannot be NULL")));

    end_id = AG_GETARG_GRAPHID(2);
    add_agtype(end_id, false, &result, GRAPHIDOID, false);

    /* process label */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("label"));

    if (fcinfo->argnull[3])
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("agtype_build_vertex() label cannot be NULL")));

    result.res =
        push_agtype_value(&result.parse_state, WAGT_VALUE,
                          string_to_agtype_value(PG_GETARG_CSTRING(3)));

    /* process properties */
    result.res = push_agtype_value(&result.parse_state, WAGT_KEY,
                                   string_to_agtype_value("properties"));

    //if the properties object is null, push an empty object
    if (fcinfo->argnull[4])
    {
        result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_OBJECT,
                                       NULL);
        result.res = push_agtype_value(&result.parse_state, WAGT_END_OBJECT,
                                       NULL);
    }
    else
    {
        agtype *properties = AG_GET_ARG_AGTYPE_P(4);

        if (!AGT_ROOT_IS_OBJECT(properties))
            ereport(
                ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg(
                     "agtype_build_vertex() properties argument must be an object")));

        add_agtype((Datum)properties, false, &result, AGTYPEOID, false);
    }

    result.res = push_agtype_value(&result.parse_state, WAGT_END_OBJECT, NULL);

    result.res->type = AGTV_EDGE;

    PG_RETURN_POINTER(agtype_value_to_agtype(result.res));
}

Datum make_edge(Datum id, Datum startid, Datum endid, Datum label,
                   Datum properties)
{
    return DirectFunctionCall5(_agtype_build_edge,
                     id, startid, endid,
                     label,
                     properties);

}


PG_FUNCTION_INFO_V1(agtype_build_map);

/*
 * SQL function agtype_build_map(variadic "any")
 */
Datum agtype_build_map(PG_FUNCTION_ARGS)
{
    int nargs;
    int i;
    agtype_in_state result;
    Datum *args;
    bool *nulls;
    Oid *types;

    /* build argument values to build the object */
    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

    if (nargs < 0)
        PG_RETURN_NULL();

    if (nargs % 2 != 0)
    {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("argument list must have been even number of elements"),
             errhint(
                 "The arguments of agtype_build_map() must consist of alternating keys and values.")));
    }

    memset(&result, 0, sizeof(agtype_in_state));

    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_OBJECT,
                                   NULL);

    for (i = 0; i < nargs; i += 2)
    {
        /* process key */
        if (nulls[i])
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("argument %d: key must not be null", i + 1)));
        }

        add_agtype(args[i], false, &result, types[i], true);

        /* process value */
        add_agtype(args[i + 1], nulls[i + 1], &result, types[i + 1], false);
    }

    result.res = push_agtype_value(&result.parse_state, WAGT_END_OBJECT, NULL);

    PG_RETURN_POINTER(agtype_value_to_agtype(result.res));
}

PG_FUNCTION_INFO_V1(agtype_build_map_noargs);

/*
 * degenerate case of agtype_build_map where it gets 0 arguments.
 */
Datum agtype_build_map_noargs(PG_FUNCTION_ARGS)
{
    agtype_in_state result;

    memset(&result, 0, sizeof(agtype_in_state));

    push_agtype_value(&result.parse_state, WAGT_BEGIN_OBJECT, NULL);
    result.res = push_agtype_value(&result.parse_state, WAGT_END_OBJECT, NULL);

    PG_RETURN_POINTER(agtype_value_to_agtype(result.res));
}

PG_FUNCTION_INFO_V1(agtype_build_list);

/*
 * SQL function agtype_build_list(variadic "any")
 */
Datum agtype_build_list(PG_FUNCTION_ARGS)
{
    int nargs;
    int i;
    agtype_in_state result;
    Datum *args;
    bool *nulls;
    Oid *types;

    /*build argument values to build the array */
    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

    if (nargs < 0)
        PG_RETURN_NULL();

    memset(&result, 0, sizeof(agtype_in_state));

    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_ARRAY,
                                   NULL);

    for (i = 0; i < nargs; i++)
        add_agtype(args[i], nulls[i], &result, types[i], false);

    result.res = push_agtype_value(&result.parse_state, WAGT_END_ARRAY, NULL);

    PG_RETURN_POINTER(agtype_value_to_agtype(result.res));
}

PG_FUNCTION_INFO_V1(agtype_build_list_noargs);

/*
 * degenerate case of agtype_build_list where it gets 0 arguments.
 */
Datum agtype_build_list_noargs(PG_FUNCTION_ARGS)
{
    agtype_in_state result;

    memset(&result, 0, sizeof(agtype_in_state));

    push_agtype_value(&result.parse_state, WAGT_BEGIN_ARRAY, NULL);
    result.res = push_agtype_value(&result.parse_state, WAGT_END_ARRAY, NULL);

    PG_RETURN_POINTER(agtype_value_to_agtype(result.res));
}

/*
 * Extract scalar value from raw-scalar pseudo-array agtype.
 */
static bool agtype_extract_scalar(agtype_container *agtc, agtype_value *res)
{
    agtype_iterator *it;
    agtype_iterator_token tok PG_USED_FOR_ASSERTS_ONLY;
    agtype_value tmp;

    if (!AGTYPE_CONTAINER_IS_ARRAY(agtc) || !AGTYPE_CONTAINER_IS_SCALAR(agtc))
    {
        /* inform caller about actual type of container */
        res->type = AGTYPE_CONTAINER_IS_ARRAY(agtc) ? AGTV_ARRAY : AGTV_OBJECT;
        return false;
    }

    /*
     * A root scalar is stored as an array of one element, so we get the array
     * and then its first (and only) member.
     */
    it = agtype_iterator_init(agtc);

    tok = agtype_iterator_next(&it, &tmp, true);
    Assert(tok == WAGT_BEGIN_ARRAY);
    Assert(tmp.val.array.num_elems == 1 && tmp.val.array.raw_scalar);

    tok = agtype_iterator_next(&it, res, true);
    Assert(tok == WAGT_ELEM);
    Assert(IS_A_AGTYPE_SCALAR(res));

    tok = agtype_iterator_next(&it, &tmp, true);
    Assert(tok == WAGT_END_ARRAY);

    tok = agtype_iterator_next(&it, &tmp, true);
    Assert(tok == WAGT_DONE);

    return true;
}

/*
 * Emit correct, translatable cast error message
 */
static void cannot_cast_agtype_value(enum agtype_value_type type,
                                     const char *sqltype)
{
    static const struct
    {
        enum agtype_value_type type;
        const char *msg;
    } messages[] = {
        {AGTV_NULL, gettext_noop("cannot cast agtype null to type %s")},
        {AGTV_STRING, gettext_noop("cannot cast agtype string to type %s")},
        {AGTV_NUMERIC, gettext_noop("cannot cast agtype numeric to type %s")},
        {AGTV_INTEGER, gettext_noop("cannot cast agtype integer to type %s")},
        {AGTV_FLOAT, gettext_noop("cannot cast agtype float to type %s")},
        {AGTV_BOOL, gettext_noop("cannot cast agtype boolean to type %s")},
        {AGTV_ARRAY, gettext_noop("cannot cast agtype array to type %s")},
        {AGTV_OBJECT, gettext_noop("cannot cast agtype object to type %s")},
        {AGTV_BINARY,
         gettext_noop("cannot cast agtype array or object to type %s")}};
    int i;

    for (i = 0; i < lengthof(messages); i++)
    {
        if (messages[i].type == type)
        {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg(messages[i].msg, sqltype)));
        }
    }

    /* should be unreachable */
    elog(ERROR, "unknown agtype type: %d", (int)type);
}

PG_FUNCTION_INFO_V1(agtype_to_bool);

/*
 * Cast agtype to boolean. From jsonb_bool().
 */
Datum agtype_to_bool(PG_FUNCTION_ARGS)
{
    agtype *agtype_in = AG_GET_ARG_AGTYPE_P(0);
    agtype_value agtv;

    if (!agtype_extract_scalar(&agtype_in->root, &agtv) ||
        agtv.type != AGTV_BOOL)
        cannot_cast_agtype_value(agtv.type, "boolean");

    PG_FREE_IF_COPY(agtype_in, 0);

    PG_RETURN_BOOL(agtv.val.boolean);
}

PG_FUNCTION_INFO_V1(bool_to_agtype);

/*
 * Cast boolean to agtype.
 */
Datum bool_to_agtype(PG_FUNCTION_ARGS)
{
    return boolean_to_agtype(PG_GETARG_BOOL(0));
}

/*
 * Helper function for agtype_access_operator map access.
 * Note: This function expects that a map and a scalar key are being passed.
 */
static agtype *execute_map_access_operator(agtype *map, agtype *key)
{
    agtype_value *key_value;
    agtype_value *map_value;
    agtype_value new_key_value;

    key_value = get_ith_agtype_value_from_container(&key->root, 0);
    /* transform key where appropriate */
    new_key_value.type = AGTV_STRING;
    switch (key_value->type)
    {
    case AGTV_NULL:
        return NULL;

    case AGTV_INTEGER:
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_INTEGER is not a valid key type")));
    case AGTV_FLOAT:
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_FLOAT is not a valid key type")));
    case AGTV_NUMERIC:
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_NUMERIC is not a valid key type")));
    case AGTV_BOOL:
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("AGTV_BOOL is not a valid key type")));

    case AGTV_STRING:
        new_key_value.val.string = key_value->val.string;
        break;

    default:
        ereport(ERROR, (errmsg("unknown agtype scalar type")));
        break;
    }

    map_value = find_agtype_value_from_container(&map->root, AGT_FOBJECT,
                                                 &new_key_value);
    if (map_value == NULL)
        return NULL;

    return agtype_value_to_agtype(map_value);
}

/*
 * Helper function for agtype_access_operator array access.
 * Note: This function expects that an array and a scalar key are being passed.
 */
static agtype *execute_array_access_operator(agtype *array, agtype *element)
{
    agtype_value *array_value;
    agtype_value *element_value;
    int64 index;
    uint32 size;

    element_value = get_ith_agtype_value_from_container(&element->root, 0);
    /* if AGTV_NULL return NULL */
    if (element_value->type == AGTV_NULL)
        return NULL;
    /* key must be an integer */
    if (element_value->type != AGTV_INTEGER)
        ereport(ERROR,
                (errmsg("array index must resolve to an integer value")));
    /* adjust for negative index values */
    index = element_value->val.int_value;
    size = AGT_ROOT_COUNT(array);
    if (index < 0)
        index = size + index;
    /* check array bounds */
    if ((index >= size) || (index < 0))
        return NULL;

    array_value = get_ith_agtype_value_from_container(&array->root, index);

    if (array_value == NULL)
        return NULL;

    return agtype_value_to_agtype(array_value);
}

PG_FUNCTION_INFO_V1(agtype_access_operator);
/*
 * Execution function for object.property, object["property"],
 * and array[element]
 */
Datum agtype_access_operator(PG_FUNCTION_ARGS)
{
    int nargs;
    Datum *args;
    bool *nulls;
    Oid *types;
    agtype *object;
    agtype *key;
    int i;

    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);
    /* we need at least 2 parameters, the object, and a field or element */
    if (nargs < 2)
        PG_RETURN_NULL();

    object = DATUM_GET_AGTYPE_P(args[0]);
    if (AGT_ROOT_IS_SCALAR(object))
    {
        agtype_value *v;
        v = get_ith_agtype_value_from_container(&object->root, 0);

        if (v->type == AGTV_VERTEX)
            object = agtype_value_to_agtype(&v->val.object.pairs[2].value);
        else if (v->type == AGTV_EDGE)
            object = agtype_value_to_agtype(&v->val.object.pairs[4].value);
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("container must be an array or object")));

        //object = agtype_value_to_agtype(&v->val.object.pairs[2].value);
    }

    for (i = 1; i < nargs; i++)
    {
        /* if we have a null, return null */
        if (nulls[i] == true)
            PG_RETURN_NULL();

        key = DATUM_GET_AGTYPE_P(args[i]);
        if (!(AGT_ROOT_IS_SCALAR(key)))
        {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("key must resolve to a scalar value")));
        }

        if (AGT_ROOT_IS_OBJECT(object))
            object = execute_map_access_operator(object, key);
        else if (AGT_ROOT_IS_ARRAY(object))
            object = execute_array_access_operator(object, key);
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("container must be an array or object")));

        if (object == NULL)
            PG_RETURN_NULL();
    }

    return AGTYPE_P_GET_DATUM(object);
}

PG_FUNCTION_INFO_V1(agtype_access_slice);
/*
 * Execution function for list slices
 */
Datum agtype_access_slice(PG_FUNCTION_ARGS)
{
    agtype_value *lidx_value = NULL;
    agtype_value *uidx_value = NULL;
    agtype_in_state result;
    agtype *array;
    int64 upper_index = 0;
    int64 lower_index = 0;
    uint32 array_size;
    int64 i;

    /* return null if the array to slice is null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();
    /* return an error if both indices are NULL */
    if (PG_ARGISNULL(1) && PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("slice start and/or end is required")));
    /* get the array parameter and verify that it is a list */
    array = AG_GET_ARG_AGTYPE_P(0);
    if (!AGT_ROOT_IS_ARRAY(array) || AGT_ROOT_IS_SCALAR(array))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("slice must access a list")));
    array_size = AGT_ROOT_COUNT(array);
    /* if we don't have a lower bound, make it 0 */
    if (PG_ARGISNULL(1))
        lower_index = 0;
    else
    {
        lidx_value = get_ith_agtype_value_from_container(
            &AG_GET_ARG_AGTYPE_P(1)->root, 0);
        /* adjust for AGTV_NULL */
        if (lidx_value->type == AGTV_NULL)
        {
            lower_index = 0;
            lidx_value = NULL;
        }
    }
    /* if we don't have an upper bound, make it the size of the array */
    if (PG_ARGISNULL(2))
        upper_index = array_size;
    else
    {
        uidx_value = get_ith_agtype_value_from_container(
            &AG_GET_ARG_AGTYPE_P(2)->root, 0);
        /* adjust for AGTV_NULL */
        if (uidx_value->type == AGTV_NULL)
        {
            upper_index = array_size;
            uidx_value = NULL;
        }
    }
    /* if both indices are NULL (AGTV_NULL) return an error */
    if (lidx_value == NULL && uidx_value == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("slice start and/or end is required")));
    /* key must be an integer or NULL */
    if ((lidx_value != NULL && lidx_value->type != AGTV_INTEGER) ||
        (uidx_value != NULL && uidx_value->type != AGTV_INTEGER))
        ereport(ERROR,
                (errmsg("array slices must resolve to an integer value")));
    /* set indices if not already set */
    if (lidx_value)
        lower_index = lidx_value->val.int_value;
    if (uidx_value)
        upper_index = uidx_value->val.int_value;
    /* adjust for negative and out of bounds index values */
    if (lower_index < 0)
        lower_index = array_size + lower_index;
    if (lower_index < 0)
        lower_index = 0;
    if (lower_index > array_size)
        lower_index = array_size;
    if (upper_index < 0)
        upper_index = array_size + upper_index;
    if (upper_index < 0)
        upper_index = 0;
    if (upper_index > array_size)
        upper_index = array_size;

    memset(&result, 0, sizeof(agtype_in_state));

    result.res = push_agtype_value(&result.parse_state, WAGT_BEGIN_ARRAY,
                                   NULL);

    /* get array elements */
    for (i = lower_index; i < upper_index; i++)
        result.res = push_agtype_value(
            &result.parse_state, WAGT_ELEM,
            get_ith_agtype_value_from_container(&array->root, i));

    result.res = push_agtype_value(&result.parse_state, WAGT_END_ARRAY, NULL);

    PG_RETURN_POINTER(agtype_value_to_agtype(result.res));
}

PG_FUNCTION_INFO_V1(agtype_in_operator);
/*
 * Execute function for IN operator
 */
Datum agtype_in_operator(PG_FUNCTION_ARGS)
{
    agtype *agt_array, *agt_item;
    agtype_iterator *it_array, *it_item;
    agtype_value agtv_item, agtv_elem;
    uint32 array_size = 0;
    bool result = false;
    uint32 i = 0;

    /* return null if the array is null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    /* get the array parameter and verify that it is a list */
    agt_array = AG_GET_ARG_AGTYPE_P(0);
    if (!AGT_ROOT_IS_ARRAY(agt_array))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("object of IN must be a list")));

    /* init array iterator */
    it_array = agtype_iterator_init(&agt_array->root);
    /* open array container */
    agtype_iterator_next(&it_array, &agtv_elem, false);
    /* check for an array scalar value */
    if (agtv_elem.type == AGTV_ARRAY && agtv_elem.val.array.raw_scalar)
    {
        agtype_iterator_next(&it_array, &agtv_elem, false);
        /* check for AGTYPE NULL */
        if (agtv_elem.type == AGTV_NULL)
            PG_RETURN_NULL();
        /* if it is a scalar, but not AGTV_NULL, error out */
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("object of IN must be a list")));
    }

    array_size = AGT_ROOT_COUNT(agt_array);

    /* return null if the item to find is null */
    if (PG_ARGISNULL(1))
        PG_RETURN_NULL();
    /* get the item to search for */
    agt_item = AG_GET_ARG_AGTYPE_P(1);

    /* init item iterator */
    it_item = agtype_iterator_init(&agt_item->root);

    /* get value of item */
    agtype_iterator_next(&it_item, &agtv_item, false);
    if (agtv_item.type == AGTV_ARRAY && agtv_item.val.array.raw_scalar)
    {
        agtype_iterator_next(&it_item, &agtv_item, false);
        /* check for AGTYPE NULL */
        if (agtv_item.type == AGTV_NULL)
            PG_RETURN_NULL();
    }

    /* iterate through the array, but stop if we find it */
    for (i = 0; i < array_size && !result; i++)
    {
        /* get next element */
        agtype_iterator_next(&it_array, &agtv_elem, true);
        /* if both are containers, compare containers */
        if (!IS_A_AGTYPE_SCALAR(&agtv_item) && !IS_A_AGTYPE_SCALAR(&agtv_elem))
        {
            result = (compare_agtype_containers_orderability(
                          &agt_item->root, agtv_elem.val.binary.data) == 0);
        }
        /* if both are scalars and of the same type, compare scalars */
        else if (IS_A_AGTYPE_SCALAR(&agtv_item) &&
                 IS_A_AGTYPE_SCALAR(&agtv_elem) &&
                 agtv_item.type == agtv_elem.type)
            result = (compare_agtype_scalar_values(&agtv_item, &agtv_elem) ==
                      0);
    }
    return boolean_to_agtype(result);
}

PG_FUNCTION_INFO_V1(agtype_string_match_starts_with);
/*
 * Execution function for STARTS WITH
 */
Datum agtype_string_match_starts_with(PG_FUNCTION_ARGS)
{
    agtype *lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *rhs = AG_GET_ARG_AGTYPE_P(1);

    if (AGT_ROOT_IS_SCALAR(lhs) && AGT_ROOT_IS_SCALAR(rhs))
    {
        agtype_value *lhs_value;
        agtype_value *rhs_value;

        lhs_value = get_ith_agtype_value_from_container(&lhs->root, 0);
        rhs_value = get_ith_agtype_value_from_container(&rhs->root, 0);

        if (lhs_value->type == AGTV_STRING && rhs_value->type == AGTV_STRING)
        {
            if (lhs_value->val.string.len < rhs_value->val.string.len)
                return boolean_to_agtype(false);

            if (strncmp(lhs_value->val.string.val, rhs_value->val.string.val,
                        rhs_value->val.string.len) == 0)
                return boolean_to_agtype(true);
            else
                return boolean_to_agtype(false);
        }
    }
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("agtype string values expected")));
}

PG_FUNCTION_INFO_V1(agtype_string_match_ends_with);
/*
 * Execution function for ENDS WITH
 */
Datum agtype_string_match_ends_with(PG_FUNCTION_ARGS)
{
    agtype *lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *rhs = AG_GET_ARG_AGTYPE_P(1);

    if (AGT_ROOT_IS_SCALAR(lhs) && AGT_ROOT_IS_SCALAR(rhs))
    {
        agtype_value *lhs_value;
        agtype_value *rhs_value;

        lhs_value = get_ith_agtype_value_from_container(&lhs->root, 0);
        rhs_value = get_ith_agtype_value_from_container(&rhs->root, 0);

        if (lhs_value->type == AGTV_STRING && rhs_value->type == AGTV_STRING)
        {
            if (lhs_value->val.string.len < rhs_value->val.string.len)
                return boolean_to_agtype(false);

            if (strncmp(lhs_value->val.string.val + lhs_value->val.string.len -
                            rhs_value->val.string.len,
                        rhs_value->val.string.val,
                        rhs_value->val.string.len) == 0)
                return boolean_to_agtype(true);
            else
                return boolean_to_agtype(false);
        }
    }
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("agtype string values expected")));
}

PG_FUNCTION_INFO_V1(agtype_string_match_contains);
/*
 * Execution function for CONTAINS
 */
Datum agtype_string_match_contains(PG_FUNCTION_ARGS)
{
    agtype *lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *rhs = AG_GET_ARG_AGTYPE_P(1);

    if (AGT_ROOT_IS_SCALAR(lhs) && AGT_ROOT_IS_SCALAR(rhs))
    {
        agtype_value *lhs_value;
        agtype_value *rhs_value;

        lhs_value = get_ith_agtype_value_from_container(&lhs->root, 0);
        rhs_value = get_ith_agtype_value_from_container(&rhs->root, 0);

        if (lhs_value->type == AGTV_STRING && rhs_value->type == AGTV_STRING)
        {
            char *l;
            char *r;

            if (lhs_value->val.string.len < rhs_value->val.string.len)
                return boolean_to_agtype(false);

            l = pnstrdup(lhs_value->val.string.val, lhs_value->val.string.len);
            r = pnstrdup(rhs_value->val.string.val, rhs_value->val.string.len);

            if (strstr(l, r) == NULL)
                return boolean_to_agtype(false);
            else
                return boolean_to_agtype(true);
        }
    }
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("agtype string values expected")));
}

static bool is_agtype_null(agtype *agt)
{
    if (AGT_ROOT_IS_ARRAY(agt) && AGT_ROOT_IS_SCALAR(agt))
    {
        agtype_value *agtv_element;

        agtv_element = get_ith_agtype_value_from_container(&agt->root, 0);

        if (agtv_element->type == AGTV_NULL)
            return true;
    }

    return false;
}

PG_FUNCTION_INFO_V1(agtype_typecast_numeric);
/*
 * Execute function to typecast an agtype to an agtype numeric
 */
Datum agtype_typecast_numeric(PG_FUNCTION_ARGS)
{
    agtype *arg_agt;
    agtype_value *arg_value;
    agtype_value result_value;
    Datum numd;
    char *string = NULL;

    /* return null if arg is null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    /* check that we have a scalar value */
    arg_agt = AG_GET_ARG_AGTYPE_P(0);
    if (!AGT_ROOT_IS_SCALAR(arg_agt))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast argument must resolve to a scalar value")));

    /* get the arg parameter */
    arg_value = get_ith_agtype_value_from_container(&arg_agt->root, 0);
    /* check for agtype null */
    if (arg_value->type == AGTV_NULL)
        PG_RETURN_NULL();

    /* the input type drives the casting */
    switch(arg_value->type)
    {
    case AGTV_INTEGER:
        numd = DirectFunctionCall1(int8_numeric,
                                   Int64GetDatum(arg_value->val.int_value));
        break;
    case AGTV_FLOAT:
        numd = DirectFunctionCall1(float8_numeric,
                                   Float8GetDatum(arg_value->val.float_value));
        break;
    case AGTV_NUMERIC:
        /* it is already a numeric so just return it */
        PG_RETURN_POINTER(agtype_value_to_agtype(arg_value));
        break;
    /* this allows string numbers and NaN */
    case AGTV_STRING:
        /* we need a null terminated string */
        string = (char *) palloc(sizeof(char)*arg_value->val.string.len + 1);
        string = strncpy(string, arg_value->val.string.val,
                         arg_value->val.string.len);
        string[arg_value->val.string.len] = '\0';
        /* pass the string to the numeric in function for conversion */
        numd = DirectFunctionCall3(numeric_in,
                                   CStringGetDatum(string),
                                   ObjectIdGetDatum(InvalidOid),
                                   Int32GetDatum(-1));
        /* free the string */
        pfree(string);
        string = NULL;
        break;
    /* what was given doesn't cast to a numeric */
    default:
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast expression must be a number or a string")));
        break;
    }

    /* fill in and return our result */
    result_value.type = AGTV_NUMERIC;
    result_value.val.numeric = DatumGetNumeric(numd);

    PG_RETURN_POINTER(agtype_value_to_agtype(&result_value));
}

PG_FUNCTION_INFO_V1(agtype_typecast_float);
/*
 * Execute function to typecast an agtype to an agtype float
 */
Datum agtype_typecast_float(PG_FUNCTION_ARGS)
{
    agtype *arg_agt;
    agtype_value *arg_value;
    agtype_value result_value;
    Datum d;
    char *string = NULL;

    /* return null if arg is null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    /* check that we have a scalar value */
    arg_agt = AG_GET_ARG_AGTYPE_P(0);
    if (!AGT_ROOT_IS_SCALAR(arg_agt))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast argument must be a scalar value")));

    /* get the arg parameter */
    arg_value = get_ith_agtype_value_from_container(&arg_agt->root, 0);
    /* check for agtype null */
    if (arg_value->type == AGTV_NULL)
        PG_RETURN_NULL();

    /* the input type drives the casting */
    switch(arg_value->type)
    {
    case AGTV_INTEGER:
        d = DirectFunctionCall1(int8out,
                                Int64GetDatum(arg_value->val.int_value));
        d = DirectFunctionCall1(float8in, d);
        break;
    case AGTV_FLOAT:
        /* it is already a float so just return it */
        PG_RETURN_POINTER(agtype_value_to_agtype(arg_value));
        break;
    case AGTV_NUMERIC:
        d = DirectFunctionCall1(numeric_float8,
                                NumericGetDatum(arg_value->val.numeric));
        break;
    /* this allows string numbers, NaN, Infinity, and -Infinity */
    case AGTV_STRING:
        /* we need a null terminated string */
        string = (char *) palloc(sizeof(char)*arg_value->val.string.len + 1);
        string = strncpy(string, arg_value->val.string.val,
                         arg_value->val.string.len);
        string[arg_value->val.string.len] = '\0';

        d = DirectFunctionCall1(float8in, CStringGetDatum(string));
        /* free the string */
        pfree(string);
        string = NULL;
        break;
    /* what was given doesn't cast to a float */
    default:
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast expression must be a number or a string")));
        break;
    }

    /* set the result type and return our result */
    result_value.type = AGTV_FLOAT;
    result_value.val.float_value = DatumGetFloat8(d);

    PG_RETURN_POINTER(agtype_value_to_agtype(&result_value));
}

PG_FUNCTION_INFO_V1(agtype_typecast_vertex);
/*
 * Execute function for typecast to vertex
 */
Datum agtype_typecast_vertex(PG_FUNCTION_ARGS)
{
    agtype *arg_agt;
    agtype_value agtv_key;
    agtype_value *agtv_graphid, *agtv_label, *agtv_properties;
    Datum result;
    int count;

    /* Return null if arg is null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    arg_agt = AG_GET_ARG_AGTYPE_P(0);

    /* Return null if arg is agtype null */
    if (is_agtype_null(arg_agt))
        PG_RETURN_NULL();

    /* A vertex is an object so the arg needs to be one too */
    if (!AGT_ROOT_IS_OBJECT(arg_agt))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("vertex typecast argument must resolve to an object")));

    /* A vertex object has 3 key/value pairs */
    count = AGTYPE_CONTAINER_SIZE(&arg_agt->root);
    if (count != 3)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast object is not a vertex")));

    /*
     * The 3 key/value pairs need to each exist and their names need to match
     * the names used for a vertex.
     */
    agtv_key.type = AGTV_STRING;
    agtv_key.val.string.val = "id";
    agtv_key.val.string.len = 2;
    agtv_graphid = find_agtype_value_from_container(&arg_agt->root,
                                                    AGT_FOBJECT, &agtv_key);
    if (agtv_graphid == NULL || agtv_graphid->type != AGTV_INTEGER)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("vertex typecast object has invalid or missing id")));

    agtv_key.val.string.val = "label";
    agtv_key.val.string.len = 5;
    agtv_label = find_agtype_value_from_container(&arg_agt->root,
                                                  AGT_FOBJECT, &agtv_key);
    if (agtv_label == NULL || agtv_label->type != AGTV_STRING)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("vertex typecast object has invalid or missing label")));

    agtv_key.val.string.val = "properties";
    agtv_key.val.string.len = 10;
    agtv_properties = find_agtype_value_from_container(&arg_agt->root,
                                                       AGT_FOBJECT, &agtv_key);
    if (agtv_properties == NULL ||
        (agtv_properties->type != AGTV_OBJECT &&
         agtv_properties->type != AGTV_BINARY))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("vertex typecast object has invalid or missing properties")));

    /* Hand it off to the build vertex routine */
    result = DirectFunctionCall3(_agtype_build_vertex,
                 Int64GetDatum(agtv_graphid->val.int_value),
                 CStringGetDatum(agtv_label->val.string.val),
                 PointerGetDatum(agtype_value_to_agtype(agtv_properties)));
    return result;
}

PG_FUNCTION_INFO_V1(agtype_typecast_edge);
/*
 * Execute function for typecast to edge
 */
Datum agtype_typecast_edge(PG_FUNCTION_ARGS)
{
    agtype *arg_agt;
    agtype_value agtv_key;
    agtype_value *agtv_graphid, *agtv_label, *agtv_properties,
                 *agtv_startid, *agtv_endid;
    Datum result;
    int count;

    /* Return null if arg is null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    arg_agt = AG_GET_ARG_AGTYPE_P(0);

    /* Return null if arg is agtype null */
    if (is_agtype_null(arg_agt))
        PG_RETURN_NULL();

    /* An edge is an object, so the arg needs to be one too */
    if (!AGT_ROOT_IS_OBJECT(arg_agt))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("edge typecast argument must resolve to an object")));

    /* An edge has 5 key/value pairs */
    count = AGTYPE_CONTAINER_SIZE(&arg_agt->root);
    if (count != 5)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast object is not an edge")));

    /*
     * The 5 key/value pairs need to each exist and their names need to match
     * the names used for an edge.
     */
    agtv_key.type = AGTV_STRING;
    agtv_key.val.string.val = "id";
    agtv_key.val.string.len = 2;
    agtv_graphid = find_agtype_value_from_container(&arg_agt->root,
                                                    AGT_FOBJECT, &agtv_key);
    if (agtv_graphid == NULL || agtv_graphid->type != AGTV_INTEGER)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("edge typecast object has an invalid or missing id")));

    agtv_key.val.string.val = "label";
    agtv_key.val.string.len = 5;
    agtv_label = find_agtype_value_from_container(&arg_agt->root,
                                                  AGT_FOBJECT, &agtv_key);
    if (agtv_label == NULL || agtv_label->type != AGTV_STRING)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("edge typecast object has an invalid or missing label")));

    agtv_key.val.string.val = "properties";
    agtv_key.val.string.len = 10;
    agtv_properties = find_agtype_value_from_container(&arg_agt->root,
                                                 AGT_FOBJECT, &agtv_key);
    if (agtv_properties == NULL ||
        (agtv_properties->type != AGTV_OBJECT &&
         agtv_properties->type != AGTV_BINARY))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("edge typecast object has invalid or missing properties")));

    agtv_key.val.string.val = "start_id";
    agtv_key.val.string.len = 8;
    agtv_startid = find_agtype_value_from_container(&arg_agt->root,
                                                    AGT_FOBJECT, &agtv_key);
    if (agtv_graphid == NULL || agtv_graphid->type != AGTV_INTEGER)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("edge typecast object has an invalid or missing start_id")));

    agtv_key.val.string.val = "end_id";
    agtv_key.val.string.len = 6;
    agtv_endid = find_agtype_value_from_container(&arg_agt->root,
                                                    AGT_FOBJECT, &agtv_key);
    if (agtv_graphid == NULL || agtv_graphid->type != AGTV_INTEGER)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("edge typecast object has an invalid or missing end_id")));

    /* Hand it off to the build edge routine */
    result = DirectFunctionCall5(_agtype_build_edge,
                 Int64GetDatum(agtv_graphid->val.int_value),
                 Int64GetDatum(agtv_startid->val.int_value),
                 Int64GetDatum(agtv_endid->val.int_value),
                 CStringGetDatum(agtv_label->val.string.val),
                 PointerGetDatum(agtype_value_to_agtype(agtv_properties)));
    return result;
}

PG_FUNCTION_INFO_V1(agtype_typecast_path);
/*
 * Execute function for typecast to path
 */
Datum agtype_typecast_path(PG_FUNCTION_ARGS)
{
    agtype *arg_agt = NULL;
    agtype_in_state path;
    agtype_value *agtv_element = NULL;
    int count = 0;
    int i = 0;

    /* return null if arg is null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    arg_agt = AG_GET_ARG_AGTYPE_P(0);

    /* Return null if arg is agtype null */
    if (is_agtype_null(arg_agt))
        PG_RETURN_NULL();

    /* path needs to be an array */
    if (!AGT_ROOT_IS_ARRAY(arg_agt))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("path typecast argument must resolve to an array")));

    count = AGT_ROOT_COUNT(arg_agt);

    /* quick check for valid path lengths */
    if (count < 3 || (count-1) % 2 != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast argument is not a valid path")));

    /* create an agtype array */
    memset(&path, 0, sizeof(agtype_in_state));
    path.res = push_agtype_value(&path.parse_state, WAGT_BEGIN_ARRAY, NULL);

    /*
     * Iterate through the provided list, check that each value conforms, and
     * then add it if it does. Otherwise error out.
     */
    for (i = 0; i+1 < count; i+=2)
    {
        /* get a potential vertex, check it, then add it */
        agtv_element = get_ith_agtype_value_from_container(&arg_agt->root, i);
        if (agtv_element == NULL || agtv_element->type != AGTV_VERTEX)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("typecast argument is not a valid path")));
        push_agtype_value(&path.parse_state, WAGT_ELEM, agtv_element);

        /* get a potential edge, check it, then add it */
        agtv_element = get_ith_agtype_value_from_container(&arg_agt->root, i+1);
        if (agtv_element == NULL || agtv_element->type != AGTV_EDGE)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("typecast argument is not a valid path")));
        push_agtype_value(&path.parse_state, WAGT_ELEM, agtv_element);
    }

    /* validate the last element is a vertex, add it if it is, fail otherwise */
    agtv_element = get_ith_agtype_value_from_container(&arg_agt->root, i);
    if (agtv_element == NULL || agtv_element->type != AGTV_VERTEX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("typecast argument is not a valid path")));
    push_agtype_value(&path.parse_state, WAGT_ELEM, agtv_element);

    /* close the array */
    path.res = push_agtype_value(&path.parse_state, WAGT_END_ARRAY, NULL);
    /* set it to a path */
    path.res->type = AGTV_PATH;

    PG_RETURN_POINTER(agtype_value_to_agtype(path.res));
}

static uint64 get_edge_uniqueness_value(Datum d, Oid type, bool is_null,
                                        int index)
{
    agtype *agt;
    agtype_value *v;

    if (is_null)
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg(
                 "parameter %i in _ag_enforce_edge_uniqueness must not be null",
                 index)));

    if (type != AGTYPEOID)
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg(
                 "parameter %i in _ag_enforce_edge_uniqueness must be an agtype",
                 index)));

    agt = DATUM_GET_AGTYPE_P(d);

    if (!AGT_ROOT_IS_SCALAR(agt))
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg(
                 "agtype parameter %i in _ag_enforce_edge_uniqueness must resolve to an integer",
                 index)));

    v = get_ith_agtype_value_from_container(&agt->root, 0);

    if (v->type != AGTV_INTEGER)
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg(
                 "agtype parameter %i in _ag_enforce_edge_uniqueness must resolve to an integer",
                 index)));

    return v->val.int_value;
}

PG_FUNCTION_INFO_V1(_ag_enforce_edge_uniqueness);

Datum _ag_enforce_edge_uniqueness(PG_FUNCTION_ARGS)
{
    int nargs;
    Datum *args;
    bool *nulls;
    Oid *types;
    int i, j;

    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

    for (i = 0; i < nargs; i++)
    {
        uint64 id_1 = get_edge_uniqueness_value(args[i], types[i],
                                                nulls[i], i);

        for (j = i + 1; j < nargs; j++)
        {
            uint64 id_2 = get_edge_uniqueness_value(args[j], types[j],
                                                    nulls[j], j);

            if (id_1 == id_2)
                return false;
        }
    }

    return true;
}

/* helper function to retrieve a value, given a key, from an agtype_value */
static agtype_value *get_agtype_value_object_value(agtype_value *agtv_object,
                                             char *key)
{
    int i;
    int length;

    Assert(agtv_object != NULL);
    Assert(agtv_object->type != AGTV_OBJECT);
    Assert(key != NULL);

    length = strlen(key);
    for (i = 0; i < agtv_object->val.object.num_pairs; i++)
    {
        agtype_value *agtv_key = &agtv_object->val.object.pairs[i].key;
        agtype_value *agtv_value = &agtv_object->val.object.pairs[i].value;

        Assert(agtv_key != NULL);
        Assert(agtv_key->type == AGTV_STRING);

        if (agtv_key->val.string.len == length &&
            strncmp(agtv_key->val.string.val, key,
                    agtv_key->val.string.len)== 0)
            return agtv_value;
    }

    return NULL;
}

PG_FUNCTION_INFO_V1(id);

Datum id(PG_FUNCTION_ARGS)
{
    agtype *agt_arg = NULL;
    agtype_value *agtv_object = NULL;
    agtype_value *agtv_result = NULL;

    /* check for null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    agt_arg = AG_GET_ARG_AGTYPE_P(0);
    /* check for a scalar object */
    if (!AGT_ROOT_IS_SCALAR(agt_arg))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("id() argument must resolve to a scalar value")));

    /* get the object out of the array */
    agtv_object = get_ith_agtype_value_from_container(&agt_arg->root, 0);

    /* is it an agtype null? */
    if (agtv_object->type == AGTV_NULL)
            PG_RETURN_NULL();

    /* check for proper agtype */
    if (agtv_object->type != AGTV_VERTEX && agtv_object->type != AGTV_EDGE)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("id() argument must be a vertex, an edge or null")));

    agtv_result = get_agtype_value_object_value(agtv_object, "id");

    Assert(agtv_result != NULL);
    Assert(agtv_result->type = AGTV_INTEGER);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_result));
    AG_RETURN_GRAPHID(agtv_result->val.int_value);
}

PG_FUNCTION_INFO_V1(start_id);

Datum start_id(PG_FUNCTION_ARGS)
{
    agtype *agt_arg = NULL;
    agtype_value *agtv_object = NULL;
    agtype_value *agtv_result = NULL;

    /* check for null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    agt_arg = AG_GET_ARG_AGTYPE_P(0);
    /* check for a scalar object */
    if (!AGT_ROOT_IS_SCALAR(agt_arg))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("start_id() argument must resolve to a scalar value")));

    /* get the object out of the array */
    agtv_object = get_ith_agtype_value_from_container(&agt_arg->root, 0);

    /* is it an agtype null? */
    if (agtv_object->type == AGTV_NULL)
            PG_RETURN_NULL();

    /* check for proper agtype */
    if (agtv_object->type != AGTV_EDGE)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("start_id() argument must be an edge or null")));

    agtv_result = get_agtype_value_object_value(agtv_object, "start_id");

    Assert(agtv_result != NULL);
    Assert(agtv_result->type = AGTV_INTEGER);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_result));
}

PG_FUNCTION_INFO_V1(end_id);

Datum end_id(PG_FUNCTION_ARGS)
{
    agtype *agt_arg = NULL;
    agtype_value *agtv_object = NULL;
    agtype_value *agtv_result = NULL;

    /* check for null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    agt_arg = AG_GET_ARG_AGTYPE_P(0);
    /* check for a scalar object */
    if (!AGT_ROOT_IS_SCALAR(agt_arg))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("end_id() argument must resolve to a scalar value")));

    /* get the object out of the array */
    agtv_object = get_ith_agtype_value_from_container(&agt_arg->root, 0);

    /* is it an agtype null? */
    if (agtv_object->type == AGTV_NULL)
            PG_RETURN_NULL();

    /* check for proper agtype */
    if (agtv_object->type != AGTV_EDGE)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("end_id() argument must be an edge or null")));

    agtv_result = get_agtype_value_object_value(agtv_object, "end_id");

    Assert(agtv_result != NULL);
    Assert(agtv_result->type = AGTV_INTEGER);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_result));
}

/*
 * Helper function to return the Datum value of a column (attribute) in a heap
 * tuple (row) given the column number (starting from 0), attribute name, typid,
 * and whether it can be null. The function is designed to extract and validate
 * that the data (attribute) is what is expected. The function will error on any
 * issues.
 */
static Datum column_get_datum(TupleDesc tupdesc, HeapTuple tuple, int column,
                        const char *attname, Oid typid, bool isnull)
{
    Form_pg_attribute att;
    HeapTupleHeader hth;
    HeapTupleData tmptup, *htd;
    Datum result;
    bool _isnull = true;

    /* build the heap tuple data */
    hth = tuple->t_data;
    tmptup.t_len = HeapTupleHeaderGetDatumLength(hth);
    tmptup.t_data = hth;
    htd = &tmptup;

    /* get the description for the column from the tuple descriptor */
    att = TupleDescAttr(tupdesc, column);
    /* get the datum (attribute) for that column*/
    result = heap_getattr(htd, column + 1, tupdesc, &_isnull);
    /* verify that the attribute typid is as expected */
    if (att->atttypid != typid)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("Invalid attribute typid. Expected %d, found %d", typid,
                        att->atttypid)));
    /* verify that the attribute name is as expected */
    if (strcmp(att->attname.data, attname) != 0)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("Invalid attribute name. Expected %s, found %s",
                        attname, att->attname.data)));
    /* verify that if it is null, it is allowed to be null */
    if (isnull == false && _isnull == true)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("Attribute was found to be null when null is not allowed.")));

    return result;
}

/*
 * Function to retrieve a label name, given the graph name and graphid. The
 * function returns a pointer to a duplicated string that needs to be freed
 * when you are finished using it.
 */
static char *get_label_name(const char *graph_name, int64 graphid)
{
    ScanKeyData scan_keys[2];
    Relation ag_label;
    SysScanDesc scan_desc;
    HeapTuple tuple;
    TupleDesc tupdesc;
    char *result = NULL;

    Oid graphoid = get_graph_oid(graph_name);

    /* scankey for first match in ag_label, column 2, graphoid, BTEQ, OidEQ */
    ScanKeyInit(&scan_keys[0], Anum_ag_label_graph, BTEqualStrategyNumber,
                F_OIDEQ, ObjectIdGetDatum(graphoid));
    /* scankey for second match in ag_label, column 3, label id, BTEQ, Int4EQ */
    ScanKeyInit(&scan_keys[1], Anum_ag_label_id, BTEqualStrategyNumber,
                F_INT4EQ, Int32GetDatum(get_graphid_label_id(graphid)));

    ag_label = heap_open(ag_relation_id("ag_label", "table"), RowExclusiveLock);
    scan_desc = systable_beginscan(ag_label,
                                   ag_relation_id("ag_label_graph_id_index",
                                                  "index"), true, NULL, 2,
                                   scan_keys);

    tuple = systable_getnext(scan_desc);
    if (!HeapTupleIsValid(tuple))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                 errmsg("graphid %lu does not exist", graphid)));
    }

    /* get the tupdesc - we don't need to release this one */
    tupdesc = RelationGetDescr(ag_label);

    /* bail if the number of columns differs */
    if (tupdesc->natts != 5)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("Invalid number of attributes for ag_catalog.ag_label")));

    /* get the label name */
    result = NameStr(*DatumGetName(column_get_datum(tupdesc, tuple, 0, "name",
                                                    NAMEOID, true)));
    /* duplicate it */
    result = strdup(result);

    /* end the scan and close the relation */
    systable_endscan(scan_desc);
    heap_close(ag_label, RowExclusiveLock);

    return result;
}

static Datum get_vertex(const char *graph, const char *vertex_label,
                         int64 graphid)
{
    ScanKeyData scan_keys[1];
    Relation graph_vertex_label;
    HeapScanDesc scan_desc;
    HeapTuple tuple;
    TupleDesc tupdesc;
    Datum id, properties, result;

    /* get the specific graph namespace (schema) */
    Oid graph_namespace_oid = get_namespace_oid(graph, false);
    /* get the specific vertex label table (schema.vertex_label) */
    Oid vertex_label_table_oid = get_relname_relid(vertex_label,
                                                 graph_namespace_oid);
    /* get the active snapshot */
    Snapshot snapshot = GetActiveSnapshot();

    /* initialize the scan key */
    ScanKeyInit(&scan_keys[0], 1, BTEqualStrategyNumber, F_OIDEQ,
                Int64GetDatum(graphid));

    /* open the relation (table), begin the scan, and get the tuple  */
    graph_vertex_label = heap_open(vertex_label_table_oid, RowExclusiveLock);
    scan_desc = heap_beginscan(graph_vertex_label, snapshot, 1, scan_keys);
    tuple = heap_getnext(scan_desc, ForwardScanDirection);

    /* bail if the tuple isn't valid */
    if (!HeapTupleIsValid(tuple))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("graphid %lu does not exist", graphid)));
    }

    /* get the tupdesc - we don't need to release this one */
    tupdesc = RelationGetDescr(graph_vertex_label);
    /* bail if the number of columns differs */
    if (tupdesc->natts != 2)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("Invalid number of attributes for %s.%s", graph,
                        vertex_label )));

    /* get the id */
    id = column_get_datum(tupdesc, tuple, 0, "id", GRAPHIDOID, true);
    /* get the properties */
    properties = column_get_datum(tupdesc, tuple, 1, "properties",
                                  AGTYPEOID, true);
    /* reconstruct the vertex */
    result = DirectFunctionCall3(_agtype_build_vertex, id,
                                 CStringGetDatum(vertex_label), properties);
    /* end the scan and close the relation */
    heap_endscan(scan_desc);
    heap_close(graph_vertex_label, RowExclusiveLock);
    /* return the vertex datum */
    return result;
}

PG_FUNCTION_INFO_V1(startnode);

Datum startnode(PG_FUNCTION_ARGS)
{
    agtype *agt_arg = NULL;
    agtype_value *agtv_object = NULL;
    agtype_value *agtv_value = NULL;
    char *graph_name = NULL;
    char *label_name = NULL;
    graphid graph_id;
    Datum result;

    /* we need the graph name */
    Assert(PG_ARGISNULL(0) == false);

    /* check for null */
    if (PG_ARGISNULL(1))
        PG_RETURN_NULL();

    /* get the graph name */
    agt_arg = AG_GET_ARG_AGTYPE_P(0);
    /* it must be a scalar and must be a string */
    Assert(AGT_ROOT_IS_SCALAR(agt_arg));
    agtv_object = get_ith_agtype_value_from_container(&agt_arg->root, 0);
    Assert(agtv_object->type == AGTV_STRING);
    graph_name = strndup(agtv_object->val.string.val,
                         agtv_object->val.string.len);

    /* get the edge */
    agt_arg = AG_GET_ARG_AGTYPE_P(1);
    /* check for a scalar object */
    if (!AGT_ROOT_IS_SCALAR(agt_arg))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("startNode() argument must resolve to a scalar value")));
    /* get the object */
    agtv_object = get_ith_agtype_value_from_container(&agt_arg->root, 0);

    /* is it an agtype null, return null if it is */
    if (agtv_object->type == AGTV_NULL)
            PG_RETURN_NULL();

    /* check for proper agtype */
    if (agtv_object->type != AGTV_EDGE)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("startNode() argument must be an edge or null")));

    /* get the graphid for start_id */
    agtv_value = get_agtype_value_object_value(agtv_object, "start_id");
    /* it must not be null and must be an integer */
    Assert(agtv_value != NULL);
    Assert(agtv_value->type = AGTV_INTEGER);
    graph_id = agtv_value->val.int_value;

    /* get the label */
    label_name = get_label_name(graph_name, graph_id);
    /* it must not be null and must be a string */
    Assert(label_name != NULL);

    result = get_vertex(graph_name, label_name, graph_id);

    free(label_name);

    return result;
}

PG_FUNCTION_INFO_V1(endnode);

Datum endnode(PG_FUNCTION_ARGS)
{
    agtype *agt_arg = NULL;
    agtype_value *agtv_object = NULL;
    agtype_value *agtv_value = NULL;
    char *graph_name = NULL;
    char *label_name = NULL;
    graphid graph_id;
    Datum result;

    /* we need the graph name */
    Assert(PG_ARGISNULL(0) == false);

    /* check for null */
    if (PG_ARGISNULL(1))
        PG_RETURN_NULL();

    /* get the graph name */
    agt_arg = AG_GET_ARG_AGTYPE_P(0);
    /* it must be a scalar and must be a string */
    Assert(AGT_ROOT_IS_SCALAR(agt_arg));
    agtv_object = get_ith_agtype_value_from_container(&agt_arg->root, 0);
    Assert(agtv_object->type == AGTV_STRING);
    graph_name = strndup(agtv_object->val.string.val,
                         agtv_object->val.string.len);

    /* get the edge */
    agt_arg = AG_GET_ARG_AGTYPE_P(1);
    /* check for a scalar object */
    if (!AGT_ROOT_IS_SCALAR(agt_arg))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("endNode() argument must resolve to a scalar value")));
    /* get the object */
    agtv_object = get_ith_agtype_value_from_container(&agt_arg->root, 0);

    /* is it an agtype null, return null if it is */
    if (agtv_object->type == AGTV_NULL)
            PG_RETURN_NULL();

    /* check for proper agtype */
    if (agtv_object->type != AGTV_EDGE)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("endNode() argument must be an edge or null")));

    /* get the graphid for the end_id */
    agtv_value = get_agtype_value_object_value(agtv_object, "end_id");
    /* it must not be null and must be an integer */
    Assert(agtv_value != NULL);
    Assert(agtv_value->type = AGTV_INTEGER);
    graph_id = agtv_value->val.int_value;

    /* get the label */
    label_name = get_label_name(graph_name, graph_id);
    /* it must not be null and must be a string */
    Assert(label_name != NULL);

    result = get_vertex(graph_name, label_name, graph_id);

    free(label_name);

    return result;
}

PG_FUNCTION_INFO_V1(head);

Datum head(PG_FUNCTION_ARGS)
{
    agtype *agt_arg = NULL;
    agtype_value *agtv_result = NULL;
    int count;

    /* check for null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    agt_arg = AG_GET_ARG_AGTYPE_P(0);
    /* check for an array */
    if (!AGT_ROOT_IS_ARRAY(agt_arg) || AGT_ROOT_IS_SCALAR(agt_arg))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("head() argument must resolve to a list or null")));

    count = AGT_ROOT_COUNT(agt_arg);

    /* if we have an empty list, return a null */
    if (count == 0)
        PG_RETURN_NULL();

    /* get the first element of the array */
    agtv_result = get_ith_agtype_value_from_container(&agt_arg->root, 0);

    /* if it is AGTV_NULL, return null */
    if (agtv_result->type == AGTV_NULL)
        PG_RETURN_NULL();

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_result));
}

PG_FUNCTION_INFO_V1(last);

Datum last(PG_FUNCTION_ARGS)
{
    agtype *agt_arg = NULL;
    agtype_value *agtv_result = NULL;
    int count;

    /* check for null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    agt_arg = AG_GET_ARG_AGTYPE_P(0);
    /* check for an array */
    if (!AGT_ROOT_IS_ARRAY(agt_arg) || AGT_ROOT_IS_SCALAR(agt_arg))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("last() argument must resolve to a list or null")));

    count = AGT_ROOT_COUNT(agt_arg);

    /* if we have an empty list, return null */
    if (count == 0)
        PG_RETURN_NULL();

    /* get the last element of the array */
    agtv_result = get_ith_agtype_value_from_container(&agt_arg->root, count -1);

    /* if it is AGTV_NULL, return null */
    if (agtv_result->type == AGTV_NULL)
        PG_RETURN_NULL();

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_result));
}

PG_FUNCTION_INFO_V1(properties);

Datum properties(PG_FUNCTION_ARGS)
{
    agtype *agt_arg = NULL;
    agtype_value *agtv_object = NULL;
    agtype_value *agtv_result = NULL;

    /* check for null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    agt_arg = AG_GET_ARG_AGTYPE_P(0);
    /* check for a scalar object */
    if (!AGT_ROOT_IS_SCALAR(agt_arg))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("properties() argument must resolve to a scalar value")));

    /* get the object out of the array */
    agtv_object = get_ith_agtype_value_from_container(&agt_arg->root, 0);

    /* is it an agtype null? */
    if (agtv_object->type == AGTV_NULL)
            PG_RETURN_NULL();

    /* check for proper agtype */
    if (agtv_object->type != AGTV_VERTEX && agtv_object->type != AGTV_EDGE)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("properties() argument must be a vertex, an edge or null")));

    agtv_result = get_agtype_value_object_value(agtv_object, "properties");

    Assert(agtv_result != NULL);
    Assert(agtv_result->type = AGTV_OBJECT);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_result));
}

PG_FUNCTION_INFO_V1(length);

Datum length(PG_FUNCTION_ARGS)
{
    agtype *agt_arg = NULL;
    agtype_value *agtv_path = NULL;
    agtype_value agtv_result;

    /* check for null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    agt_arg = AG_GET_ARG_AGTYPE_P(0);
    /* check for a scalar */
    if (!AGT_ROOT_IS_SCALAR(agt_arg))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("length() argument must resolve to a scalar")));

    /* get the path array */
    agtv_path = get_ith_agtype_value_from_container(&agt_arg->root, 0);

    /* if it is AGTV_NULL, return null */
    if (agtv_path->type == AGTV_NULL)
        PG_RETURN_NULL();

    /* check for a path */
    if (agtv_path ->type != AGTV_PATH)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("length() argument must resolve to a path or null")));

    agtv_result.type = AGTV_INTEGER;
    agtv_result.val.int_value = (agtv_path->val.array.num_elems - 1) /2;

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(toboolean);

Datum toboolean(PG_FUNCTION_ARGS)
{
    int nargs;
    Datum *args;
    Datum arg;
    bool *nulls;
    Oid *types;
    Oid type;
    agtype_value agtv_result;
    char *string = NULL;
    bool result = false;

    /* extract argument values */
    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

    /* check number of args */
    if (nargs > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("toBoolean() only supports one argument")));

    /* check for null */
    if (nargs < 0 || nulls[0])
        PG_RETURN_NULL();

    /*
     * toBoolean() supports bool, text, cstring, or the agtype bool, and string
     * input.
     */
    arg = args[0];
    type = types[0];

    if (type != AGTYPEOID)
    {
        if (type == BOOLOID)
            result = DatumGetBool(arg);
        else if (type == CSTRINGOID || type == TEXTOID)
        {
            if (type == CSTRINGOID)
                string = DatumGetCString(arg);
            else
                string = text_to_cstring(DatumGetTextPP(arg));

            if (pg_strcasecmp(string, "true") == 0)
                result = true;
            else if (pg_strcasecmp(string, "false") == 0)
                result = false;
            else
                PG_RETURN_NULL();
        }
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("toBoolean() unsuppoted argument type %d",
                                   type)));
    }
    else
    {
        agtype *agt_arg;
        agtype_value *agtv_value;

        /* get the agtype argument */
        agt_arg = DATUM_GET_AGTYPE_P(arg);

        if (!AGT_ROOT_IS_SCALAR(agt_arg))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("toBoolean() only supports scalar arguments")));

        agtv_value = get_ith_agtype_value_from_container(&agt_arg->root, 0);

        if (agtv_value->type == AGTV_BOOL)
            result = agtv_value->val.boolean;
        else if (agtv_value->type == AGTV_STRING)
        {
            int len = agtv_value->val.string.len;

            string = agtv_value->val.string.val;

            if (len == 4 && pg_strncasecmp(string, "true", len) == 0)
                result = true;
            else if (len == 5 && pg_strncasecmp(string, "false", len) == 0)
                result = false;
            else
                PG_RETURN_NULL();
        }
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("toBoolean() unsuppoted argument agtype %d",
                                   agtv_value->type)));
    }

    /* build the result */
    agtv_result.type = AGTV_BOOL;
    agtv_result.val.boolean = result;

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(tofloat);

Datum tofloat(PG_FUNCTION_ARGS)
{
    int nargs;
    Datum *args;
    Datum arg;
    bool *nulls;
    Oid *types;
    agtype_value agtv_result;
    char *string = NULL;
    bool is_valid = false;
    Oid type;
    float8 result;

    /* extract argument values */
    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

    /* check number of args */
    if (nargs > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("toFloat() only supports one argument")));

    /* check for null */
    if (nargs < 0 || nulls[0])
        PG_RETURN_NULL();

    /*
     * toFloat() supports integer, float, numeric, text, cstring, or the
     * agtype integer, float, numeric, and string input
     */
    arg = args[0];
    type = types[0];

    if (type != AGTYPEOID)
    {
        if (type == INT2OID)
            result = (float8) DatumGetInt16(arg);
        else if (type == INT4OID)
            result = (float8) DatumGetInt32(arg);
        else if (type == INT8OID)
        {
            /*
             * Get the string representation of the integer because it could be
             * too large to fit in a float. Let the float routine determine
             * what to do with it.
             */
            string = DatumGetCString(DirectFunctionCall1(int8out, arg));
            /* turn it into a float */
            result = float8in_internal_null(string, NULL, "double precision",
                                            string, &is_valid);
            /* return null if it was not a invalid float */
            if (!is_valid)
                PG_RETURN_NULL();
        }
        else if (type == FLOAT4OID)
            result = (float8) DatumGetFloat4(arg);
        else if (type == FLOAT8OID)
            result = DatumGetFloat8(arg);
        else if (type == NUMERICOID)
            result = DatumGetFloat8(DirectFunctionCall1(
                numeric_float8_no_overflow, arg));
        else if (type == CSTRINGOID || type == TEXTOID)
        {
            if (type == CSTRINGOID)
                string = DatumGetCString(arg);
            else
                string = text_to_cstring(DatumGetTextPP(arg));

            result = float8in_internal_null(string, NULL, "double precision",
                                            string, &is_valid);
            if (!is_valid)
                PG_RETURN_NULL();
        }
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("toFloat() unsuppoted argument type %d",
                                   type)));
    }
    else
    {
        agtype *agt_arg;
        agtype_value *agtv_value;

        /* get the agtype argument */
        agt_arg = DATUM_GET_AGTYPE_P(arg);

        if (!AGT_ROOT_IS_SCALAR(agt_arg))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("toFloat() only supports scalar arguments")));

        agtv_value = get_ith_agtype_value_from_container(&agt_arg->root, 0);

        if (agtv_value->type == AGTV_INTEGER)
        {
            /* get the string representation of the integer */
            string = DatumGetCString(DirectFunctionCall1(int8out,
                         Int64GetDatum(agtv_value->val.int_value)));
            /* turn it into a float */
            result = float8in_internal_null(string, NULL, "double precision",
                                            string, &is_valid);
            /* return null if it was an invalid float */
            if (!is_valid)
                PG_RETURN_NULL();
        }
        else if (agtv_value->type == AGTV_FLOAT)
            result = agtv_value->val.float_value;
        else if (agtv_value->type == AGTV_NUMERIC)
            result = DatumGetFloat8(DirectFunctionCall1(
                numeric_float8_no_overflow,
                NumericGetDatum(agtv_value->val.numeric)));
        else if (agtv_value->type == AGTV_STRING)
        {
            string = strndup(agtv_value->val.string.val,
                             agtv_value->val.string.len);
            result = float8in_internal_null(string, NULL, "double precision",
                                            string, &is_valid);
            free(string);
            if (!is_valid)
                PG_RETURN_NULL();
        }
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("toFloat() unsuppoted argument agtype %d",
                                   agtv_value->type)));
    }

    /* build the result */
    agtv_result.type = AGTV_FLOAT;
    agtv_result.val.float_value = result;

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(tointeger);

Datum tointeger(PG_FUNCTION_ARGS)
{
    int nargs;
    Datum *args;
    Datum arg;
    bool *nulls;
    Oid *types;
    agtype_value agtv_result;
    char *string = NULL;
    bool is_valid = false;
    Oid type;
    int64 result;

    /* extract argument values */
    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

    /* check number of args */
    if (nargs > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("toInteger() only supports one argument")));

    /* check for null */
    if (nargs < 0 || nulls[0])
        PG_RETURN_NULL();

    /*
     * toInteger() supports integer, float, numeric, text, cstring, or the
     * agtype integer, float, numeric, and string input
     */
    arg = args[0];
    type = types[0];

    if (type != AGTYPEOID)
    {
        if (type == INT2OID)
            result = (int64) DatumGetInt16(arg);
        else if (type == INT4OID)
            result = (int64) DatumGetInt32(arg);
        else if (type == INT8OID)
            result = (int64) DatumGetInt64(arg);
        else if (type == FLOAT4OID)
        {
            float4 f = DatumGetFloat4(arg);

            if (isnan(f) || isinf(f) ||
                f < PG_INT64_MIN || f > PG_INT64_MAX)
                PG_RETURN_NULL();

            result = (int64) f;
        }
        else if (type == FLOAT8OID)
        {
            float8 f = DatumGetFloat8(arg);

            if (isnan(f) || isinf(f) ||
                f < PG_INT64_MIN || f > PG_INT64_MAX)
                PG_RETURN_NULL();

            result = (int64) f;
        }
        else if (type == NUMERICOID)
        {
            float8 f;

            f = DatumGetFloat8(DirectFunctionCall1(
                numeric_float8_no_overflow, arg));

            if (isnan(f) || isinf(f) ||
                f < PG_INT64_MIN || f > PG_INT64_MAX)
                PG_RETURN_NULL();

            result = (int64) f;
        }
        else if (type == CSTRINGOID || type == TEXTOID)
        {
            if (type == CSTRINGOID)
                string = DatumGetCString(arg);
            else
                string = text_to_cstring(DatumGetTextPP(arg));

            /* convert it if it is a regular integer string */
            is_valid = scanint8(string, true, &result);
            /*
             * If it isn't an integer string, try converting it as a float
             * string.
             */
            if (!is_valid)
            {
                float f;

                f = float8in_internal_null(string, NULL, "double precision",
                                           string, &is_valid);
                /*
                 * If the conversions failed or it's a special float value,
                 * return null.
                 */
                if (!is_valid || isnan(f) || isinf(f) ||
                    f < PG_INT64_MIN || f > PG_INT64_MAX)
                    PG_RETURN_NULL();

                result = (int64) f;
            }
        }
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("toInteger() unsuppoted argument type %d",
                                   type)));
    }
    else
    {
        agtype *agt_arg;
        agtype_value *agtv_value;

        /* get the agtype argument */
        agt_arg = DATUM_GET_AGTYPE_P(arg);

        if (!AGT_ROOT_IS_SCALAR(agt_arg))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("toInteger() only supports scalar arguments")));

        agtv_value = get_ith_agtype_value_from_container(&agt_arg->root, 0);

        if (agtv_value->type == AGTV_INTEGER)
            result = agtv_value->val.int_value;
        else if (agtv_value->type == AGTV_FLOAT)
        {
            float f = agtv_value->val.float_value;

            if (isnan(f) || isinf(f) ||
                f < PG_INT64_MIN || f > PG_INT64_MAX)
                PG_RETURN_NULL();

            result = (int64) f;
        }
        else if (agtv_value->type == AGTV_NUMERIC)
        {
            float8 f;
            Datum num = NumericGetDatum(agtv_value->val.numeric);

            f = DatumGetFloat8(DirectFunctionCall1(
                numeric_float8_no_overflow, num));

            if (isnan(f) || isinf(f) ||
                f < PG_INT64_MIN || f > PG_INT64_MAX)
                PG_RETURN_NULL();

            result = (int64) f;
        }
        else if (agtv_value->type == AGTV_STRING)
        {
            /* we need a null terminated cstring */
            string = strndup(agtv_value->val.string.val,
                             agtv_value->val.string.len);
            /* convert it if it is a regular integer string */
            is_valid = scanint8(string, true, &result);
            /*
             * If it isn't an integer string, try converting it as a float
             * string.
             */
            if (!is_valid)
            {
                float f;

                f = float8in_internal_null(string, NULL, "double precision",
                                           string, &is_valid);
                free(string);
                /*
                 * If the conversions failed or it's a special float value,
                 * return null.
                 */
                if (!is_valid || isnan(f) || isinf(f) ||
                    f < PG_INT64_MIN || f > PG_INT64_MAX)
                    PG_RETURN_NULL();

                result = (int64) f;
            }
            else
                free(string);
        }
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("toInteger() unsuppoted argument agtype %d",
                                   agtv_value->type)));
    }

    /* build the result */
    agtv_result.type = AGTV_INTEGER;
    agtv_result.val.int_value = result;

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(size);

Datum size(PG_FUNCTION_ARGS)
{
    int nargs;
    Datum *args;
    Datum arg;
    bool *nulls;
    Oid *types;
    agtype_value agtv_result;
    char *string = NULL;
    Oid type;
    int64 result;

    /* extract argument values */
    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

    /* check number of args */
    if (nargs > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("size() only supports one argument")));

    /* check for null */
    if (nargs < 0 || nulls[0])
        PG_RETURN_NULL();

    /*
     * size() supports cstring, text, or the agtype string or list input
     */
    arg = args[0];
    type = types[0];

    if (type == CSTRINGOID)
    {
        string = DatumGetCString(arg);
        result = strlen(string);
    }
    else if (type == TEXTOID)
    {
        string = text_to_cstring(DatumGetTextPP(arg));
        result = strlen(string);
    }
    else if (type == AGTYPEOID)
    {
        agtype *agt_arg;

        /* get the agtype argument */
        agt_arg = DATUM_GET_AGTYPE_P(arg);

        if (AGT_ROOT_IS_SCALAR(agt_arg))
        {
            agtype_value *agtv_value;

            agtv_value = get_ith_agtype_value_from_container(&agt_arg->root, 0);

            if (agtv_value->type == AGTV_STRING)
                result = agtv_value->val.string.len;
            else
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                        errmsg("size() unsupported argument")));
        }
        else if (AGT_ROOT_IS_ARRAY(agt_arg))
            result = AGT_ROOT_COUNT(agt_arg);
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("size() unsupported argument")));
    }
    else
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("size() unsupported argument")));

    /* build the result */
    agtv_result.type = AGTV_INTEGER;
    agtv_result.val.int_value = result;

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(graphid_to_agtype);

Datum graphid_to_agtype(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(integer_to_agtype(AG_GETARG_GRAPHID(0)));
}

PG_FUNCTION_INFO_V1(type);

Datum type(PG_FUNCTION_ARGS)
{
    agtype *agt_arg = NULL;
    agtype_value *agtv_object = NULL;
    agtype_value *agtv_result = NULL;

    /* check for null */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    agt_arg = AG_GET_ARG_AGTYPE_P(0);
    /* check for a scalar object */
    if (!AGT_ROOT_IS_SCALAR(agt_arg))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("type() argument must resolve to a scalar value")));

    /* get the object out of the array */
    agtv_object = get_ith_agtype_value_from_container(&agt_arg->root, 0);

    /* is it an agtype null? */
    if (agtv_object->type == AGTV_NULL)
            PG_RETURN_NULL();

    /* check for proper agtype */
    if (agtv_object->type != AGTV_EDGE)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("type() argument must be an edge or null")));

    agtv_result = get_agtype_value_object_value(agtv_object, "label");

    Assert(agtv_result != NULL);
    Assert(agtv_result->type = AGTV_STRING);

    PG_RETURN_POINTER(agtype_value_to_agtype(agtv_result));
}

PG_FUNCTION_INFO_V1(exists_property);
/*
 * Executor function for EXISTS(property).
 *
 * Note: For most executor functions we want to return SQL NULL for NULL input.
 *       However, in this case, NULL means false - it was not found.
 */
Datum exists_property(PG_FUNCTION_ARGS)
{
    agtype *agt_arg = NULL;
    agtype_value *agtv_value = NULL;

    /* check for NULL, NULL is FALSE */
    if (PG_ARGISNULL(0))
        PG_RETURN_BOOL(false);

    /* get the argument */
    agt_arg = AG_GET_ARG_AGTYPE_P(0);

    /* check for a scalar AGTV_NULL */
    if (AGT_ROOT_IS_SCALAR(agt_arg))
    {
        agtv_value = get_ith_agtype_value_from_container(&agt_arg->root, 0);

        /* again, if NULL, NULL is FALSE */
        if (agtv_value->type == AGTV_NULL)
            PG_RETURN_BOOL(false);
    }

    /* otherwise, we have something, and something is TRUE */
    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(tostring);

Datum tostring(PG_FUNCTION_ARGS)
{
    int nargs;
    Datum *args;
    Datum arg;
    bool *nulls;
    Oid *types;
    agtype_value agtv_result;
    char *string = NULL;
    Oid type;

    /* extract argument values */
    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

    /* check number of args */
    if (nargs > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("toString() only supports one argument")));
    /* check for null */
    if (nargs < 0 || nulls[0])
        PG_RETURN_NULL();

    /*
     * toString() supports integer, float, numeric, text, cstring, boolean or
     * the agtype integer, float, numeric, string, boolean input
     */
    arg = args[0];
    type = types[0];

    if (type != AGTYPEOID)
    {
        if (type == INT2OID)
            string = DatumGetCString(DirectFunctionCall1(int8out,
                Int64GetDatum((int64) DatumGetInt16(arg))));
        else if (type == INT4OID)
            string = DatumGetCString(DirectFunctionCall1(int8out,
                Int64GetDatum((int64) DatumGetInt32(arg))));
        else if (type == INT8OID)
            string = DatumGetCString(DirectFunctionCall1(int8out, arg));
        else if (type == FLOAT4OID)
            string = DatumGetCString(DirectFunctionCall1(float8out, arg));
        else if (type == FLOAT8OID)
            string = DatumGetCString(DirectFunctionCall1(float8out, arg));
        else if (type == NUMERICOID)
            string = DatumGetCString(DirectFunctionCall1(numeric_out, arg));
        else if (type == CSTRINGOID)
            string = DatumGetCString(arg);
        else if (type == TEXTOID)
            string = text_to_cstring(DatumGetTextPP(arg));
        else if (type == BOOLOID)
            string = DatumGetBool(arg) ? "true" : "false";
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("toString() unsuppoted argument type %d",
                                   type)));
    }
    else
    {
        agtype *agt_arg;
        agtype_value *agtv_value;

        /* get the agtype argument */
        agt_arg = DATUM_GET_AGTYPE_P(arg);

        if (!AGT_ROOT_IS_SCALAR(agt_arg))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("toString() only supports scalar arguments")));

        agtv_value = get_ith_agtype_value_from_container(&agt_arg->root, 0);

        if (agtv_value->type == AGTV_NULL)
            PG_RETURN_NULL();
        else if (agtv_value->type == AGTV_INTEGER)
            string = DatumGetCString(DirectFunctionCall1(int8out,
                Int64GetDatum(agtv_value->val.int_value)));
        else if (agtv_value->type == AGTV_FLOAT)
            string = DatumGetCString(DirectFunctionCall1(float8out,
                Float8GetDatum(agtv_value->val.float_value)));
        else if (agtv_value->type == AGTV_STRING)
            string = pnstrdup(agtv_value->val.string.val,
                              agtv_value->val.string.len);
        else if (agtv_value->type == AGTV_NUMERIC)
            string = DatumGetCString(DirectFunctionCall1(numeric_out,
                PointerGetDatum(agtv_value->val.numeric)));
        else if (agtv_value->type == AGTV_BOOL)
            string = (agtv_value->val.boolean) ? "true" : "false";
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("toString() unsuppoted argument agtype %d",
                                   agtv_value->type)));
    }

    /* build the result */
    agtv_result.type = AGTV_STRING;
    agtv_result.val.string.val = string;
    agtv_result.val.string.len = strlen(string);

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(reverse);

Datum reverse(PG_FUNCTION_ARGS)
{
    int nargs;
    Datum *args;
    Datum arg;
    bool *nulls;
    Oid *types;
    agtype_value agtv_result;
    char *string = NULL;
    char *result = NULL;
    int string_len;
    Oid type;
    int i;

    /* extract argument values */
    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

    /* check number of args */
    if (nargs > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("reverse() only supports one argument")));

    /* check for null */
    if (nargs < 0 || nulls[0])
        PG_RETURN_NULL();

    /* reverse() supports text, cstring, or the agtype string input */
    arg = args[0];
    type = types[0];

    if (type != AGTYPEOID)
    {
        if (type == CSTRINGOID)
            string = DatumGetCString(arg);
        else if (type == TEXTOID)
            string = text_to_cstring(DatumGetTextPP(arg));
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("reverse() unsuppoted argument type %d",
                                   type)));
        string_len = strlen(string);
    }
    else
    {
        agtype *agt_arg;
        agtype_value *agtv_value;

        /* get the agtype argument */
        agt_arg = DATUM_GET_AGTYPE_P(arg);

        if (!AGT_ROOT_IS_SCALAR(agt_arg))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("reverse() only supports scalar arguments")));

        agtv_value = get_ith_agtype_value_from_container(&agt_arg->root, 0);

        /* check for agtype null */
        if (agtv_value->type == AGTV_NULL)
            PG_RETURN_NULL();
        if (agtv_value->type == AGTV_STRING)
        {
            string = agtv_value->val.string.val;
            string_len = agtv_value->val.string.len;
        }
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("reverse() unsuppoted argument agtype %d",
                                   agtv_value->type)));
    }

    /* if we have an empty string, return null */
    if (string_len == 0)
        PG_RETURN_NULL();

    /* allocate the new string */
    result = palloc(string_len);

    /* reverse the string */
    for (i = 0; i < string_len; i++)
        result[i] = string[string_len - i - 1];

    /* build the result */
    agtv_result.type = AGTV_STRING;
    agtv_result.val.string.val = result;
    agtv_result.val.string.len = string_len;

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(touppercase);

Datum touppercase(PG_FUNCTION_ARGS)
{
    int nargs;
    Datum *args;
    Datum arg;
    bool *nulls;
    Oid *types;
    agtype_value agtv_result;
    char *string = NULL;
    char *result = NULL;
    int string_len;
    Oid type;
    int i;

    /* extract argument values */
    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

    /* check number of args */
    if (nargs > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("touppercase() only supports one argument")));

    /* check for null */
    if (nargs < 0 || nulls[0])
        PG_RETURN_NULL();

    /* touppercase() supports text, cstring, or the agtype string input */
    arg = args[0];
    type = types[0];
    if (type != AGTYPEOID)
    {
        if (type == CSTRINGOID)
            string = DatumGetCString(arg);
        else if (type == TEXTOID)
            string = text_to_cstring(DatumGetTextPP(arg));
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("touppercase() unsuppoted argument type %d",
                                   type)));
        string_len = strlen(string);
    }
    else
    {
        agtype *agt_arg;
        agtype_value *agtv_value;

        /* get the agtype argument */
        agt_arg = DATUM_GET_AGTYPE_P(arg);

        if (!AGT_ROOT_IS_SCALAR(agt_arg))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("touppercase() only supports scalar arguments")));

        agtv_value = get_ith_agtype_value_from_container(&agt_arg->root, 0);

        /* check for agtype null */
        if (agtv_value->type == AGTV_NULL)
            PG_RETURN_NULL();
        if (agtv_value->type == AGTV_STRING)
        {
            string = agtv_value->val.string.val;
            string_len = agtv_value->val.string.len;
        }
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("touppercase() unsuppoted argument agtype %d",
                                   agtv_value->type)));
    }

    /* if we have an empty string, return null */
    if (string_len == 0)
        PG_RETURN_NULL();

    /* allocate the new string */
    result = palloc(string_len);

    /* upcase the string */
    for (i = 0; i < string_len; i++)
        result[i] = pg_toupper(string[i]);

    /* build the result */
    agtv_result.type = AGTV_STRING;
    agtv_result.val.string.val = result;
    agtv_result.val.string.len = string_len;

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(tolowercase);

Datum tolowercase(PG_FUNCTION_ARGS)
{
    int nargs;
    Datum *args;
    Datum arg;
    bool *nulls;
    Oid *types;
    agtype_value agtv_result;
    char *string = NULL;
    char *result = NULL;
    int string_len;
    Oid type;
    int i;

    /* extract argument values */
    nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

    /* check number of args */
    if (nargs > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("tolowercase() only supports one argument")));

    /* check for null */
    if (nargs < 0 || nulls[0])
        PG_RETURN_NULL();

    /* tolowercase() supports text, cstring, or the agtype string input */
    arg = args[0];
    type = types[0];
    if (type != AGTYPEOID)
    {
        if (type == CSTRINGOID)
            string = DatumGetCString(arg);
        else if (type == TEXTOID)
            string = text_to_cstring(DatumGetTextPP(arg));
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("tolowercase() unsuppoted argument type %d",
                                   type)));
        string_len = strlen(string);
    }
    else
    {
        agtype *agt_arg;
        agtype_value *agtv_value;

        /* get the agtype argument */
        agt_arg = DATUM_GET_AGTYPE_P(arg);

        if (!AGT_ROOT_IS_SCALAR(agt_arg))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("tolowercase() only supports scalar arguments")));

        agtv_value = get_ith_agtype_value_from_container(&agt_arg->root, 0);

        /* check for agtype null */
        if (agtv_value->type == AGTV_NULL)
            PG_RETURN_NULL();
        if (agtv_value->type == AGTV_STRING)
        {
            string = agtv_value->val.string.val;
            string_len = agtv_value->val.string.len;
        }
        else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("tolowercase() unsuppoted argument agtype %d",
                                   agtv_value->type)));
    }

    /* if we have an empty string, return null */
    if (string_len == 0)
        PG_RETURN_NULL();

    /* allocate the new string */
    result = palloc(string_len);

    /* downcase the string */
    for (i = 0; i < string_len; i++)
        result[i] = pg_tolower(string[i]);

    /* build the result */
    agtv_result.type = AGTV_STRING;
    agtv_result.val.string.val = result;
    agtv_result.val.string.len = string_len;

    PG_RETURN_POINTER(agtype_value_to_agtype(&agtv_result));
}
