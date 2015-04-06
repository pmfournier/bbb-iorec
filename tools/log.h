#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#define ERROR(msg, args...) fprintf(stderr, "error: " msg "\n", ##args)

#endif /* LOG_H */
