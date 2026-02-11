#ifndef ERRORS_H
#define ERRORS_H

void exodus_error(const char* format, ...);
void exodus_print(const char* format, ...); // For standard output if needed, but error is main focus

#endif
