#include "include.h"

#if (PG_VERSION_NUM >= 140000)
#include <postgres.140000.c>
#elif (PG_VERSION_NUM >= 130000)
#include <postgres.130000.c>
#elif (PG_VERSION_NUM >= 120000)
#include <postgres.120000.c>
#elif (PG_VERSION_NUM >= 110000)
#include <postgres.110000.c>
#endif
