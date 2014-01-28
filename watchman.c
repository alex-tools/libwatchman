#include "watchman.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <jansson.h>

static void watchman_err(struct watchman_error *error, char *message, ...)
    __attribute__ ((format(printf, 2, 3)));

static void
watchman_err(struct watchman_error *error, char *message, ...)
{
    va_list argptr;
    va_start(argptr, message);
    char c;
    int len = vsnprintf(&c, 1, message, argptr);
    va_end(argptr);

    error->message = malloc(len + 1);
    va_start(argptr, message);
    vsnprintf(error->message, len + 1, message, argptr);
    va_end(argptr);
}

struct watchman_connection *
watchman_sock_connect(struct watchman_error *error, const char *sockname)
{
    struct sockaddr_un addr = { };

    int fd;
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        watchman_err(error, "Socket error %s", strerror(errno));
        return NULL;
    }

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockname, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        watchman_err(error, "Connect error %s", strerror(errno));
        return NULL;
    }

    FILE *sockfp = fdopen(fd, "r+");
    if (!sockfp) {
        watchman_err(error,
                     "Failed to connect to watchman socket %s: %s.",
                     sockname, strerror(errno));
        return NULL;
    }
    setlinebuf(sockfp);

    struct watchman_connection *conn = malloc(sizeof(*conn));
    conn->fp = sockfp;
    return conn;
}

struct watchman_connection *
watchman_connect(struct watchman_error *error)
{
    struct watchman_connection *conn = NULL;
    FILE *fp = popen("watchman get-sockname", "r");
    if (!fp) {
        watchman_err(error,
                     "Could not run watchman get-sockname: %s",
                     strerror(errno));
        return NULL;
    }
    json_error_t jerror;
    json_t *json = json_loadf(fp, 0, &jerror);
    pclose(fp);
    if (!json) {
        watchman_err(error,
                     "Got bad JSON from watchman get-sockname: %s",
                     jerror.text);
        goto done;
    }
    if (!json_is_object(json)) {
        watchman_err(error, "Got bad JSON from watchman get-sockname:"
                     " object expected");
        goto bad_json;
    }
    json_t *sockname_obj = json_object_get(json, "sockname");
    if (!sockname_obj) {
        watchman_err(error, "Got bad JSON from watchman get-sockname:"
                     " sockname element expected");
        goto bad_json;
    }
    if (!json_is_string(sockname_obj)) {
        watchman_err(error, "Got bad JSON from watchman get-sockname:"
                     " sockname is not string");
        goto bad_json;
    }
    const char *sockname = json_string_value(sockname_obj);
    conn = watchman_sock_connect(error, sockname);
bad_json:
    json_decref(json);
done:
    return conn;
}

static int
watchman_send_simple_command(struct watchman_connection *conn,
                             struct watchman_error *error, ...)
{
    int result = 0;
    json_t *cmd_array = json_array();
    va_list argptr;
    va_start(argptr, error);
    char *arg;
    while (arg = va_arg(argptr, char *)) {
        json_array_append_new(cmd_array, json_string(arg));
    }
    int json_result = json_dumpf(cmd_array, conn->fp, JSON_COMPACT);
    if (json_result) {
        watchman_err(error, "Failed to send simple watchman command");
        result = 1;
    }
    fputc('\n', conn->fp);

    json_decref(cmd_array);
    return result;
}

static json_t *
watchman_read(struct watchman_connection *conn, struct watchman_error *error)
{
    json_error_t jerror;
    int flags = JSON_DISABLE_EOF_CHECK;
    json_t *result = json_loadf(conn->fp, flags, &jerror);
    if (!result) {
        watchman_err(error, "Can't parse result from watchman: %s",
                     jerror.text);
        return NULL;
    }
    if (fgetc(conn->fp) != '\n') {
        watchman_err(error, "No newline at end of reply");
        json_decref(result);
        return NULL;
    }
    return result;
}

static int
watchman_read_and_handle_errors(struct watchman_connection *conn,
                                struct watchman_error *error)
{
    json_t *obj = watchman_read(conn, error);
    if (!obj) {
        return 1;
    }
    if (!json_is_object(obj)) {
        char *bogus_json_text = json_dumps(obj, 0);
        watchman_err(error, "Got non-object result from watchman : %s",
                     bogus_json_text);
        free(bogus_json_text);
        json_decref(obj);
        return 1;
    }
    json_t *error_json = json_object_get(obj, "error");
    if (error_json) {
        watchman_err(error, "Got error result from watchman : %s",
                     json_string_value(error_json));
        json_decref(obj);
        return 1;
    }

    json_decref(obj);
    return 0;
}

int
watchman_watch(struct watchman_connection *conn,
               const char *path, struct watchman_error *error)
{
    if (watchman_send_simple_command(conn, error, "watch", path, NULL)) {
        return 1;
    }
    if (watchman_read_and_handle_errors(conn, error)) {
        return 1;
    }
    return 0;
}

int
watchman_watch_del(struct watchman_connection *conn,
                   const char *path, struct watchman_error *error)
{
    if (watchman_send_simple_command(conn, error, "watch-del", path, NULL)) {
        return 1;
    }
    if (watchman_read_and_handle_errors(conn, error)) {
        return 1;
    }
    return 0;
}

static struct watchman_expression *
alloc_expr(enum watchman_expression_type ty)
{
    struct watchman_expression *expr;
    expr = calloc(1, sizeof(*expr));
    expr->ty = ty;
    return expr;
}

struct watchman_expression *
watchman_since_expression(const char *since, enum watchman_clockspec spec)
{
    assert(since);
    struct watchman_expression *expr = alloc_expr(WATCHMAN_EXPR_TY_SINCE);
    expr->e.since_expr.is_str = 1;
    expr->e.since_expr.t.since = strdup(since);
    expr->e.since_expr.clockspec = spec;
    return expr;
}

struct watchman_expression *
watchman_since_expression_time_t(time_t time, enum watchman_clockspec
                                 spec)
{
    struct watchman_expression *expr = alloc_expr(WATCHMAN_EXPR_TY_SINCE);
    expr->e.since_expr.is_str = 0;
    expr->e.since_expr.t.time = time;
    expr->e.since_expr.clockspec = spec;
    return expr;
}

/* corresponds to enum watchman_expression_type */
static char *ty_str[] = {
    "allof",
    "anyof",
    "not",
    "true",
    "false",
    "since",
    "suffix",
    "match",
    "imatch",
    "pcre",
    "ipcre",
    "name",
    "iname",
    "type",
    "empty",
    "exists"
};

/* corresponds to enum watchman_clockspec */
static char *clockspec_str[] = {
    NULL,
    "oclock",
    "cclock",
    "mtime",
    "ctime"
};

/* corresponds to enum watchman_basename */
static char *basename_str[] = {
    NULL,
    "basename",
    "wholename"
};

static json_t *
json_string_from_char(char c)
{
    char str[2] = { c, 0 };
    return json_string(str);
}

static json_t *
json_string_or_array(int nr, char **items)
{
    if (nr == 1) {
        return json_string(items[0]);
    }
    json_t *result = json_array();
    int i;
    for (i = 0; i < nr; ++i) {
        json_array_append_new(result, json_string(items[i]));
    }
    return result;
}

static json_t *
since_to_json(json_t *result, const struct watchman_expression *expr)
{
    if (expr->e.since_expr.is_str) {
        json_array_append_new(result, json_string(expr->e.since_expr.t.since));
    } else {
        json_array_append_new(result, json_integer(expr->e.since_expr.t.time));
    }
    if (expr->e.since_expr.clockspec) {
        char *clockspec = ty_str[expr->e.since_expr.clockspec];
        json_array_append_new(result, json_string(clockspec));
    }
}

static json_t *
to_json(const struct watchman_expression *expr)
{
    json_t *result = json_array();
    json_t *arg;
    json_array_append_new(result, json_string(ty_str[expr->ty]));

    int i;
    switch (expr->ty) {
        case WATCHMAN_EXPR_TY_ALLOF:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_ANYOF:
            for (i = 0; i < expr->e.union_expr.nr; ++i) {
                json_array_append_new(result,
                                      to_json(expr->e.union_expr.clauses[i]));
            }
            break;
        case WATCHMAN_EXPR_TY_NOT:
            json_array_append_new(result, to_json(expr->e.not_expr.clause));
            break;
        case WATCHMAN_EXPR_TY_TRUE:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_FALSE:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_EMPTY:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_EXISTS:
            /* Nothing to do */
            break;

        case WATCHMAN_EXPR_TY_SINCE:
            since_to_json(result, expr);
            break;
        case WATCHMAN_EXPR_TY_SUFFIX:
            json_array_append_new(result,
                                  json_string(expr->e.suffix_expr.suffix));
            break;
        case WATCHMAN_EXPR_TY_MATCH:
                                   /*-fallthrough*/
        case WATCHMAN_EXPR_TY_IMATCH:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_PCRE:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_IPCRE:
            json_array_append_new(result,
                                  json_string(expr->e.match_expr.match));
            if (expr->e.match_expr.basename) {
                char *base = basename_str[expr->e.match_expr.basename];
                json_array_append_new(result, json_string(base));
            }
            break;
        case WATCHMAN_EXPR_TY_NAME:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_INAME:
            arg =
                json_string_or_array(expr->e.name_expr.nr,
                                     expr->e.name_expr.names);
            json_array_append_new(result, arg);
            if (expr->e.name_expr.basename) {
                char *base = basename_str[expr->e.name_expr.basename];
                json_array_append_new(result, json_string(base));
            }
            break;
        case WATCHMAN_EXPR_TY_TYPE:
            json_array_append_new(result,
                                  json_string_from_char(expr->e.
                                                        type_expr.type));
    }
    return result;
}

/* corresponds to enum watchman_fields */
static char *fields_str[] = {
    "name",
    "exists",
    "cclock",
    "oclock",
    "ctime",
    "ctime_ms",
    "ctime_us",
    "ctime_ns",
    "ctime_f",
    "mtime",
    "mtime_ms",
    "mtime_us",
    "mtime_ns",
    "mtime_f",
    "size",
    "uid",
    "gid",
    "ino",
    "dev",
    "nlink",
    "new"
};

json_t *
fields_to_json(int fields)
{
    json_t *result = json_array();
    int i = 0;
    int mask;
    for (mask = 1; mask < WATCHMAN_FIELD_END; mask *= 2) {
        if (fields & mask) {
            json_array_append_new(result, json_string(fields_str[i]));
        }
        ++i;
    }
    return result;
}

#define JSON_ASSERT(cond, condarg, msg)                                 \
    if (!cond(condarg)) {                                               \
        char *dump = json_dumps(condarg, 0);                            \
        watchman_err(error, msg, dump);                                 \
        free(dump);                                                     \
        goto done;                                                      \
    }

struct watchman_watch_list *
watchman_watch_list(struct watchman_connection *conn,
                    struct watchman_error *error)
{
    struct watchman_watch_list *res = NULL;
    struct watchman_watch_list *result = NULL;
    if (watchman_send_simple_command(conn, error, "watch-list", NULL)) {
        return NULL;
    }

    json_t *obj = watchman_read(conn, error);
    if (!obj) {
        return NULL;
    }
    JSON_ASSERT(json_is_object, obj, "Got bogus value from watch-list %s");
    json_t *roots = json_object_get(obj, "roots");
    JSON_ASSERT(json_is_array, roots, "Got bogus value from watch-list %s");

    res = malloc(sizeof(*res));
    int nr = json_array_size(roots);
    res->nr = 0;
    res->roots = calloc(nr, sizeof(*res->roots));
    int i;
    for (i = 0; i < nr; ++i) {
        json_t *root = json_array_get(roots, i);
        JSON_ASSERT(json_is_string, root,
                    "Got non-string root from watch-list %s");
        res->nr++;
        res->roots[i] = strdup(json_string_value(root));
    }
    result = res;
    res = NULL;
done:
    if (res) {
        watchman_free_watch_list(res);
    }
    json_decref(obj);
    return result;
}

#define WRITE_BOOL_STAT(stat, statobj, attr)                            \
    json_t *attr = json_object_get(statobj, #attr);                     \
    if (attr) {                                                         \
        JSON_ASSERT(json_is_boolean, attr, #attr " is not boolean");    \
        stat->attr = json_is_true(attr);                                \
    }

#define WRITE_INT_STAT(stat, statobj, attr)                             \
    json_t *attr = json_object_get(statobj, #attr);                     \
    if (attr) {                                                         \
        JSON_ASSERT(json_is_integer, attr, #attr " is not an integer"); \
        stat->attr = json_integer_value(attr);                          \
    }

#define WRITE_STR_STAT(stat, statobj, attr)                             \
    json_t *attr = json_object_get(statobj, #attr);                     \
    if (attr) {                                                         \
        JSON_ASSERT(json_is_string, attr, #attr " is not an integer");  \
        stat->attr = strdup(json_string_value(attr));                   \
    }

#define WRITE_FLOAT_STAT(stat, statobj, attr)                           \
    json_t *attr = json_object_get(statobj, #attr);                     \
    if (attr) {                                                         \
        JSON_ASSERT(json_is_real, attr, #attr " is not an integer");    \
        stat->attr = json_real_value(attr);                             \
    }

static int
watchman_send(struct watchman_connection *conn,
              json_t *query, struct watchman_error *error)
{
    int json_result = json_dumpf(query, conn->fp, JSON_COMPACT);
    if (json_result) {
        char *dump = json_dumps(query, 0);
        watchman_err(error, "Failed to send watchman query %s", dump);
        free(dump);
        return 1;
    }
    fputc('\n', conn->fp);
    return 0;
}

static struct watchman_query_result *
watchman_query_json(struct watchman_connection *conn, json_t *query,
                    struct watchman_error *error)
{
    struct watchman_query_result *result = NULL;
    struct watchman_query_result *res = NULL;

    if (watchman_send(conn, query, error)) {
        return NULL;
    }
    /* parse the result */
    json_t *obj = watchman_read(conn, error);
    if (!obj) {
        return NULL;
    }
    JSON_ASSERT(json_is_object, obj, "Failed to send watchman query %s");

    json_t *jerror = json_object_get(obj, "error");
    if (jerror) {
        watchman_err(error, "Error result from watchman: %s",
                     json_string_value(jerror));
        goto done;
    }

    res = calloc(1, sizeof(*res));

    json_t *files = json_object_get(obj, "files");
    JSON_ASSERT(json_is_array, files, "Bad files %s");

    int nr = json_array_size(files);
    res->stats = calloc(nr, sizeof(*res->stats));

    int i;
    for (i = 0; i < nr; ++i) {
        struct watchman_stat *stat = res->stats + i;
        json_t *statobj = json_array_get(files, i);
        if (json_is_string(statobj)) {
            /* then hopefully we only requested names */
            stat->name = strdup(json_string_value(statobj));
            res->nr++;
            continue;
        }

        JSON_ASSERT(json_is_object, statobj, "must be object: %s");

        json_t *name = json_object_get(statobj, "name");
        JSON_ASSERT(json_is_string, name, "name must be string: %s");
        stat->name = strdup(json_string_value(name));

        WRITE_BOOL_STAT(stat, statobj, exists);
        WRITE_INT_STAT(stat, statobj, ctime);
        WRITE_INT_STAT(stat, statobj, ctime_ms);
        WRITE_INT_STAT(stat, statobj, ctime_us);
        WRITE_INT_STAT(stat, statobj, ctime_ns);
        WRITE_INT_STAT(stat, statobj, dev);
        WRITE_INT_STAT(stat, statobj, gid);
        WRITE_INT_STAT(stat, statobj, ino);
        WRITE_INT_STAT(stat, statobj, mode);
        WRITE_INT_STAT(stat, statobj, mtime);
        WRITE_INT_STAT(stat, statobj, mtime_ms);
        WRITE_INT_STAT(stat, statobj, mtime_us);
        WRITE_INT_STAT(stat, statobj, mtime_ns);
        WRITE_INT_STAT(stat, statobj, nlink);
        WRITE_INT_STAT(stat, statobj, size);
        WRITE_INT_STAT(stat, statobj, uid);

        WRITE_STR_STAT(stat, statobj, cclock);
        WRITE_STR_STAT(stat, statobj, oclock);

        WRITE_FLOAT_STAT(stat, statobj, ctime_f);
        WRITE_FLOAT_STAT(stat, statobj, mtime_f);

        /* the one we have to do manually because we don't
         * want to use the name "new" */
        json_t *newer = json_object_get(statobj, "new");
        if (newer) {
            stat->newer = json_is_true(newer);
        }
        res->nr++;
    }

    json_t *version = json_object_get(obj, "version");
    JSON_ASSERT(json_is_string, version, "Bad version %s");
    res->version = strdup(json_string_value(version));

    json_t *clock = json_object_get(obj, "clock");
    JSON_ASSERT(json_is_string, clock, "Bad clock %s");
    res->clock = strdup(json_string_value(clock));

    json_t *fresh = json_object_get(obj, "is_fresh_instance");
    JSON_ASSERT(json_is_boolean, fresh, "Bad is_fresh_instance %s");
    res->is_fresh_instance = json_is_true(fresh);

    result = res;
    res = NULL;
done:
    if (res) {
        watchman_free_query_result(res);
    }
    json_decref(obj);
    return result;
}

struct watchman_query *
watchman_query(void)
{
    struct watchman_query *result = calloc(1, sizeof(*result));
    result->sync_timeout = -1;
    return result;
}

void
watchman_free_query(struct watchman_query *query)
{
    if (query->since_is_str) {
        free(query->s.str);
        query->s.str = NULL;
    }
    if (query->nr_suffixes) {
        int i;
        for (i = 0; i < query->nr_suffixes; ++i) {
            free(query->suffixes[i]);
            query->suffixes[i] = NULL;
        }
        free(query->suffixes);
        query->suffixes = NULL;
    }
    if (query->nr_paths) {
        int i;
        for (i = 0; i < query->nr_paths; ++i) {
            free(query->paths[i].path);
            query->paths[i].path = NULL;
        }
        free(query->paths);
        query->paths = NULL;
    }
    free(query);
}

void
watchman_query_add_suffix(struct watchman_query *query, char *suffix)
{
    assert(suffix);
    if (query->cap_suffixes == query->nr_suffixes) {
        if (query->nr_suffixes == 0) {
            query->cap_suffixes = 10;
        } else {
            query->cap_suffixes *= 2;
        }
        int new_size = sizeof(*query->suffixes) * query->cap_suffixes;
        query->suffixes = realloc(query->suffixes, new_size);
    }
    query->suffixes[query->nr_suffixes] = strdup(suffix);
    query->nr_suffixes++;
}

void
watchman_query_add_path(struct watchman_query *query, char *path, int depth)
{
    if (query->cap_paths == query->nr_paths) {
        if (query->nr_paths == 0) {
            query->cap_paths = 10;
        } else {
            query->cap_paths *= 2;
        }
        int new_size = sizeof(*query->paths) * query->cap_paths;
        query->paths = realloc(query->paths, new_size);
    }
    query->paths[query->nr_paths].path = strdup(path);
    query->paths[query->nr_paths].depth = depth;
    query->nr_paths++;
}

void
watchman_query_set_since_oclock(struct watchman_query *query, char *since)
{
    if (query->since_is_str) {
        free(query->s.str);
    }
    query->since_is_str = 1;
    query->s.str = strdup(since);
}

void
watchman_query_set_since_time_t(struct watchman_query *query, time_t since)
{
    if (query->since_is_str) {
        free(query->s.str);
    }
    query->since_is_str = 1;
    query->s.time = since;
}

void
watchman_query_set_fields(struct watchman_query *query, int fields)
{
    query->fields = fields;
}

static json_t *
json_path(struct watchman_pathspec *spec)
{
    if (spec->depth == -1) {
        return json_string(spec->path);
    }

    json_t *obj = json_object();
    json_object_set_new(obj, "depth", json_integer(spec->depth));
    json_object_set_new(obj, "path", json_string(spec->path));
    return obj;
}

struct watchman_query_result *
watchman_do_query(struct watchman_connection *conn,
                  const char *fs_path,
                  const struct watchman_query *query,
                  const struct watchman_expression *expr,
                  struct watchman_error *error)
{
    /* construct the json */
    json_t *json = json_array();
    json_array_append_new(json, json_string("query"));
    json_array_append_new(json, json_string(fs_path));
    json_t *obj = json_object();
    json_object_set_new(obj, "expression", to_json(expr));
    if (query) {
        if (query->fields) {
            json_object_set_new(obj, "fields", fields_to_json(query->fields));
        }

        if (query->s.time) {
            if (query->since_is_str) {
                json_object_set_new(obj, "since", json_string(query->s.str));
            } else {
                json_t *since = json_integer(query->s.time);
                json_object_set_new(obj, "since", since);
            }
        }

        if (query->nr_suffixes) {
            /* Note that even if you have only one suffix,
             * watchman requires this to be an array. */
            int i;
            json_t *suffixes = json_array();
            for (i = 0; i < query->nr_suffixes; ++i) {
                json_array_append_new(suffixes,
                                      json_string(query->suffixes[i]));
            }
            json_object_set_new(obj, "suffix", suffixes);
        }
        if (query->nr_paths) {
            int i;
            json_t *paths = json_array();
            for (i = 0; i < query->nr_paths; ++i) {
                json_array_append_new(paths, json_path(&query->paths[i]));
            }
            json_object_set_new(obj, "path", paths);
        }

        if (query->all) {
            json_object_set_new(obj, "all", json_string("all"));
        }

        if (query->sync_timeout >= 0) {
            json_object_set_new(obj, "sync_timeout",
                                json_integer(query->sync_timeout));
        }
    }
    json_array_append_new(json, obj);

    /* do the query */
    struct watchman_query_result *r = watchman_query_json(conn, json, error);
    json_decref(json);
    return r;
}

void
watchman_free_expression(struct watchman_expression *expr)
{
    int i;
    switch (expr->ty) {
        case WATCHMAN_EXPR_TY_ALLOF:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_ANYOF:
            for (i = 0; i < expr->e.union_expr.nr; ++i) {
                watchman_free_expression(expr->e.union_expr.clauses[i]);
            }
            free(expr->e.union_expr.clauses);
            free(expr);
            break;
        case WATCHMAN_EXPR_TY_NOT:
            watchman_free_expression(expr->e.not_expr.clause);
            free(expr);
            break;
        case WATCHMAN_EXPR_TY_TRUE:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_FALSE:
            /*-fallthrough*/
            /* These are singletons; don't delete them */
            break;
        case WATCHMAN_EXPR_TY_SINCE:
            if (expr->e.since_expr.is_str) {
                free(expr->e.since_expr.t.since);
            }
            free(expr);
            break;
        case WATCHMAN_EXPR_TY_SUFFIX:
            free(expr->e.suffix_expr.suffix);
            free(expr);
            break;
        case WATCHMAN_EXPR_TY_MATCH:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_IMATCH:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_PCRE:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_IPCRE:
            /*-fallthrough*/
            free(expr->e.match_expr.match);
            free(expr);
            break;
        case WATCHMAN_EXPR_TY_NAME:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_INAME:
            for (i = 0; i < expr->e.name_expr.nr; ++i) {
                free(expr->e.name_expr.names[i]);
            }
            free(expr->e.name_expr.names);
            free(expr);
            break;
        case WATCHMAN_EXPR_TY_TYPE:
            free(expr);
            break;
        case WATCHMAN_EXPR_TY_EMPTY:
            /*-fallthrough*/
        case WATCHMAN_EXPR_TY_EXISTS:
            /* These are singletons; don't delete them */
            break;
    }
}

void
watchman_connection_close(struct watchman_connection *conn)
{
    if (!conn->fp) {
        return;
    }
    fclose(conn->fp);
    conn->fp = NULL;
    free(conn);
}

void
watchman_release_error(struct watchman_error *error)
{
    if (error->message) {
        free(error->message);
    }
}

void
watchman_free_watch_list(struct watchman_watch_list *list)
{
    int i;
    for (i = 0; i < list->nr; ++i) {
        free(list->roots[i]);
        list->roots[i] = NULL;
    }
    free(list->roots);
    list->roots = NULL;
    free(list);
}

/* Not a _free_ function, since stats are allocated as a block. */
static void
watchman_release_stat(struct watchman_stat *stat)
{
    if (stat->name) {
        free(stat->name);
        stat->name = NULL;
    }
}

void
watchman_free_query_result(struct watchman_query_result *result)
{
    if (result->version) {
        free(result->version);
        result->version = NULL;
    }
    if (result->clock) {
        free(result->clock);
        result->clock = NULL;
    }
    if (result->stats) {
        int i;
        for (i = 0; i < result->nr; ++i) {
            watchman_release_stat(&(result->stats[i]));
        }
        free(result->stats);
        result->stats = NULL;
    }
    free(result);
}

struct watchman_expression *
watchman_not_expression(struct watchman_expression *expression)
{
    struct watchman_expression *not_expr = alloc_expr(WATCHMAN_EXPR_TY_NOT);
    not_expr->e.not_expr.clause = expression;
    return not_expr;
}

static struct watchman_expression *
watchman_union_expression(enum watchman_expression_type
                          ty, int nr, struct watchman_expression **expressions)
{
    assert(nr);
    assert(expressions);
    size_t sz = sizeof(*expressions);
    struct watchman_expression *result = malloc(sz);
    result->ty = ty;
    result->e.union_expr.nr = nr;
    result->e.union_expr.clauses = malloc(nr * sz);
    memcpy(result->e.union_expr.clauses, expressions, nr * sz);
    return result;
}

struct watchman_expression *
watchman_allof_expression(int nr, struct watchman_expression **expressions)
{
    return watchman_union_expression(WATCHMAN_EXPR_TY_ALLOF, nr, expressions);
}

struct watchman_expression *
watchman_anyof_expression(int nr, struct watchman_expression **expressions)
{
    return watchman_union_expression(WATCHMAN_EXPR_TY_ANYOF, nr, expressions);
}

#define STATIC_EXPR(ty, tylower)                                        \
    static struct watchman_expression ty##_EXPRESSION =                 \
        { WATCHMAN_EXPR_TY_##ty };                                      \
    struct watchman_expression *                                        \
    watchman_##tylower##_expression(void)                               \
    {                                                                   \
        return &ty##_EXPRESSION;                                        \
    }

STATIC_EXPR(EMPTY, empty)
STATIC_EXPR(TRUE, true)
STATIC_EXPR(FALSE, false)
STATIC_EXPR(EXISTS, exists)
#undef STATIC_EXPR

struct watchman_expression *
watchman_suffix_expression(const char *suffix)
{
    assert(suffix);
    struct watchman_expression *expr = alloc_expr(WATCHMAN_EXPR_TY_SUFFIX);
    expr->e.suffix_expr.suffix = strdup(suffix);
    return expr;
}

#define MATCH_EXPR(tyupper, tylower)                                    \
    struct watchman_expression *                                        \
    watchman_##tylower##_expression(const char *match,                  \
                                    enum watchman_basename basename)    \
    {                                                                   \
        assert(match);                                                  \
        struct watchman_expression *expr =                              \
            alloc_expr(WATCHMAN_EXPR_TY_##tyupper);                     \
        expr->e.match_expr.match = strdup(match);                       \
        expr->e.match_expr.basename = basename;                         \
        return expr;                                                    \
    }

MATCH_EXPR(MATCH, match)
MATCH_EXPR(IMATCH, imatch)
MATCH_EXPR(PCRE, pcre)
MATCH_EXPR(IPCRE, ipcre)
#undef MATCH_EXPR

#define NAME_EXPR(tyupper, tylower)                                     \
    struct watchman_expression *                                        \
    watchman_##tylower##_expression(const char *name,                   \
                                    enum watchman_basename basename)    \
    {                                                                   \
        assert(name);                                                   \
        return watchman_##tylower##s_expression(1, &name, basename);    \
    }

NAME_EXPR(NAME, name)
NAME_EXPR(INAME, iname)
#undef NAME_EXPR

#define NAMES_EXPR(tyupper, tylower)                                    \
    struct watchman_expression *                                        \
    watchman_##tylower##s_expression(int nr, const char **names,        \
                                     enum watchman_basename basename)   \
    {                                                                   \
        assert(nr);                                                     \
        assert(names);                                                  \
        struct watchman_expression *result =                            \
            alloc_expr(WATCHMAN_EXPR_TY_##tyupper);                     \
        result->e.name_expr.nr = nr;                                    \
        result->e.name_expr.names = malloc(nr * sizeof(*names));        \
        int i;                                                          \
        for (i = 0; i < nr; ++i) {                                      \
            result->e.name_expr.names[i] = strdup(names[i]);            \
        }                                                               \
        return result;                                                  \
    }

NAMES_EXPR(NAME, name)
NAMES_EXPR(INAME, iname)
#undef NAMES_EXPR

struct watchman_expression *
watchman_type_expression(char c)
{
    struct watchman_expression *result = alloc_expr(WATCHMAN_EXPR_TY_TYPE);
    result->e.type_expr.type = c;
    return result;
}
