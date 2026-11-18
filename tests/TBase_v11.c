/* Implementation of the installed TBase, version 1.1. */
#include "TBase.h"

#include <stdio.h>
#include <stdlib.h>

void TBase__wpInitData(wpo_object *self)
{
    TBase_parent_wpInitData(self);
    TBase_data(self)->b = 42;
    for (int i = 0; i < 40; i++) TBase_data(self)->pad[i] = (char)0xAB;
}

int64_t TBase__tGetA(wpo_object *self) { return TBase_data(self)->a; }
void TBase__tSetA(wpo_object *self, int64_t v) { TBase_data(self)->a = (int)v; }
int64_t TBase__tGetB(wpo_object *self) { return TBase_data(self)->b; }

char *TBase__tDescribe(wpo_object *self)
{
    char *s = malloc(64);
    if (s) snprintf(s, 64, "base a=%d b=%d", TBase_data(self)->a, TBase_data(self)->b);
    return s;
}
