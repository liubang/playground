//=====================================================================
//
// preprocessor.h -
//
// Created by liubang on 2023/06/08 15:42
// Last Modified: 2023/06/08 15:42
//
//=====================================================================

#pragma once

#define PG_CONCATENATE_IMPL(s1, s2) s1##s2
#define PG_CONCATENATE(s1, s2) PG_CONCATENATE_IMPL(s1, s2)

#ifdef __COUNTER__
#define PG_ANONYMOUS_VARIABLE(str) PG_CONCATENATE(str, __COUNTER__)
#else
#define PG_ANONYMOUS_VARIABLE(str) PG_CONCATENATE(str, __LINE__)
#endif
