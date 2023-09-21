//=====================================================================
//
// preprocessor.h -
//
// Created by liubang on 2023/06/08 15:42
// Last Modified: 2023/06/08 15:42
//
//=====================================================================

#pragma once

#define PL_CONCATENATE_IMPL(s1, s2) s1##s2
#define PL_CONCATENATE(s1, s2)      PL_CONCATENATE_IMPL(s1, s2)

#ifdef __COUNTER__
#define PL_ANONYMOUS_VARIABLE(str) PL_CONCATENATE(str, __COUNTER__)
#else
#define PL_ANONYMOUS_VARIABLE(str) PL_CONCATENATE(str, __LINE__)
#endif
