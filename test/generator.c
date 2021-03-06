#include <stdlib.h>
#include <time.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>

#define MODULE "[generator]"

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
    unsigned char buf[100*1024];
    FILE *logf = NULL;
    unsigned char counter = 0;

    srandom(time(NULL));

    logf = fopen("generator.log", "w+");
    if(NULL==logf) {
        fprintf(stderr, "%s Cannot open log file\n", MODULE);
        exit(1);
    }

    gettimeofday(&t1, NULL);

    while(1) {
        unsigned long buf_size = random() % sizeof(buf);
        int sleep_ms = 100; //rand() % 50;
        unsigned long diff_ms;
        int i=0;

        for(i=0; i<buf_size; ++i) {
            buf[i] = counter++;
        }

        write(1, buf, buf_size);
        usleep(sleep_ms*1000);

        gettimeofday(&t2, NULL);
        diff_ms = get_time_interval_in_ms(&t1, &t2);
        fprintf(logf, "%ld %d %ld\n", diff_ms, sleep_ms, buf_size);
        fflush(logf);

        //if(diff_ms >= 20*1000) break;
    }

    fclose(logf);
    return 0;
}


