#ifndef ALTEK_JSON_H
#define ALTEK_JSON_H
#include <stddef.h>
struct json_token
{
    int start, end, next, type;
};
struct json
{
    const char *s;
    struct json_token tokens[256];
    int count, valid;
};
int json_parse(struct json *j, const char *s);
int json_get(struct json *j, const char *key, char *out, size_t cap);
long json_num(struct json *j, const char *key, long fallback);
int json_bool(struct json *j, const char *key);
char *json_quote(const char *text);
#endif
