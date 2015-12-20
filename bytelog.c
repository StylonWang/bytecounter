#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <getopt.h>
#include <signal.h>

int buffer_size = 4*1024;
int to_quit = 0;

void signal_handler(int signo)
{
    to_quit = 1;
}

unsigned long get_time_interval_in_ms(const struct timeval *pt1,
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
	unsigned char *buf;
    FILE *inf;
    FILE *outf;
    FILE *logf = NULL;
	unsigned long interval_size = 0;
    unsigned long total_size = 0;
    struct timeval t1, t2, t_start;

    while(1) {
        int c;

        if( -1 == (c = getopt(argc, argv, "?hb:s:")) ) break;

        switch(c) {
        case '?':
        case 'h':
            fprintf(stderr, "%s -s file\n", argv[0]);
            fprintf(stderr, "-s generate time and data size to log file\n");
            fprintf(stderr, "   format is \"time-in-millisecond bytes\" perline\n");
            fprintf(stderr, "\nThis tool calculates bytes flow from stdin and copy data to stdout\n\n");
            exit(1);
            break;

        case 'b':
            buffer_size = atoi(optarg);
            break;

        case 's':
            {
                logf = fopen(optarg, "w");
                if(NULL==logf) {
                    fprintf(stderr, "cannot open '%s' for writing: %s\n",
                            optarg, strerror(errno));
                    exit(1);
                }
                break;
            }

        }
    }

    if(NULL==logf) {
        fprintf(stderr, "Please specify path to log file via \"-s\" option.\n");
        exit(1);
    }

    fprintf(stderr, "Use buffer %d bytes\n", buffer_size);
    
    inf = fopen("/dev/stdin", "r");
    if(!inf) {
        fprintf(stderr, "cannot open /dev/stdin for reading\n");
        exit(1);
    }

    outf = fopen("/dev/stdout", "w");
    if(!outf) {
        fprintf(stderr, "cannot open /dev/stdout for writing\n");
        exit(1);
    }

    buf = malloc(buffer_size);
    if(!buf) {
        fprintf(stderr, "cannot allocate buffer of %d bytes\n", buffer_size);
        exit(1);
    }

    signal(SIGINT, signal_handler);

    fprintf(logf, "time-in-ms bytes\n");

    gettimeofday(&t_start, NULL);
    gettimeofday(&t1, NULL);

    // calculate the byte count every specified milli-second
	while(!to_quit) {
        size_t sizer, sizew;
        unsigned long time_diff_millisec;

        sizer = fread(buf, 1, buffer_size, inf);
        gettimeofday(&t2, NULL);

        interval_size += (unsigned long)sizer;
        total_size += (unsigned long)sizer;


        time_diff_millisec = get_time_interval_in_ms(&t1, &t2);

        if( time_diff_millisec >= 200 ) {

            unsigned long time_diff_from_start = get_time_interval_in_ms(&t_start, &t2);

            fprintf(logf, "%ld %ld\n", time_diff_from_start, interval_size);
            fflush(logf);

            // reset
            interval_size = 0;
            gettimeofday(&t1, NULL);
        }

        sizew = fwrite(buf, 1, sizer, outf);
	} // end of while loop

	fprintf(stderr, "Total %ld bytes read\n", total_size);
    fclose(inf);
    fclose(outf);
    fclose(logf);
	return 0;
}
