#include <inc/time.h>
#include <inc/lib.h>
void umain(int argc, char** argv)
{
    struct tm tm;
    sys_get_time(&tm);
    cprintf("The time is now :%d, %d, %d, %d, %d\n", tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min);
}