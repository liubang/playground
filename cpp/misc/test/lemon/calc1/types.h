#pragma once

#include <stdlib.h>

struct Token {
  int value;
  const char *token;
};

extern void *ParseAlloc(void *(*)(size_t));
extern void Parse(void *, int, struct Token *);
void ParseFree(void *, void (*)(void *));
