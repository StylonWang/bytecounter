#include <stdlib.h>
#include <time.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>

// random data generator
// generate random number of bytes and send to stdout between random number 
// of interval

static unsigned long get_time_interval_in_ms(const struct timeval *pt1,
                        const struct timeval *pt2)
{
    unsigned long diff_in_ms;

    diff_in_ms = (pt2->tv_sec - pt1->tv_sec)*1000;
    
    if(pt2->tv_usec < pt1->tv_usec) {
        diff_in_ms -= 1000; // borrow 1 second to pt2
        diff_in_ms += (pt2->tv_usec+1000000 - pt1->tv_usec)/1000;
    }
    else {
        diff_in_ms += (pt2->tv_usec-pt1->tv_usec)/1000;
    }

    return diff_in_ms;
}

int main(int argc, char **argv)
{
    struct timeval t1, t2;
    char buf[50*1024];
    FILE *logf = NULL;

    logf = fopen("generator.log", "w+");
    if(NULL==logf) {
        fprintf(stderr, "Cannot open log file\n");
        exit(1);
    }

    gettimeofday(&t1, NULL);

    while(1) {
        unsigned long buf_size = rand() % sizeof(buf);
        int sleep_ms = 200; //rand() % 50;
        unsigned long diff_ms;

        write(1, buf, buf_size);
        usleep(sleep_ms*1000);

        gettimeofday(&t2, NULL);
        diff_ms = get_time_interval_in_ms(&t1, &t2);
        fprintf(logf, "%ld %ld\n", diff_ms, 
                buf_size);
        fflush(logf);

        if(diff_ms >= 20*1000) break;
    }

    fclose(logf);
    return 0;
}


