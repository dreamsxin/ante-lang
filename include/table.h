#ifndef TABLE_H
#define TABLE_H

#include <stdlib.h>
#include "types.h"

typedef struct{
    Variable *table;
    unsigned int size;
}VarTable;

void varTable_add();
void varTable_remove();
void varTable_free();

#endif