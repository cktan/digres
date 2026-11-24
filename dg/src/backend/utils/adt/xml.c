/*-------------------------------------------------------------------------
 *
 * xml.c
 *	  XML data type support.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 * src/backend/utils/adt/xml.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * digres has no XML support: it is built without libxml, and the code that
 * used libxml is gone. The type and its functions stay, so a catalog and a
 * dump that name them still work; xml_out() works, and most of the rest
 * fails with the error of a server built without libxml.
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "executor/tablefunc.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/xml.h"
#include "utils/control_block.h"


static void xmldata_root_element_start(StringInfo result, const char *eltname,
                                       const char *xmlschema,
                                       const char *targetns, bool top_level);
static void xmldata_root_element_end(StringInfo result, const char *eltname);
static StringInfo query_to_xml_internal(const char *query, char *tablename,
                                        const char *xmlschema, bool nulls,
                                        bool tableforest, const char *targetns,
                                        bool top_level);
static const char *map_sql_table_to_xmlschema(TupleDesc tupdesc, Oid relid,
                                              bool nulls, bool tableforest,
                                              const char *targetns);
static const char *
map_sql_schema_to_xmlschema_types(Oid nspid, List *relid_list, bool nulls,
                                  bool tableforest, const char *targetns);
static const char *map_sql_catalog_to_xmlschema_types(List *nspid_list,
                                                      bool nulls,
                                                      bool tableforest,
                                                      const char *targetns);
static const char *map_sql_type_to_xml_name(Oid typeoid, int typmod);
static const char *map_sql_typecoll_to_xmlschema_types(List *tupdesc_list);
static const char *map_sql_type_to_xmlschema_type(Oid typeoid, int typmod);
static void SPI_sql_row_to_xmlelement(uint64 rownum, StringInfo result,
                                      char *tablename, bool nulls,
                                      bool tableforest, const char *targetns,
                                      bool top_level);

/* XMLTABLE support */

static void XmlTableInitOpaque(struct TableFuncScanState *state, int natts);
static void XmlTableSetDocument(struct TableFuncScanState *state, Datum value);
static void XmlTableSetNamespace(struct TableFuncScanState *state,
                                 const char *name, const char *uri);
static void XmlTableSetRowFilter(struct TableFuncScanState *state,
                                 const char *path);
static void XmlTableSetColumnFilter(struct TableFuncScanState *state,
                                    const char *path, int colnum);
static bool XmlTableFetchRow(struct TableFuncScanState *state);
static Datum XmlTableGetValue(struct TableFuncScanState *state, int colnum,
                              Oid typid, int32 typmod, bool *isnull);
static void XmlTableDestroyOpaque(struct TableFuncScanState *state);

const TableFuncRoutine XmlTableRoutine = {
    .InitOpaque = XmlTableInitOpaque,
    .SetDocument = XmlTableSetDocument,
    .SetNamespace = XmlTableSetNamespace,
    .SetRowFilter = XmlTableSetRowFilter,
    .SetColumnFilter = XmlTableSetColumnFilter,
    .FetchRow = XmlTableFetchRow,
    .GetValue = XmlTableGetValue,
    .DestroyOpaque = XmlTableDestroyOpaque};

#define NO_XML_SUPPORT() \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("unsupported XML feature"), \
			 errdetail("This functionality requires the server to be built with libxml support.")))

/* from SQL/XML:2008 section 4.9 */
#define NAMESPACE_XSD "http://www.w3.org/2001/XMLSchema"
#define NAMESPACE_XSI "http://www.w3.org/2001/XMLSchema-instance"
#define NAMESPACE_SQLXML "http://standards.iso.org/iso/9075/2003/sqlxml"


/*
 * xml_in uses a plain C string to VARDATA conversion, so for the time being
 * we use the conversion function for the text datatype.
 *
 * This is only acceptable so long as xmltype and text use the same
 * representation.
 */
Datum xml_in(PG_FUNCTION_ARGS) {
  NO_XML_SUPPORT();
  return 0;
}

#define PG_XML_DEFAULT_VERSION "1.0"

/*
 * xml_out_internal uses a plain VARDATA to C string conversion, so for the
 * time being we use the conversion function for the text datatype.
 *
 * This is only acceptable so long as xmltype and text use the same
 * representation.
 */
static char *xml_out_internal(xmltype *x, pg_enc target_encoding) {
  char *str = text_to_cstring((text *)x);

  return str;
}

Datum xml_out(PG_FUNCTION_ARGS) {
  xmltype *x = PG_GETARG_XML_P(0);

  /*
   * xml_out removes the encoding property in all cases.  This is because we
   * cannot control from here whether the datum will be converted to a
   * different client encoding, so we'd do more harm than good by including
   * it.
   */
  PG_RETURN_CSTRING(xml_out_internal(x, (pg_enc)0));
}

Datum xml_recv(PG_FUNCTION_ARGS) {
  NO_XML_SUPPORT();
  return 0;
}

Datum xml_send(PG_FUNCTION_ARGS) {
  xmltype *x = PG_GETARG_XML_P(0);
  char *outval;
  StringInfoData buf;

  /*
   * xml_out_internal doesn't convert the encoding, it just prints the right
   * declaration. pq_sendtext will do the conversion.
   */
  outval = xml_out_internal(x, (pg_enc)pg_get_client_encoding());

  pq_begintypsend(&buf);
  pq_sendtext(&buf, outval, strlen(outval));
  pfree(outval);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


static xmltype *stringinfo_to_xmltype(StringInfo buf) {
  return (xmltype *)cstring_to_text_with_len(buf->data, buf->len);
}

static xmltype *cstring_to_xmltype(const char *string) {
  return (xmltype *)cstring_to_text(string);
}


Datum xmlcomment(PG_FUNCTION_ARGS) {
  NO_XML_SUPPORT();
  return 0;
}

Datum xmltext(PG_FUNCTION_ARGS) {
  NO_XML_SUPPORT();
  return 0;
}

/*
 * TODO: xmlconcat needs to merge the notations and unparsed entities
 * of the argument values.  Not very important in practice, though.
 */
xmltype *xmlconcat(List *args) {
  NO_XML_SUPPORT();
  return NULL;
}

/*
 * XMLAGG support
 */
Datum xmlconcat2(PG_FUNCTION_ARGS) {
  if (PG_ARGISNULL(0)) {
    if (PG_ARGISNULL(1))
      PG_RETURN_NULL();
    else
      PG_RETURN_XML_P(PG_GETARG_XML_P(1));
  } else if (PG_ARGISNULL(1))
    PG_RETURN_XML_P(PG_GETARG_XML_P(0));
  else
    PG_RETURN_XML_P(
        xmlconcat(list_make2(PG_GETARG_XML_P(0), PG_GETARG_XML_P(1))));
}

Datum texttoxml(PG_FUNCTION_ARGS) {
  text *data = PG_GETARG_TEXT_PP(0);

  PG_RETURN_XML_P(xmlparse(data, XMLOPTION_CONTENT, true));
}

Datum xmltotext(PG_FUNCTION_ARGS) {
  xmltype *data = PG_GETARG_XML_P(0);

  /* It's actually binary compatible. */
  PG_RETURN_TEXT_P((text *)data);
}

text *xmltotext_with_options(xmltype *data, XmlOptionType xmloption_arg,
                             bool indent) {

  if (xmloption_arg != XMLOPTION_DOCUMENT && !indent) {
    /*
     * We don't actually need to do anything, so just return the
     * binary-compatible input.  For backwards-compatibility reasons,
     * allow such cases to succeed even without USE_LIBXML.
     */
    return (text *)data;
  }

  NO_XML_SUPPORT();
  return NULL;
}

xmltype *xmlelement(XmlExpr *xexpr, Datum *named_argvalue, bool *named_argnull,
                    Datum *argvalue, bool *argnull) {
  NO_XML_SUPPORT();
  return NULL;
}

xmltype *xmlparse(text *data, XmlOptionType xmloption_arg,
                  bool preserve_whitespace) {
  NO_XML_SUPPORT();
  return NULL;
}

xmltype *xmlpi(const char *target, text *arg, bool arg_is_null,
               bool *result_is_null) {
  NO_XML_SUPPORT();
  return NULL;
}

xmltype *xmlroot(xmltype *data, text *version, int standalone) {
  NO_XML_SUPPORT();
  return NULL;
}

/*
 * Validate document (given as string) against DTD (given as external link)
 *
 * This has been removed because it is a security hole: unprivileged users
 * should not be able to use Postgres to fetch arbitrary external files,
 * which unfortunately is exactly what libxml is willing to do with the DTD
 * parameter.
 */
Datum xmlvalidate(PG_FUNCTION_ARGS) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("xmlvalidate is not implemented")));
  return 0;
}

bool xml_is_document(xmltype *arg) {
  NO_XML_SUPPORT();
  return false;
}


/*
 * Map SQL identifier to XML name; see SQL/XML:2008 section 9.1.
 */
char *map_sql_identifier_to_xml_name(const char *ident, bool fully_escaped,
                                     bool escape_period) {
  NO_XML_SUPPORT();
  return NULL;
}

/*
 * Map XML name to SQL identifier; see SQL/XML:2008 section 9.3.
 */
char *map_xml_name_to_sql_identifier(const char *name) {
  StringInfoData buf;
  const char *p;

  initStringInfo(&buf);

  for (p = name; *p; p += pg_mblen_cstr(p)) {
    if (*p == '_' && *(p + 1) == 'x' && isxdigit((unsigned char)*(p + 2)) &&
        isxdigit((unsigned char)*(p + 3)) &&
        isxdigit((unsigned char)*(p + 4)) &&
        isxdigit((unsigned char)*(p + 5)) && *(p + 6) == '_') {
      char cbuf[MAX_UNICODE_EQUIVALENT_STRING + 1];
      unsigned int u;

      sscanf(p + 2, "%X", &u);
      pg_unicode_to_server(u, (unsigned char *)cbuf);
      appendStringInfoString(&buf, cbuf);
      p += 6;
    } else
      appendBinaryStringInfo(&buf, p, pg_mblen_cstr(p));
  }

  return buf.data;
}

/*
 * Map SQL value to XML value; see SQL/XML:2008 section 9.8.
 *
 * When xml_escape_strings is true, then certain characters in string
 * values are replaced by entity references (&lt; etc.), as specified
 * in SQL/XML:2008 section 9.8 GR 9) a) iii).   This is normally what is
 * wanted.  The false case is mainly useful when the resulting value
 * is used with xmlTextWriterWriteAttribute() to write out an
 * attribute, because that function does the escaping itself.
 */
char *map_sql_value_to_xml_value(Datum value, Oid type,
                                 bool xml_escape_strings) {
  if (type_is_array_domain(type)) {
    ArrayType *array;
    Oid elmtype;
    int16 elmlen;
    bool elmbyval;
    char elmalign;
    int num_elems;
    Datum *elem_values;
    bool *elem_nulls;
    StringInfoData buf;
    int i;

    array = DatumGetArrayTypeP(value);
    elmtype = ARR_ELEMTYPE(array);
    get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);

    deconstruct_array(array, elmtype, elmlen, elmbyval, elmalign, &elem_values,
                      &elem_nulls, &num_elems);

    initStringInfo(&buf);

    for (i = 0; i < num_elems; i++) {
      if (elem_nulls[i])
        continue;
      appendStringInfoString(&buf, "<element>");
      appendStringInfoString(
          &buf, map_sql_value_to_xml_value(elem_values[i], elmtype, true));
      appendStringInfoString(&buf, "</element>");
    }

    pfree(elem_values);
    pfree(elem_nulls);

    return buf.data;
  } else {
    Oid typeOut;
    bool isvarlena;
    char *str;

    /*
     * Flatten domains; the special-case treatments below should apply to,
     * eg, domains over boolean not just boolean.
     */
    type = getBaseType(type);

    /*
     * Special XSD formatting for some data types
     */
    switch (type) {
    case BOOLOID:
      if (DatumGetBool(value))
        return "true";
      else
        return "false";

    case DATEOID: {
      DateADT date;
      struct pg_tm tm;
      char buf[MAXDATELEN + 1];

      date = DatumGetDateADT(value);
      /* XSD doesn't support infinite values */
      if (DATE_NOT_FINITE(date))
        ereport(ERROR,
                (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                 errmsg("date out of range"),
                 errdetail("XML does not support infinite date values.")));
      j2date(date + POSTGRES_EPOCH_JDATE, &(tm.tm_year), &(tm.tm_mon),
             &(tm.tm_mday));
      EncodeDateOnly(&tm, USE_XSD_DATES, buf);

      return pstrdup(buf);
    }

    case TIMESTAMPOID: {
      Timestamp timestamp;
      struct pg_tm tm;
      fsec_t fsec;
      char buf[MAXDATELEN + 1];

      timestamp = DatumGetTimestamp(value);

      /* XSD doesn't support infinite values */
      if (TIMESTAMP_NOT_FINITE(timestamp))
        ereport(ERROR,
                (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                 errmsg("timestamp out of range"),
                 errdetail("XML does not support infinite timestamp values.")));
      else if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, NULL) == 0)
        EncodeDateTime(&tm, fsec, false, 0, NULL, USE_XSD_DATES, buf);
      else
        ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                        errmsg("timestamp out of range")));

      return pstrdup(buf);
    }

    case TIMESTAMPTZOID: {
      TimestampTz timestamp;
      struct pg_tm tm;
      int tz;
      fsec_t fsec;
      const char *tzn = NULL;
      char buf[MAXDATELEN + 1];

      timestamp = DatumGetTimestamp(value);

      /* XSD doesn't support infinite values */
      if (TIMESTAMP_NOT_FINITE(timestamp))
        ereport(ERROR,
                (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                 errmsg("timestamp out of range"),
                 errdetail("XML does not support infinite timestamp values.")));
      else if (timestamp2tm(timestamp, &tz, &tm, &fsec, &tzn, NULL) == 0)
        EncodeDateTime(&tm, fsec, true, tz, tzn, USE_XSD_DATES, buf);
      else
        ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                        errmsg("timestamp out of range")));

      return pstrdup(buf);
    }

    }

    /*
     * otherwise, just use the type's native text representation
     */
    getTypeOutputInfo(type, &typeOut, &isvarlena);
    str = OidOutputFunctionCall(typeOut, value);

    /* ... exactly as-is for XML, and when escaping is not wanted */
    if (type == XMLOID || !xml_escape_strings)
      return str;

    /* otherwise, translate special characters as needed */
    return escape_xml(str);
  }
}

/*
 * Escape characters in text that have special meanings in XML.
 *
 * Returns a palloc'd string.
 *
 * NB: this is intentionally not dependent on libxml.
 */
char *escape_xml(const char *str) {
  StringInfoData buf;
  const char *p;

  initStringInfo(&buf);
  for (p = str; *p; p++) {
    switch (*p) {
    case '&':
      appendStringInfoString(&buf, "&amp;");
      break;
    case '<':
      appendStringInfoString(&buf, "&lt;");
      break;
    case '>':
      appendStringInfoString(&buf, "&gt;");
      break;
    case '\r':
      appendStringInfoString(&buf, "&#x0d;");
      break;
    default:
      appendStringInfoCharMacro(&buf, *p);
      break;
    }
  }
  return buf.data;
}

static char *_SPI_strdup(const char *s) {
  size_t len = strlen(s) + 1;
  char *ret = (char *)SPI_palloc(len);

  memcpy(ret, s, len);
  return ret;
}

/*
 * SQL to XML mapping functions
 *
 * What follows below was at one point intentionally organized so that
 * you can read along in the SQL/XML standard. The functions are
 * mostly split up the way the clauses lay out in the standards
 * document, and the identifiers are also aligned with the standard
 * text.  Unfortunately, SQL/XML:2006 reordered the clauses
 * differently than SQL/XML:2003, so the order below doesn't make much
 * sense anymore.
 *
 * There are many things going on there:
 *
 * There are two kinds of mappings: Mapping SQL data (table contents)
 * to XML documents, and mapping SQL structure (the "schema") to XML
 * Schema.  And there are functions that do both at the same time.
 *
 * Then you can map a database, a schema, or a table, each in both
 * ways.  This breaks down recursively: Mapping a database invokes
 * mapping schemas, which invokes mapping tables, which invokes
 * mapping rows, which invokes mapping columns, although you can't
 * call the last two from the outside.  Because of this, there are a
 * number of xyz_internal() functions which are to be called both from
 * the function manager wrapper and from some upper layer in a
 * recursive call.
 *
 * See the documentation about what the common function arguments
 * nulls, tableforest, and targetns mean.
 *
 * Some style guidelines for XML output: Use double quotes for quoting
 * XML attributes.  Indent XML elements by two spaces, but remember
 * that a lot of code is called recursively at different levels, so
 * it's better not to indent rather than create output that indents
 * and outdents weirdly.  Add newlines to make the output look nice.
 */

/*
 * Visibility of objects for XML mappings; see SQL/XML:2008 section
 * 4.10.8.
 */

/*
 * Given a query, which must return type oid as first column, produce
 * a list of Oids with the query results.
 */
static List *query_to_oid_list(const char *query) {
  uint64 i;
  List *list = NIL;
  int spi_result;

  spi_result = SPI_execute(query, true, 0);
  if (spi_result != SPI_OK_SELECT)
    elog(ERROR, "SPI_execute returned %s for %s",
         SPI_result_code_string(spi_result), query);

  for (i = 0; i < SPI_processed; i++) {
    Datum oid;
    bool isnull;

    oid =
        SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull)
      list = lappend_oid(list, DatumGetObjectId(oid));
  }

  return list;
}

static List *schema_get_xml_visible_tables(Oid nspid) {
  StringInfoData query;

  initStringInfo(&query);
  appendStringInfo(
      &query,
      "SELECT oid FROM pg_catalog.pg_class"
      " WHERE relnamespace = %u AND relkind IN (" CppAsString2(RELKIND_RELATION) "," CppAsString2(
          RELKIND_MATVIEW) "," CppAsString2(RELKIND_VIEW) ")"
                                                          " AND "
                                                          "pg_catalog.has_"
                                                          "table_privilege "
                                                          "(oid, 'SELECT')"
                                                          " ORDER BY relname;",
      nspid);

  return query_to_oid_list(query.data);
}

/*
 * Including the system schemas is probably not useful for a database
 * mapping.
 */
#define XML_VISIBLE_SCHEMAS_EXCLUDE "(nspname ~ '^pg_' OR nspname = 'information_schema')"

#define XML_VISIBLE_SCHEMAS "SELECT oid FROM pg_catalog.pg_namespace WHERE pg_catalog.has_schema_privilege (oid, 'USAGE') AND NOT " XML_VISIBLE_SCHEMAS_EXCLUDE

static List *database_get_xml_visible_schemas(void) {
  return query_to_oid_list(XML_VISIBLE_SCHEMAS " ORDER BY nspname;");
}

static List *database_get_xml_visible_tables(void) {
  /* At the moment there is no order required here. */
  return query_to_oid_list(
      "SELECT oid FROM pg_catalog.pg_class"
      " WHERE relkind IN (" CppAsString2(RELKIND_RELATION) "," CppAsString2(
          RELKIND_MATVIEW) "," CppAsString2(RELKIND_VIEW) ")"
                                                          " AND "
                                                          "pg_catalog.has_"
                                                          "table_privilege(pg_"
                                                          "class.oid, 'SELECT')"
                                                          " AND relnamespace "
                                                          "IN "
                                                          "(" XML_VISIBLE_SCHEMAS
                                                          ");");
}

/*
 * Map SQL table to XML and/or XML Schema document; see SQL/XML:2008
 * section 9.11.
 */

static StringInfo table_to_xml_internal(Oid relid, const char *xmlschema,
                                        bool nulls, bool tableforest,
                                        const char *targetns, bool top_level) {
  StringInfoData query;

  initStringInfo(&query);
  appendStringInfo(&query, "SELECT * FROM %s",
                   DatumGetCString(DirectFunctionCall1(
                       regclassout, ObjectIdGetDatum(relid))));
  return query_to_xml_internal(query.data, get_rel_name(relid), xmlschema,
                               nulls, tableforest, targetns, top_level);
}

Datum table_to_xml(PG_FUNCTION_ARGS) {
  Oid relid = PG_GETARG_OID(0);
  bool nulls = PG_GETARG_BOOL(1);
  bool tableforest = PG_GETARG_BOOL(2);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

  PG_RETURN_XML_P(stringinfo_to_xmltype(
      table_to_xml_internal(relid, NULL, nulls, tableforest, targetns, true)));
}

Datum query_to_xml(PG_FUNCTION_ARGS) {
  char *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
  bool nulls = PG_GETARG_BOOL(1);
  bool tableforest = PG_GETARG_BOOL(2);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

  PG_RETURN_XML_P(stringinfo_to_xmltype(query_to_xml_internal(
      query, NULL, NULL, nulls, tableforest, targetns, true)));
}

Datum cursor_to_xml(PG_FUNCTION_ARGS) {
  char *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
  int32 count = PG_GETARG_INT32(1);
  bool nulls = PG_GETARG_BOOL(2);
  bool tableforest = PG_GETARG_BOOL(3);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(4));

  StringInfoData result;
  Portal portal;
  uint64 i;

  initStringInfo(&result);

  if (!tableforest) {
    xmldata_root_element_start(&result, "table", NULL, targetns, true);
    appendStringInfoChar(&result, '\n');
  }

  SPI_connect();
  portal = SPI_cursor_find(name);
  if (portal == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_CURSOR),
                    errmsg("cursor \"%s\" does not exist", name)));

  SPI_cursor_fetch(portal, true, count);
  for (i = 0; i < SPI_processed; i++)
    SPI_sql_row_to_xmlelement(i, &result, NULL, nulls, tableforest, targetns,
                              true);

  SPI_finish();

  if (!tableforest)
    xmldata_root_element_end(&result, "table");

  PG_RETURN_XML_P(stringinfo_to_xmltype(&result));
}

/*
 * Write the start tag of the root element of a data mapping.
 *
 * top_level means that this is the very top level of the eventual
 * output.  For example, when the user calls table_to_xml, then a call
 * with a table name to this function is the top level.  When the user
 * calls database_to_xml, then a call with a schema name to this
 * function is not the top level.  If top_level is false, then the XML
 * namespace declarations are omitted, because they supposedly already
 * appeared earlier in the output.  Repeating them is not wrong, but
 * it looks ugly.
 */
static void xmldata_root_element_start(StringInfo result, const char *eltname,
                                       const char *xmlschema,
                                       const char *targetns, bool top_level) {
  /* This isn't really wrong but currently makes no sense. */
  Assert(top_level || !xmlschema);

  appendStringInfo(result, "<%s", eltname);
  if (top_level) {
    appendStringInfoString(result, " xmlns:xsi=\"" NAMESPACE_XSI "\"");
    if (strlen(targetns) > 0)
      appendStringInfo(result, " xmlns=\"%s\"", targetns);
  }
  if (xmlschema) {
    /* FIXME: better targets */
    if (strlen(targetns) > 0)
      appendStringInfo(result, " xsi:schemaLocation=\"%s #\"", targetns);
    else
      appendStringInfoString(result, " xsi:noNamespaceSchemaLocation=\"#\"");
  }
  appendStringInfoString(result, ">\n");
}

static void xmldata_root_element_end(StringInfo result, const char *eltname) {
  appendStringInfo(result, "</%s>\n", eltname);
}

static StringInfo query_to_xml_internal(const char *query, char *tablename,
                                        const char *xmlschema, bool nulls,
                                        bool tableforest, const char *targetns,
                                        bool top_level) {
  StringInfo result;
  char *xmltn;
  uint64 i;

  if (tablename)
    xmltn = map_sql_identifier_to_xml_name(tablename, true, false);
  else
    xmltn = "table";

  result = makeStringInfo();

  SPI_connect();
  if (SPI_execute(query, true, 0) != SPI_OK_SELECT)
    ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("invalid query")));

  if (!tableforest) {
    xmldata_root_element_start(result, xmltn, xmlschema, targetns, top_level);
    appendStringInfoChar(result, '\n');
  }

  if (xmlschema)
    appendStringInfo(result, "%s\n\n", xmlschema);

  for (i = 0; i < SPI_processed; i++)
    SPI_sql_row_to_xmlelement(i, result, tablename, nulls, tableforest,
                              targetns, top_level);

  if (!tableforest)
    xmldata_root_element_end(result, xmltn);

  SPI_finish();

  return result;
}

Datum table_to_xmlschema(PG_FUNCTION_ARGS) {
  Oid relid = PG_GETARG_OID(0);
  bool nulls = PG_GETARG_BOOL(1);
  bool tableforest = PG_GETARG_BOOL(2);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
  const char *result;
  Relation rel;

  rel = table_open(relid, AccessShareLock);
  result = map_sql_table_to_xmlschema(rel->rd_att, relid, nulls, tableforest,
                                      targetns);
  table_close(rel, NoLock);

  PG_RETURN_XML_P(cstring_to_xmltype(result));
}

Datum query_to_xmlschema(PG_FUNCTION_ARGS) {
  char *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
  bool nulls = PG_GETARG_BOOL(1);
  bool tableforest = PG_GETARG_BOOL(2);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
  const char *result;
  SPIPlanPtr plan;
  Portal portal;

  SPI_connect();

  if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
    elog(ERROR, "SPI_prepare(\"%s\") failed", query);

  if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
    elog(ERROR, "SPI_cursor_open(\"%s\") failed", query);

  result = _SPI_strdup(map_sql_table_to_xmlschema(
      portal->tupDesc, InvalidOid, nulls, tableforest, targetns));
  SPI_cursor_close(portal);
  SPI_finish();

  PG_RETURN_XML_P(cstring_to_xmltype(result));
}

Datum cursor_to_xmlschema(PG_FUNCTION_ARGS) {
  char *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
  bool nulls = PG_GETARG_BOOL(1);
  bool tableforest = PG_GETARG_BOOL(2);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
  const char *xmlschema;
  Portal portal;

  SPI_connect();
  portal = SPI_cursor_find(name);
  if (portal == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_CURSOR),
                    errmsg("cursor \"%s\" does not exist", name)));
  if (portal->tupDesc == NULL)
    ereport(ERROR, (errcode(ERRCODE_INVALID_CURSOR_STATE),
                    errmsg("portal \"%s\" does not return tuples", name)));

  xmlschema = _SPI_strdup(map_sql_table_to_xmlschema(
      portal->tupDesc, InvalidOid, nulls, tableforest, targetns));
  SPI_finish();

  PG_RETURN_XML_P(cstring_to_xmltype(xmlschema));
}

Datum table_to_xml_and_xmlschema(PG_FUNCTION_ARGS) {
  Oid relid = PG_GETARG_OID(0);
  bool nulls = PG_GETARG_BOOL(1);
  bool tableforest = PG_GETARG_BOOL(2);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
  Relation rel;
  const char *xmlschema;

  rel = table_open(relid, AccessShareLock);
  xmlschema = map_sql_table_to_xmlschema(rel->rd_att, relid, nulls, tableforest,
                                         targetns);
  table_close(rel, NoLock);

  PG_RETURN_XML_P(stringinfo_to_xmltype(table_to_xml_internal(
      relid, xmlschema, nulls, tableforest, targetns, true)));
}

Datum query_to_xml_and_xmlschema(PG_FUNCTION_ARGS) {
  char *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
  bool nulls = PG_GETARG_BOOL(1);
  bool tableforest = PG_GETARG_BOOL(2);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

  const char *xmlschema;
  SPIPlanPtr plan;
  Portal portal;

  SPI_connect();

  if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
    elog(ERROR, "SPI_prepare(\"%s\") failed", query);

  if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
    elog(ERROR, "SPI_cursor_open(\"%s\") failed", query);

  xmlschema = _SPI_strdup(map_sql_table_to_xmlschema(
      portal->tupDesc, InvalidOid, nulls, tableforest, targetns));
  SPI_cursor_close(portal);
  SPI_finish();

  PG_RETURN_XML_P(stringinfo_to_xmltype(query_to_xml_internal(
      query, NULL, xmlschema, nulls, tableforest, targetns, true)));
}

/*
 * Map SQL schema to XML and/or XML Schema document; see SQL/XML:2008
 * sections 9.13, 9.14.
 */

static StringInfo schema_to_xml_internal(Oid nspid, const char *xmlschema,
                                         bool nulls, bool tableforest,
                                         const char *targetns, bool top_level) {
  StringInfo result;
  char *xmlsn;
  List *relid_list;
  ListCell *cell;

  xmlsn =
      map_sql_identifier_to_xml_name(get_namespace_name(nspid), true, false);
  result = makeStringInfo();

  xmldata_root_element_start(result, xmlsn, xmlschema, targetns, top_level);
  appendStringInfoChar(result, '\n');

  if (xmlschema)
    appendStringInfo(result, "%s\n\n", xmlschema);

  SPI_connect();

  relid_list = schema_get_xml_visible_tables(nspid);

  foreach (cell, relid_list) {
    Oid relid = lfirst_oid(cell);
    StringInfo subres;

    subres =
        table_to_xml_internal(relid, NULL, nulls, tableforest, targetns, false);

    appendBinaryStringInfo(result, subres->data, subres->len);
    appendStringInfoChar(result, '\n');
  }

  SPI_finish();

  xmldata_root_element_end(result, xmlsn);

  return result;
}

Datum schema_to_xml(PG_FUNCTION_ARGS) {
  Name name = PG_GETARG_NAME(0);
  bool nulls = PG_GETARG_BOOL(1);
  bool tableforest = PG_GETARG_BOOL(2);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

  char *schemaname;
  Oid nspid;

  schemaname = NameStr(*name);
  nspid = LookupExplicitNamespace(schemaname, false);

  PG_RETURN_XML_P(stringinfo_to_xmltype(
      schema_to_xml_internal(nspid, NULL, nulls, tableforest, targetns, true)));
}

/*
 * Write the start element of the root element of an XML Schema mapping.
 */
static void xsd_schema_element_start(StringInfo result, const char *targetns) {
  appendStringInfoString(result, "<xsd:schema\n"
                                 "    xmlns:xsd=\"" NAMESPACE_XSD "\"");
  if (strlen(targetns) > 0)
    appendStringInfo(result,
                     "\n"
                     "    targetNamespace=\"%s\"\n"
                     "    elementFormDefault=\"qualified\"",
                     targetns);
  appendStringInfoString(result, ">\n\n");
}

static void xsd_schema_element_end(StringInfo result) {
  appendStringInfoString(result, "</xsd:schema>");
}

static StringInfo schema_to_xmlschema_internal(const char *schemaname,
                                               bool nulls, bool tableforest,
                                               const char *targetns) {
  Oid nspid;
  List *relid_list;
  List *tupdesc_list;
  ListCell *cell;
  StringInfo result;

  result = makeStringInfo();

  nspid = LookupExplicitNamespace(schemaname, false);

  xsd_schema_element_start(result, targetns);

  SPI_connect();

  relid_list = schema_get_xml_visible_tables(nspid);

  tupdesc_list = NIL;
  foreach (cell, relid_list) {
    Relation rel;

    rel = table_open(lfirst_oid(cell), AccessShareLock);
    tupdesc_list = lappend(tupdesc_list, CreateTupleDescCopy(rel->rd_att));
    table_close(rel, NoLock);
  }

  appendStringInfoString(result,
                         map_sql_typecoll_to_xmlschema_types(tupdesc_list));

  appendStringInfoString(
      result, map_sql_schema_to_xmlschema_types(nspid, relid_list, nulls,
                                                tableforest, targetns));

  xsd_schema_element_end(result);

  SPI_finish();

  return result;
}

Datum schema_to_xmlschema(PG_FUNCTION_ARGS) {
  Name name = PG_GETARG_NAME(0);
  bool nulls = PG_GETARG_BOOL(1);
  bool tableforest = PG_GETARG_BOOL(2);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

  PG_RETURN_XML_P(stringinfo_to_xmltype(schema_to_xmlschema_internal(
      NameStr(*name), nulls, tableforest, targetns)));
}

Datum schema_to_xml_and_xmlschema(PG_FUNCTION_ARGS) {
  Name name = PG_GETARG_NAME(0);
  bool nulls = PG_GETARG_BOOL(1);
  bool tableforest = PG_GETARG_BOOL(2);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
  char *schemaname;
  Oid nspid;
  StringInfo xmlschema;

  schemaname = NameStr(*name);
  nspid = LookupExplicitNamespace(schemaname, false);

  xmlschema =
      schema_to_xmlschema_internal(schemaname, nulls, tableforest, targetns);

  PG_RETURN_XML_P(stringinfo_to_xmltype(schema_to_xml_internal(
      nspid, xmlschema->data, nulls, tableforest, targetns, true)));
}

/*
 * Map SQL database to XML and/or XML Schema document; see SQL/XML:2008
 * sections 9.16, 9.17.
 */

static StringInfo database_to_xml_internal(const char *xmlschema, bool nulls,
                                           bool tableforest,
                                           const char *targetns) {
  StringInfo result;
  List *nspid_list;
  ListCell *cell;
  char *xmlcn;

  xmlcn = map_sql_identifier_to_xml_name(get_database_name(MyDatabaseId), true,
                                         false);
  result = makeStringInfo();

  xmldata_root_element_start(result, xmlcn, xmlschema, targetns, true);
  appendStringInfoChar(result, '\n');

  if (xmlschema)
    appendStringInfo(result, "%s\n\n", xmlschema);

  SPI_connect();

  nspid_list = database_get_xml_visible_schemas();

  foreach (cell, nspid_list) {
    Oid nspid = lfirst_oid(cell);
    StringInfo subres;

    subres = schema_to_xml_internal(nspid, NULL, nulls, tableforest, targetns,
                                    false);

    appendBinaryStringInfo(result, subres->data, subres->len);
    appendStringInfoChar(result, '\n');
  }

  SPI_finish();

  xmldata_root_element_end(result, xmlcn);

  return result;
}

Datum database_to_xml(PG_FUNCTION_ARGS) {
  bool nulls = PG_GETARG_BOOL(0);
  bool tableforest = PG_GETARG_BOOL(1);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(2));

  PG_RETURN_XML_P(stringinfo_to_xmltype(
      database_to_xml_internal(NULL, nulls, tableforest, targetns)));
}

static StringInfo database_to_xmlschema_internal(bool nulls, bool tableforest,
                                                 const char *targetns) {
  List *relid_list;
  List *nspid_list;
  List *tupdesc_list;
  ListCell *cell;
  StringInfo result;

  result = makeStringInfo();

  xsd_schema_element_start(result, targetns);

  SPI_connect();

  relid_list = database_get_xml_visible_tables();
  nspid_list = database_get_xml_visible_schemas();

  tupdesc_list = NIL;
  foreach (cell, relid_list) {
    Relation rel;

    rel = table_open(lfirst_oid(cell), AccessShareLock);
    tupdesc_list = lappend(tupdesc_list, CreateTupleDescCopy(rel->rd_att));
    table_close(rel, NoLock);
  }

  appendStringInfoString(result,
                         map_sql_typecoll_to_xmlschema_types(tupdesc_list));

  appendStringInfoString(result, map_sql_catalog_to_xmlschema_types(
                                     nspid_list, nulls, tableforest, targetns));

  xsd_schema_element_end(result);

  SPI_finish();

  return result;
}

Datum database_to_xmlschema(PG_FUNCTION_ARGS) {
  bool nulls = PG_GETARG_BOOL(0);
  bool tableforest = PG_GETARG_BOOL(1);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(2));

  PG_RETURN_XML_P(stringinfo_to_xmltype(
      database_to_xmlschema_internal(nulls, tableforest, targetns)));
}

Datum database_to_xml_and_xmlschema(PG_FUNCTION_ARGS) {
  bool nulls = PG_GETARG_BOOL(0);
  bool tableforest = PG_GETARG_BOOL(1);
  const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(2));
  StringInfo xmlschema;

  xmlschema = database_to_xmlschema_internal(nulls, tableforest, targetns);

  PG_RETURN_XML_P(stringinfo_to_xmltype(
      database_to_xml_internal(xmlschema->data, nulls, tableforest, targetns)));
}

/*
 * Map a multi-part SQL name to an XML name; see SQL/XML:2008 section
 * 9.2.
 */
static char *map_multipart_sql_identifier_to_xml_name(const char *a,
                                                      const char *b,
                                                      const char *c,
                                                      const char *d) {
  StringInfoData result;

  initStringInfo(&result);

  if (a)
    appendStringInfoString(&result,
                           map_sql_identifier_to_xml_name(a, true, true));
  if (b)
    appendStringInfo(&result, ".%s",
                     map_sql_identifier_to_xml_name(b, true, true));
  if (c)
    appendStringInfo(&result, ".%s",
                     map_sql_identifier_to_xml_name(c, true, true));
  if (d)
    appendStringInfo(&result, ".%s",
                     map_sql_identifier_to_xml_name(d, true, true));

  return result.data;
}

/*
 * Map an SQL table to an XML Schema document; see SQL/XML:2008
 * section 9.11.
 *
 * Map an SQL table to XML Schema data types; see SQL/XML:2008 section
 * 9.9.
 */
static const char *map_sql_table_to_xmlschema(TupleDesc tupdesc, Oid relid,
                                              bool nulls, bool tableforest,
                                              const char *targetns) {
  int i;
  char *xmltn;
  char *tabletypename;
  char *rowtypename;
  StringInfoData result;

  initStringInfo(&result);

  if (OidIsValid(relid)) {
    HeapTuple tuple;
    Form_pg_class reltuple;

    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(tuple))
      elog(ERROR, "cache lookup failed for relation %u", relid);
    reltuple = (Form_pg_class)GETSTRUCT(tuple);

    xmltn =
        map_sql_identifier_to_xml_name(NameStr(reltuple->relname), true, false);

    tabletypename = map_multipart_sql_identifier_to_xml_name(
        "TableType", get_database_name(MyDatabaseId),
        get_namespace_name(reltuple->relnamespace), NameStr(reltuple->relname));

    rowtypename = map_multipart_sql_identifier_to_xml_name(
        "RowType", get_database_name(MyDatabaseId),
        get_namespace_name(reltuple->relnamespace), NameStr(reltuple->relname));

    ReleaseSysCache(tuple);
  } else {
    if (tableforest)
      xmltn = "row";
    else
      xmltn = "table";

    tabletypename = "TableType";
    rowtypename = "RowType";
  }

  xsd_schema_element_start(&result, targetns);

  appendStringInfoString(
      &result, map_sql_typecoll_to_xmlschema_types(list_make1(tupdesc)));

  appendStringInfo(&result,
                   "<xsd:complexType name=\"%s\">\n"
                   "  <xsd:sequence>\n",
                   rowtypename);

  for (i = 0; i < tupdesc->natts; i++) {
    Form_pg_attribute att = TupleDescAttr(tupdesc, i);

    if (att->attisdropped)
      continue;
    appendStringInfo(
        &result, "    <xsd:element name=\"%s\" type=\"%s\"%s></xsd:element>\n",
        map_sql_identifier_to_xml_name(NameStr(att->attname), true, false),
        map_sql_type_to_xml_name(att->atttypid, -1),
        nulls ? " nillable=\"true\"" : " minOccurs=\"0\"");
  }

  appendStringInfoString(&result, "  </xsd:sequence>\n"
                                  "</xsd:complexType>\n\n");

  if (!tableforest) {
    appendStringInfo(&result,
                     "<xsd:complexType name=\"%s\">\n"
                     "  <xsd:sequence>\n"
                     "    <xsd:element name=\"row\" type=\"%s\" "
                     "minOccurs=\"0\" maxOccurs=\"unbounded\"/>\n"
                     "  </xsd:sequence>\n"
                     "</xsd:complexType>\n\n",
                     tabletypename, rowtypename);

    appendStringInfo(&result, "<xsd:element name=\"%s\" type=\"%s\"/>\n\n",
                     xmltn, tabletypename);
  } else
    appendStringInfo(&result, "<xsd:element name=\"%s\" type=\"%s\"/>\n\n",
                     xmltn, rowtypename);

  xsd_schema_element_end(&result);

  return result.data;
}

/*
 * Map an SQL schema to XML Schema data types; see SQL/XML:2008
 * section 9.12.
 */
static const char *
map_sql_schema_to_xmlschema_types(Oid nspid, List *relid_list, bool nulls,
                                  bool tableforest, const char *targetns) {
  char *dbname;
  char *nspname;
  char *xmlsn;
  char *schematypename;
  StringInfoData result;
  ListCell *cell;

  dbname = get_database_name(MyDatabaseId);
  nspname = get_namespace_name(nspid);

  initStringInfo(&result);

  xmlsn = map_sql_identifier_to_xml_name(nspname, true, false);

  schematypename = map_multipart_sql_identifier_to_xml_name(
      "SchemaType", dbname, nspname, NULL);

  appendStringInfo(&result, "<xsd:complexType name=\"%s\">\n", schematypename);
  if (!tableforest)
    appendStringInfoString(&result, "  <xsd:all>\n");
  else
    appendStringInfoString(&result, "  <xsd:sequence>\n");

  foreach (cell, relid_list) {
    Oid relid = lfirst_oid(cell);
    char *relname = get_rel_name(relid);
    char *xmltn = map_sql_identifier_to_xml_name(relname, true, false);
    char *tabletypename = map_multipart_sql_identifier_to_xml_name(
        tableforest ? "RowType" : "TableType", dbname, nspname, relname);

    if (!tableforest)
      appendStringInfo(&result, "    <xsd:element name=\"%s\" type=\"%s\"/>\n",
                       xmltn, tabletypename);
    else
      appendStringInfo(&result,
                       "    <xsd:element name=\"%s\" type=\"%s\" "
                       "minOccurs=\"0\" maxOccurs=\"unbounded\"/>\n",
                       xmltn, tabletypename);
  }

  if (!tableforest)
    appendStringInfoString(&result, "  </xsd:all>\n");
  else
    appendStringInfoString(&result, "  </xsd:sequence>\n");
  appendStringInfoString(&result, "</xsd:complexType>\n\n");

  appendStringInfo(&result, "<xsd:element name=\"%s\" type=\"%s\"/>\n\n", xmlsn,
                   schematypename);

  return result.data;
}

/*
 * Map an SQL catalog to XML Schema data types; see SQL/XML:2008
 * section 9.15.
 */
static const char *map_sql_catalog_to_xmlschema_types(List *nspid_list,
                                                      bool nulls,
                                                      bool tableforest,
                                                      const char *targetns) {
  char *dbname;
  char *xmlcn;
  char *catalogtypename;
  StringInfoData result;
  ListCell *cell;

  dbname = get_database_name(MyDatabaseId);

  initStringInfo(&result);

  xmlcn = map_sql_identifier_to_xml_name(dbname, true, false);

  catalogtypename = map_multipart_sql_identifier_to_xml_name(
      "CatalogType", dbname, NULL, NULL);

  appendStringInfo(&result, "<xsd:complexType name=\"%s\">\n", catalogtypename);
  appendStringInfoString(&result, "  <xsd:all>\n");

  foreach (cell, nspid_list) {
    Oid nspid = lfirst_oid(cell);
    char *nspname = get_namespace_name(nspid);
    char *xmlsn = map_sql_identifier_to_xml_name(nspname, true, false);
    char *schematypename = map_multipart_sql_identifier_to_xml_name(
        "SchemaType", dbname, nspname, NULL);

    appendStringInfo(&result, "    <xsd:element name=\"%s\" type=\"%s\"/>\n",
                     xmlsn, schematypename);
  }

  appendStringInfoString(&result, "  </xsd:all>\n");
  appendStringInfoString(&result, "</xsd:complexType>\n\n");

  appendStringInfo(&result, "<xsd:element name=\"%s\" type=\"%s\"/>\n\n", xmlcn,
                   catalogtypename);

  return result.data;
}

/*
 * Map an SQL data type to an XML name; see SQL/XML:2008 section 9.4.
 */
static const char *map_sql_type_to_xml_name(Oid typeoid, int typmod) {
  StringInfoData result;

  initStringInfo(&result);

  switch (typeoid) {
  case BPCHAROID:
    if (typmod == -1)
      appendStringInfoString(&result, "CHAR");
    else
      appendStringInfo(&result, "CHAR_%d", typmod - VARHDRSZ);
    break;
  case VARCHAROID:
    if (typmod == -1)
      appendStringInfoString(&result, "VARCHAR");
    else
      appendStringInfo(&result, "VARCHAR_%d", typmod - VARHDRSZ);
    break;
  case DECIMAL64OID:
  case DECIMAL128OID:
    if (typmod == -1)
      appendStringInfoString(&result, "NUMERIC");
    else
      appendStringInfo(&result, "NUMERIC_%d_%d",
                       ((typmod - VARHDRSZ) >> 16) & 0xffff,
                       (typmod - VARHDRSZ) & 0xffff);
    break;
  case INT4OID:
    appendStringInfoString(&result, "INTEGER");
    break;
  case INT2OID:
    appendStringInfoString(&result, "SMALLINT");
    break;
  case INT8OID:
    appendStringInfoString(&result, "BIGINT");
    break;
  case FLOAT4OID:
    appendStringInfoString(&result, "REAL");
    break;
  case FLOAT8OID:
    appendStringInfoString(&result, "DOUBLE");
    break;
  case BOOLOID:
    appendStringInfoString(&result, "BOOLEAN");
    break;
  case TIMEOID:
    if (typmod == -1)
      appendStringInfoString(&result, "TIME");
    else
      appendStringInfo(&result, "TIME_%d", typmod);
    break;
  case TIMETZOID:
    if (typmod == -1)
      appendStringInfoString(&result, "TIME_WTZ");
    else
      appendStringInfo(&result, "TIME_WTZ_%d", typmod);
    break;
  case TIMESTAMPOID:
    if (typmod == -1)
      appendStringInfoString(&result, "TIMESTAMP");
    else
      appendStringInfo(&result, "TIMESTAMP_%d", typmod);
    break;
  case TIMESTAMPTZOID:
    if (typmod == -1)
      appendStringInfoString(&result, "TIMESTAMP_WTZ");
    else
      appendStringInfo(&result, "TIMESTAMP_WTZ_%d", typmod);
    break;
  case DATEOID:
    appendStringInfoString(&result, "DATE");
    break;
  case XMLOID:
    appendStringInfoString(&result, "XML");
    break;
  default: {
    HeapTuple tuple;
    Form_pg_type typtuple;

    tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeoid));
    if (!HeapTupleIsValid(tuple))
      elog(ERROR, "cache lookup failed for type %u", typeoid);
    typtuple = (Form_pg_type)GETSTRUCT(tuple);

    appendStringInfoString(
        &result, map_multipart_sql_identifier_to_xml_name(
                     (typtuple->typtype == TYPTYPE_DOMAIN) ? "Domain" : "UDT",
                     get_database_name(MyDatabaseId),
                     get_namespace_name(typtuple->typnamespace),
                     NameStr(typtuple->typname)));

    ReleaseSysCache(tuple);
  }
  }

  return result.data;
}

/*
 * Map a collection of SQL data types to XML Schema data types; see
 * SQL/XML:2008 section 9.7.
 */
static const char *map_sql_typecoll_to_xmlschema_types(List *tupdesc_list) {
  List *uniquetypes = NIL;
  int i;
  StringInfoData result;
  ListCell *cell0;

  /* extract all column types used in the set of TupleDescs */
  foreach (cell0, tupdesc_list) {
    TupleDesc tupdesc = (TupleDesc)lfirst(cell0);

    for (i = 0; i < tupdesc->natts; i++) {
      Form_pg_attribute att = TupleDescAttr(tupdesc, i);

      if (att->attisdropped)
        continue;
      uniquetypes = list_append_unique_oid(uniquetypes, att->atttypid);
    }
  }

  /* add base types of domains */
  foreach (cell0, uniquetypes) {
    Oid typid = lfirst_oid(cell0);
    Oid basetypid = getBaseType(typid);

    if (basetypid != typid)
      uniquetypes = list_append_unique_oid(uniquetypes, basetypid);
  }

  /* Convert to textual form */
  initStringInfo(&result);

  foreach (cell0, uniquetypes) {
    appendStringInfo(&result, "%s\n",
                     map_sql_type_to_xmlschema_type(lfirst_oid(cell0), -1));
  }

  return result.data;
}

/*
 * Map an SQL data type to a named XML Schema data type; see
 * SQL/XML:2008 sections 9.5 and 9.6.
 *
 * (The distinction between 9.5 and 9.6 is basically that 9.6 adds
 * a name attribute, which this function does.  The name-less version
 * 9.5 doesn't appear to be required anywhere.)
 */
static const char *map_sql_type_to_xmlschema_type(Oid typeoid, int typmod) {
  StringInfoData result;
  const char *typename_ = map_sql_type_to_xml_name(typeoid, typmod);

  initStringInfo(&result);

  if (typeoid == XMLOID) {
    appendStringInfoString(
        &result, "<xsd:complexType mixed=\"true\">\n"
                 "  <xsd:sequence>\n"
                 "    <xsd:any name=\"element\" minOccurs=\"0\" "
                 "maxOccurs=\"unbounded\" processContents=\"skip\"/>\n"
                 "  </xsd:sequence>\n"
                 "</xsd:complexType>\n");
  } else {
    appendStringInfo(&result, "<xsd:simpleType name=\"%s\">\n", typename_);

    switch (typeoid) {
    case BPCHAROID:
    case VARCHAROID:
    case TEXTOID:
      appendStringInfoString(&result,
                             "  <xsd:restriction base=\"xsd:string\">\n");
      if (typmod != -1)
        appendStringInfo(&result, "    <xsd:maxLength value=\"%d\"/>\n",
                         typmod - VARHDRSZ);
      appendStringInfoString(&result, "  </xsd:restriction>\n");
      break;

    case BYTEAOID:
      appendStringInfo(&result,
                       "  <xsd:restriction base=\"xsd:%s\">\n"
                       "  </xsd:restriction>\n",
                       tcb->guc.xmlbinary == XMLBINARY_BASE64 ? "base64Binary"
                                                              : "hexBinary");
      break;

    case DECIMAL64OID:
    case DECIMAL128OID:
      if (typmod != -1)
        appendStringInfo(&result,
                         "  <xsd:restriction base=\"xsd:decimal\">\n"
                         "    <xsd:totalDigits value=\"%d\"/>\n"
                         "    <xsd:fractionDigits value=\"%d\"/>\n"
                         "  </xsd:restriction>\n",
                         ((typmod - VARHDRSZ) >> 16) & 0xffff,
                         (typmod - VARHDRSZ) & 0xffff);
      break;

    case INT2OID:
      appendStringInfo(&result,
                       "  <xsd:restriction base=\"xsd:short\">\n"
                       "    <xsd:maxInclusive value=\"%d\"/>\n"
                       "    <xsd:minInclusive value=\"%d\"/>\n"
                       "  </xsd:restriction>\n",
                       SHRT_MAX, SHRT_MIN);
      break;

    case INT4OID:
      appendStringInfo(&result,
                       "  <xsd:restriction base=\"xsd:int\">\n"
                       "    <xsd:maxInclusive value=\"%d\"/>\n"
                       "    <xsd:minInclusive value=\"%d\"/>\n"
                       "  </xsd:restriction>\n",
                       INT_MAX, INT_MIN);
      break;

    case INT8OID:
      appendStringInfo(&result,
                       "  <xsd:restriction base=\"xsd:long\">\n"
                       "    <xsd:maxInclusive value=\"" INT64_FORMAT "\"/>\n"
                       "    <xsd:minInclusive value=\"" INT64_FORMAT "\"/>\n"
                       "  </xsd:restriction>\n",
                       PG_INT64_MAX, PG_INT64_MIN);
      break;

    case FLOAT4OID:
      appendStringInfoString(
          &result,
          "  <xsd:restriction base=\"xsd:float\"></xsd:restriction>\n");
      break;

    case FLOAT8OID:
      appendStringInfoString(
          &result,
          "  <xsd:restriction base=\"xsd:double\"></xsd:restriction>\n");
      break;

    case BOOLOID:
      appendStringInfoString(
          &result,
          "  <xsd:restriction base=\"xsd:boolean\"></xsd:restriction>\n");
      break;

    case TIMEOID:
    case TIMETZOID: {
      const char *tz =
          (typeoid == TIMETZOID ? "(\\+|-)\\p{Nd}{2}:\\p{Nd}{2}" : "");

      if (typmod == -1)
        appendStringInfo(
            &result,
            "  <xsd:restriction base=\"xsd:time\">\n"
            "    <xsd:pattern "
            "value=\"\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}(.\\p{Nd}+)?%s\"/>\n"
            "  </xsd:restriction>\n",
            tz);
      else if (typmod == 0)
        appendStringInfo(
            &result,
            "  <xsd:restriction base=\"xsd:time\">\n"
            "    <xsd:pattern value=\"\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}%s\"/>\n"
            "  </xsd:restriction>\n",
            tz);
      else
        appendStringInfo(
            &result,
            "  <xsd:restriction base=\"xsd:time\">\n"
            "    <xsd:pattern "
            "value=\"\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}.\\p{Nd}{%d}%s\"/>\n"
            "  </xsd:restriction>\n",
            typmod - VARHDRSZ, tz);
      break;
    }

    case TIMESTAMPOID:
    case TIMESTAMPTZOID: {
      const char *tz =
          (typeoid == TIMESTAMPTZOID ? "(\\+|-)\\p{Nd}{2}:\\p{Nd}{2}" : "");

      if (typmod == -1)
        appendStringInfo(&result,
                         "  <xsd:restriction base=\"xsd:dateTime\">\n"
                         "    <xsd:pattern "
                         "value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}T\\p{Nd}{2}:"
                         "\\p{Nd}{2}:\\p{Nd}{2}(.\\p{Nd}+)?%s\"/>\n"
                         "  </xsd:restriction>\n",
                         tz);
      else if (typmod == 0)
        appendStringInfo(&result,
                         "  <xsd:restriction base=\"xsd:dateTime\">\n"
                         "    <xsd:pattern "
                         "value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}T\\p{Nd}{2}:"
                         "\\p{Nd}{2}:\\p{Nd}{2}%s\"/>\n"
                         "  </xsd:restriction>\n",
                         tz);
      else
        appendStringInfo(&result,
                         "  <xsd:restriction base=\"xsd:dateTime\">\n"
                         "    <xsd:pattern "
                         "value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}T\\p{Nd}{2}:"
                         "\\p{Nd}{2}:\\p{Nd}{2}.\\p{Nd}{%d}%s\"/>\n"
                         "  </xsd:restriction>\n",
                         typmod - VARHDRSZ, tz);
      break;
    }

    case DATEOID:
      appendStringInfoString(
          &result,
          "  <xsd:restriction base=\"xsd:date\">\n"
          "    <xsd:pattern value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}\"/>\n"
          "  </xsd:restriction>\n");
      break;

    default:
      if (get_typtype(typeoid) == TYPTYPE_DOMAIN) {
        Oid base_typeoid;
        int32 base_typmod = -1;

        base_typeoid = getBaseTypeAndTypmod(typeoid, &base_typmod);

        appendStringInfo(&result, "  <xsd:restriction base=\"%s\"/>\n",
                         map_sql_type_to_xml_name(base_typeoid, base_typmod));
      }
      break;
    }
    appendStringInfoString(&result, "</xsd:simpleType>\n");
  }

  return result.data;
}

/*
 * Map an SQL row to an XML element, taking the row from the active
 * SPI cursor.  See also SQL/XML:2008 section 9.10.
 */
static void SPI_sql_row_to_xmlelement(uint64 rownum, StringInfo result,
                                      char *tablename, bool nulls,
                                      bool tableforest, const char *targetns,
                                      bool top_level) {
  int i;
  char *xmltn;

  if (tablename)
    xmltn = map_sql_identifier_to_xml_name(tablename, true, false);
  else {
    if (tableforest)
      xmltn = "row";
    else
      xmltn = "table";
  }

  if (tableforest)
    xmldata_root_element_start(result, xmltn, NULL, targetns, top_level);
  else
    appendStringInfoString(result, "<row>\n");

  for (i = 1; i <= SPI_tuptable->tupdesc->natts; i++) {
    char *colname;
    Datum colval;
    bool isnull;

    colname = map_sql_identifier_to_xml_name(
        SPI_fname(SPI_tuptable->tupdesc, i), true, false);
    colval = SPI_getbinval(SPI_tuptable->vals[rownum], SPI_tuptable->tupdesc, i,
                           &isnull);
    if (isnull) {
      if (nulls)
        appendStringInfo(result, "  <%s xsi:nil=\"true\"/>\n", colname);
    } else
      appendStringInfo(
          result, "  <%s>%s</%s>\n", colname,
          map_sql_value_to_xml_value(
              colval, SPI_gettypeid(SPI_tuptable->tupdesc, i), true),
          colname);
  }

  if (tableforest) {
    xmldata_root_element_end(result, xmltn);
    appendStringInfoChar(result, '\n');
  } else
    appendStringInfoString(result, "</row>\n\n");
}

/*
 * XPath related functions
 */


/*
 * Evaluate XPath expression and return array of XML values.
 *
 * As we have no support of XQuery sequences yet, this function seems
 * to be the most useful one (array of XML functions plays a role of
 * some kind of substitution for XQuery sequences).
 */
Datum xpath(PG_FUNCTION_ARGS) {
  NO_XML_SUPPORT();
  return 0;
}

/*
 * Determines if the node specified by the supplied XPath exists
 * in a given XML document, returning a boolean.
 */
Datum xmlexists(PG_FUNCTION_ARGS) {
  NO_XML_SUPPORT();
  return 0;
}

/*
 * Determines if the node specified by the supplied XPath exists
 * in a given XML document, returning a boolean. Differs from
 * xmlexists as it supports namespaces and is not defined in SQL/XML.
 */
Datum xpath_exists(PG_FUNCTION_ARGS) {
  NO_XML_SUPPORT();
  return 0;
}

/*
 * Functions for checking well-formed-ness
 */


Datum xml_is_well_formed(PG_FUNCTION_ARGS) {
  NO_XML_SUPPORT();
  return 0;
}

Datum xml_is_well_formed_document(PG_FUNCTION_ARGS) {
  NO_XML_SUPPORT();
  return 0;
}

Datum xml_is_well_formed_content(PG_FUNCTION_ARGS) {
  NO_XML_SUPPORT();
  return 0;
}

/*
 * support functions for XMLTABLE
 *
 */

/*
 * XmlTableInitOpaque
 *		Fill in TableFuncScanState->opaque for XmlTable processor;
 *initialize the XML parser.
 *
 * Note: Because we call pg_xml_init() here and pg_xml_done() in
 * XmlTableDestroyOpaque, it is critical for robustness that no other
 * executor nodes run until this node is processed to completion.  Caller
 * must execute this to completion (probably filling a tuplestore to exhaust
 * this node in a single pass) instead of using row-per-call mode.
 */
static void XmlTableInitOpaque(TableFuncScanState *state, int natts) {
  NO_XML_SUPPORT();
}

/*
 * XmlTableSetDocument
 *		Install the input document
 */
static void XmlTableSetDocument(TableFuncScanState *state, Datum value) {
  NO_XML_SUPPORT();
}

/*
 * XmlTableSetNamespace
 *		Add a namespace declaration
 */
static void XmlTableSetNamespace(TableFuncScanState *state, const char *name,
                                 const char *uri) {
  NO_XML_SUPPORT();
}

/*
 * XmlTableSetRowFilter
 *		Install the row-filter Xpath expression.
 */
static void XmlTableSetRowFilter(TableFuncScanState *state, const char *path) {
  NO_XML_SUPPORT();
}

/*
 * XmlTableSetColumnFilter
 *		Install the column-filter Xpath expression, for the given
 *column.
 */
static void XmlTableSetColumnFilter(TableFuncScanState *state, const char *path,
                                    int colnum) {
  NO_XML_SUPPORT();
}

/*
 * XmlTableFetchRow
 *		Prepare the next "current" tuple for upcoming GetValue calls.
 *		Returns false if the row-filter expression returned no more
 *rows.
 */
static bool XmlTableFetchRow(TableFuncScanState *state) {
  NO_XML_SUPPORT();
  return false;
}

/*
 * XmlTableGetValue
 *		Return the value for column number 'colnum' for the current row.
 *If column -1 is requested, return representation of the whole row.
 *
 * This leaks memory, so be sure to reset often the context in which it's
 * called.
 */
static Datum XmlTableGetValue(TableFuncScanState *state, int colnum, Oid typid,
                              int32 typmod, bool *isnull) {
  NO_XML_SUPPORT();
  return 0;
}

/*
 * XmlTableDestroyOpaque
 *		Release all libxml2 resources
 */
static void XmlTableDestroyOpaque(TableFuncScanState *state) {
  NO_XML_SUPPORT();
}
