/* Host tests: load the stock data blob named by $FB200_STOCK_BLOB (built by
 * src/fb200/stockdata.py from the user's .mr) and check it with the
 * firmware's own stock_check(). Without the variable g_stock stays NULL. */
#ifndef FB200_TESTS_STOCK_HOST_H
#define FB200_TESTS_STOCK_HOST_H
#include <stdio.h>
#include <stdlib.h>
#include "dsp/stock_data.h"

static void stock_from_env(void)
{
    const char *path = getenv("FB200_STOCK_BLOB");
    if (!path) return;
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(3); }
    static stock_data_t blob;
    size_t n = fread(&blob, 1, sizeof blob, f);
    fclose(f);
    int r = stock_check(&blob, (uint32_t)n);
    if (r) { fprintf(stderr, "%s: stock data %s\n", path, stock_error(r)); exit(3); }
    g_stock = &blob;
}
#endif
