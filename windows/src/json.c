#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
static void space(struct json *j, int *p)
{
    while (j->s[*p] && isspace((unsigned char)j->s[*p]))
    {
        (*p)++;
    }
}
static int value(struct json *j, int *p, int depth)
{
    int i, close, x;
    if (depth > 16 || j->count >= 256)
    {
        return -1;
    }
    space(j, p);
    i = j->count++;
    j->tokens[i].start = *p;
    j->tokens[i].type = j->s[*p];
    if (j->s[*p] == '"')
    {
        (*p)++;
        while (j->s[*p] && j->s[*p] != '"')
        {
            if ((unsigned char)j->s[*p] < 32)
            {
                return -1;
            }
            if (j->s[*p] == '\\')
            {
                (*p)++;
                if (!strchr("\"\\/bfnrtu", j->s[*p]) || !j->s[*p])
                {
                    return -1;
                }
                if (j->s[*p] == 'u')
                {
                    for (x = 0; x < 4; x++)
                    {
                        (*p)++;
                        if (!isxdigit((unsigned char)j->s[*p]))
                        {
                            return -1;
                        }
                    }
                }
            }
            (*p)++;
        }
        if (j->s[*p] != '"')
        {
            return -1;
        }
        (*p)++;
    }
    else if (j->s[*p] == '{' || j->s[*p] == '[')
    {
        close = j->s[*p] == '{' ? '}' : ']';
        (*p)++;
        space(j, p);
        while (j->s[*p] != close)
        {
            if (close == '}')
            {
                if (j->s[*p] != '"' || value(j, p, depth + 1) < 0)
                {
                    return -1;
                }
                space(j, p);
                if (j->s[(*p)++] != ':')
                {
                    return -1;
                }
            }
            if (value(j, p, depth + 1) < 0)
            {
                return -1;
            }
            space(j, p);
            if (j->s[*p] == close)
            {
                break;
            }
            if (j->s[(*p)++] != ',')
            {
                return -1;
            }
            space(j, p);
            if (j->s[*p] == close)
            {
                return -1;
            }
        }
        (*p)++;
    }
    else
    {
        x = *p;
        while (j->s[*p] && !strchr(" \t\r\n,]}", j->s[*p]))
        {
            (*p)++;
        }
        if (x == *p)
        {
            return -1;
        }
        if (!((*p - x == 4 && (!memcmp(j->s + x, "true", 4) || !memcmp(j->s + x, "null", 4))) ||
              (*p - x == 5 && !memcmp(j->s + x, "false", 5))))
        {
            int q = x;
            if (j->s[q] == '-')
            {
                q++;
            }
            if (j->s[q] == '0')
            {
                q++;
            }
            else
            {
                if (j->s[q] < '1' || j->s[q] > '9')
                {
                    return -1;
                }
                while (j->s[q] >= '0' && j->s[q] <= '9')
                {
                    q++;
                }
            }
            if (j->s[q] == '.')
            {
                q++;
                if (j->s[q] < '0' || j->s[q] > '9')
                {
                    return -1;
                }
                while (j->s[q] >= '0' && j->s[q] <= '9')
                {
                    q++;
                }
            }
            if (j->s[q] == 'e' || j->s[q] == 'E')
            {
                q++;
                if (j->s[q] == '+' || j->s[q] == '-')
                {
                    q++;
                }
                if (j->s[q] < '0' || j->s[q] > '9')
                {
                    return -1;
                }
                while (j->s[q] >= '0' && j->s[q] <= '9')
                {
                    q++;
                }
            }
            if (q != *p)
            {
                return -1;
            }
        }
    }
    j->tokens[i].end = *p;
    j->tokens[i].next = j->count;
    return i;
}
int json_parse(struct json *j, const char *s)
{
    int p = 0;
    memset(j, 0, sizeof(*j));
    j->s = s;
    if (!s || value(j, &p, 0) != 0 || j->tokens[0].type != '{')
    {
        return 0;
    }
    space(j, &p);
    j->valid = s[p] == 0;
    return j->valid;
}
static int find(struct json *j, const char *key)
{
    int i = 1, n = (int)strlen(key);
    if (!j->valid)
    {
        return -1;
    }
    while (i < j->count)
    {
        if (j->tokens[i].type == '"' && j->tokens[i].end - j->tokens[i].start == n + 2 &&
            !memcmp(j->s + j->tokens[i].start + 1, key, n))
        {
            return i + 1;
        }
        if (i + 1 >= j->count || j->tokens[i + 1].next <= i)
        {
            return -1;
        }
        i = j->tokens[i + 1].next;
    }
    return -1;
}
static unsigned int hex4(const char *s)
{
    unsigned int v = 0;
    int i;
    for (i = 0; i < 4; i++)
    {
        v = (v << 4) + (s[i] <= '9' ? s[i] - '0' : (s[i] | 32) - 'a' + 10);
    }
    return v;
}
int json_get(struct json *j, const char *key, char *out, size_t cap)
{
    int i = find(j, key), p, e, k, n;
    unsigned int c;
    unsigned char b[4];
    size_t at = 0;
    if (!cap)
    {
        return 0;
    }
    out[0] = 0;
    if (i < 0 || j->tokens[i].type != '"')
    {
        return 0;
    }
    p = j->tokens[i].start + 1;
    e = j->tokens[i].end - 1;
    while (p < e)
    {
        c = (unsigned char)j->s[p++];
        n = 1;
        b[0] = (unsigned char)c;
        if (c == '\\')
        {
            c = j->s[p++];
            if (c == 'u')
            {
                c = hex4(j->s + p);
                p += 4;
                if (c >= 0xd800 && c <= 0xdbff)
                {
                    unsigned int low;
                    if (p + 6 > e || j->s[p] != '\\' || j->s[p + 1] != 'u')
                    {
                        return 0;
                    }
                    low = hex4(j->s + p + 2);
                    if (low < 0xdc00 || low > 0xdfff)
                    {
                        return 0;
                    }
                    c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                    p += 6;
                }
                else if (c >= 0xdc00 && c <= 0xdfff)
                {
                    return 0;
                }
                if (!c)
                {
                    return 0;
                }
                if (c < 128)
                {
                    b[0] = (unsigned char)c;
                }
                else if (c < 2048)
                {
                    n = 2;
                    b[0] = 192 | (c >> 6);
                    b[1] = 128 | (c & 63);
                }
                else if (c < 65536)
                {
                    n = 3;
                    b[0] = 224 | (c >> 12);
                    b[1] = 128 | ((c >> 6) & 63);
                    b[2] = 128 | (c & 63);
                }
                else
                {
                    n = 4;
                    b[0] = 240 | (c >> 18);
                    b[1] = 128 | ((c >> 12) & 63);
                    b[2] = 128 | ((c >> 6) & 63);
                    b[3] = 128 | (c & 63);
                }
            }
            else
            {
                switch (c)
                {
                case 'n':
                    c = '\n';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 't':
                    c = '\t';
                    break;
                case 'b':
                    c = '\b';
                    break;
                case 'f':
                    c = '\f';
                    break;
                }
                b[0] = (unsigned char)c;
            }
        }
        if (at + n >= cap)
        {
            out[0] = 0;
            return 0;
        }
        for (k = 0; k < n; k++)
        {
            out[at++] = (char)b[k];
        }
    }
    out[at] = 0;
    return 1;
}
int json_u64(struct json *j, const char *key, uint64_t *out)
{
    int i = find(j, key), k;
    uint64_t v = 0, digit;
    if (i < 0 || j->tokens[i].start >= j->tokens[i].end) return 0;
    for (k = j->tokens[i].start; k < j->tokens[i].end; k++)
    {
        if (j->s[k] < '0' || j->s[k] > '9') return 0;
        digit = (unsigned)(j->s[k] - '0');
        if (v > (UINT64_MAX - digit) / 10) return 0;
        v = v * 10 + digit;
    }
    *out = v;
    return 1;
}
long json_num(struct json *j, const char *key, long fallback)
{
    int i = find(j, key);
    return i < 0 ? fallback : strtol(j->s + j->tokens[i].start, NULL, 10);
}
int json_bool(struct json *j, const char *key)
{
    int i = find(j, key);
    return i >= 0 && j->tokens[i].end - j->tokens[i].start == 4 &&
           !memcmp(j->s + j->tokens[i].start, "true", 4);
}
char *json_quote(const char *text)
{
    size_t i, n = strlen(text), p = 0;
    const char h[] = "0123456789abcdef";
    char *out = (char *)malloc(n * 6 + 3);
    if (!out)
    {
        return NULL;
    }
    out[p++] = '"';
    for (i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\')
        {
            out[p++] = '\\';
            out[p++] = c;
        }
        else if (c < 32)
        {
            out[p++] = '\\';
            out[p++] = 'u';
            out[p++] = '0';
            out[p++] = '0';
            out[p++] = h[c >> 4];
            out[p++] = h[c & 15];
        }
        else
        {
            out[p++] = c;
        }
    }
    out[p++] = '"';
    out[p] = 0;
    return out;
}
