#include "Runtime.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

// function

#define F(x) \
  void* rv32_##x = x;

F(abort)
F(abs)
F(acos)
F(atan)
F(atexit)
F(atoi)
F(atof)
F(bcopy)
F(clock)
F(close)
F(cos)
F(exit)
F(exp)
F(fclose)
F(feof)
F(ferror)
F(fflush)
F(fgetc)
F(fgetpos)
F(fgets)
F(fopen)
F(free)
F(fprintf)
F(fputc)
F(fread)
F(fscanf)
F(fseek)
F(ftell)
F(fwrite)
F(getc)
F(malloc)
F(memchr)
F(memcpy)
F(memset)
F(perror)
F(pow)
F(printf)
F(putchar)
F(puts)
F(rand)
F(read)
F(realloc)
F(sin)
F(sleep)
F(sqrt)
F(sqrtf)
F(srand)
F(sscanf)
F(strchr)
F(strlen)
F(strncmp)
F(strtod)
F(strtol)
F(tolower)
F(toupper)
F(usleep)
F(write)

F(_IO_getc)
F(_IO_putc)
F(__ctype_tolower_loc)
F(__ctype_toupper_loc)

// GNU extensions (used by gcc -O3)
void sincos(double x, double* sin, double* cos);
F(sincos)

// soft float

F(sbt__extenddftf2)
F(sbt__trunctfdf2)
F(sbt__addtf3)
F(sbt__subtf3)
F(sbt__multf3)
F(sbt__divtf3)
F(sbt__lttf2)

// runtime

F(sbt_printf_d)

// data

#define D(x) \
  void* rv32_##x = NULL;

#define DI(x) \
  rv32_##x = x;

D(stdin)
D(stderr)
D(stdout)

// dummy function to be able to reference non compile time stuff
void set_non_const()
{
    DI(stdin)
    DI(stderr)
    DI(stdout)
}
