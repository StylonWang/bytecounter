#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <getopt.h>
#include <sys/select.h>

struct log_sample {
    unsigned long time_ms;
    unsigned long bytes;

    struct log_sample *prev;
    struct log_sample *next;
};

static struct log_sample *g_head, *g_tail;

static int g_buffer_size = 4*1024;
static int g_to_quit = 0;
static int g_granularity = 100;
static int g_run_time = 0;

static void init_sample_log(void)
{
    g_head = g_tail = NULL;
}

static int add_sample_to_log(unsigned long time_ms, unsigned long bytes)
{
    struct log_sample *newsample = malloc(sizeof(struct log_sample ));
    if(NULL==newsample) return -1;

    newsample->time_ms = time_ms;
    newsample->bytes = bytes;
    newsample->prev = g_tail;
    newsample->next = NULL;

    if(NULL==g_head || NULL==g_tail) { // empty list
        g_head = g_tail = newsample;
    }
    else {
        g_tail->next = newsample;
        g_tail = newsample;
    }

    return 0;
}

static int analyze_sample_and_report(FILE* logf)
{
    unsigned long count = 0;
    unsigned long bytes = 0;
    unsigned long time_unit = 1;
    fprintf(stderr, "Report granularity: %d milli seconds\n", g_granularity); 

    while(g_head) {
        //fprintf(logf, "%ld %ld\n", g_head->time_ms, g_head->bytes);
        //fflush(logf);
        
        bytes += g_head->bytes;
        count++;

        if(time_unit*g_granularity <= g_head->time_ms) {
            
            fprintf(logf, "%ld %ld\n", time_unit*g_granularity, bytes);
            fflush(logf);

            bytes = 0;
            time_unit++;
        }

        g_head = g_head->next;
    }

    fprintf(stderr, "Total %ld samples\n", count);
    return 0;
}

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

static void signal_handler(int signo)
{
    g_to_quit = 1;
    fprintf(stderr, "Signal %d caught\n", signo);
}


int main(int argc, char **argv)
{
	unsigned char *buf;
    int inf, outf; //TODO: remove outf
    FILE *logf = NULL;
    unsigned long total_size = 0;
    struct timeval t2, t_start;
    //fd_set rfd;

    while(1) {
        int c;

        if( -1 == (c = getopt(argc, argv, "?hg:t:s:")) ) break;

        switch(c) {
        case '?':
        case 'h':
            fprintf(stderr, "%s [-g granularity] [-t run-time] -s file\n", argv[0]);
            fprintf(stderr, "-g set granularity of the report in milli seconds\n");
            fprintf(stderr, "-t set maximum time for capture and analyze. Default is forever\n");
            fprintf(stderr, "-s generate time and data size to log file\n");
            fprintf(stderr, "   format is \"time-in-millisecond bytes\" perline\n");
            fprintf(stderr, "\nThis tool calculates bytes flow from stdin\n\n");
            exit(1);
            break;

        case 't':
            g_run_time = atoi(optarg);
            break;

        case 'g':
            g_granularity = atoi(optarg);
            break;

        case 's':
            {
                logf = fopen(optarg, "w+");
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

    fprintf(stderr, "Use buffer %d bytes\n", g_buffer_size);
    fprintf(stderr, "Max run time is %d milli seconds\n", g_run_time);
    
    inf = open("/dev/stdin", O_RDONLY);
    if(-1==inf) {
        fprintf(stderr, "cannot open /dev/stdin for reading\n");
        exit(1);
    }

    outf = open("/dev/stdout", O_WRONLY);
    if(-1==outf) {
        fprintf(stderr, "cannot open /dev/stdout for writing\n");
        exit(1);
    }

    buf = malloc(g_buffer_size);
    if(!buf) {
        fprintf(stderr, "cannot allocate buffer of %d bytes\n", g_buffer_size);
        exit(1);
    }

    signal(SIGINT, signal_handler);

    fprintf(logf, "time-in-ms bytes\n");

    gettimeofday(&t_start, NULL);
    //gettimeofday(&t1, NULL);

    init_sample_log();

    // calculate the byte count every specified milli-second
	while(!g_to_quit) {
        ssize_t sizer;
       // unsigned long time_diff_millisec;
        fd_set rfd;
        struct timeval timeout;
        int ret;

        FD_ZERO(&rfd);
        FD_SET(inf, &rfd);
        timeout.tv_sec = 0;
        timeout.tv_usec = 100*1000; // 100 milli seconds

        ret = select(inf+1, &rfd, NULL, NULL, &timeout);
        if(0==ret || !FD_ISSET(inf, &rfd)) {
            sizer = 0;
        }
        else {
            sizer = read(inf, buf, g_buffer_size);
        }
        gettimeofday(&t2, NULL);

        unsigned long time_diff_from_start = get_time_interval_in_ms(&t_start, &t2);
        add_sample_to_log(time_diff_from_start, sizer);

        total_size += (unsigned long)sizer;

        //time_diff_millisec = get_time_interval_in_ms(&t1, &t2);

        fprintf(stderr, "dbg: %ld %ld\n", time_diff_from_start, sizer);

        if(g_run_time!=0 && g_run_time*1000 < time_diff_from_start) break;

	} // end of while loop

	fprintf(stderr, "Total %ld bytes read\n", total_size);
    close(inf);
    close(outf);

    analyze_sample_and_report(logf);
    fclose(logf);

	return 0;
}
