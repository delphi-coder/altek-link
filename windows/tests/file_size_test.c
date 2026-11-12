#include "json.h"
#include <stdio.h>
#include <string.h>
static int check(const char *json, int expected_ok, uint64_t expected)
{
    struct json j;
    uint64_t value = 0;
    int ok = json_parse(&j, json) && json_u64(&j, "size", &value);
    if (ok != expected_ok || (ok && value != expected))
    {
        printf("FAIL %s\n", json);
        return 0;
    }
    return 1;
}
int main(void)
{
    char text[100];
    uint64_t v = UINT64_C(128849018880);
    if (!check("{\"size\":128849018880}", 1, v) ||
        !check("{\"size\":4294967296}", 1, UINT64_C(4294967296)) ||
        !check("{\"size\":0}", 1, 0) ||
        !check("{\"size\":-1}", 0, 0) ||
        !check("{\"size\":18446744073709551616}", 0, 0) ||
        !check("{\"size\":\"123\"}", 0, 0) ||
        !check("{}", 0, 0)) return 1;
    _snprintf(text, sizeof(text), "{\"size\":%I64u}", v);
    if (strcmp(text, "{\"size\":128849018880}")) { printf("FAIL formatting: %s\n", text); return 1; }
    puts("PASS 64-bit JSON size and Windows wire formatting");
    return 0;
}
