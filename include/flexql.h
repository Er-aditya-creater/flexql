/*
 * include/flexql.h  —  FlexQL Public C API
 * ==========================================
 * FlexQL is an opaque pointer; callers never see its internals.
 *
 *   FlexQL *db;
 *   flexql_open("127.0.0.1", 9000, &db);
 *   char *err = NULL;
 *   flexql_exec(db, "SELECT * FROM T;", my_cb, NULL, &err);
 *   flexql_close(db);
 */
#ifndef FLEXQL_H
#define FLEXQL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FlexQL FlexQL;   /* opaque — internals in src/client/client.cpp */

#define FLEXQL_OK    0
#define FLEXQL_ERROR 1

int  flexql_open (const char *host, int port, FlexQL **db);
int  flexql_close(FlexQL *db);
int  flexql_exec (FlexQL *db,
                  const char *sql,
                  int (*callback)(void *data, int columnCount,
                                  char **values, char **columnNames),
                  void  *arg,
                  char **errmsg);
void flexql_free (void *ptr);

#ifdef __cplusplus
}
#endif
#endif /* FLEXQL_H */
