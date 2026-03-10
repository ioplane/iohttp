/**
 * @file ioh_route_meta.c
 * @brief Route metadata utility functions.
 */

#include "router/ioh_route_meta.h"

const char *ioh_param_in_name(ioh_param_in_t in)
{
    switch (in) {
    case IOH_PARAM_PATH:
        return "path";
    case IOH_PARAM_QUERY:
        return "query";
    case IOH_PARAM_HEADER:
        return "header";
    case IOH_PARAM_COOKIE:
        return "cookie";
    default:
        return "unknown";
    }
}
