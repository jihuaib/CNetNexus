/**
 * @file   if_api.c
 * @brief  IF 模块对外 API 实现
 * @author jhb
 * @date   2026/02/14
 */
#include <glib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "dev.h"
#include "errcode.h"
#include "if_main.h"
#include "if_map.h"
#include "ipc.h"
#include "path_utils.h"
