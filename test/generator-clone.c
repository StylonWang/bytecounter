#include <stdlib.h>
#include <time.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>

#define MODULE "[generator-clone]"

// turn on/off debug message
#if 1
#define dbg_print(fmt, args...)  \
        do {\
            fprintf(stderr, "%s " fmt, MODULE, ##args);\
        } while(0)
#else
    #define dbg_print(fmt, args...) do {} while(0)
#endif

// Use random data samples from logs from generator and reproduce the 
// same data write-out pattern.
//

struct sample {
    unsigned long diff_ms;
    int sleep_ms;
    unsigned long buf_size; 
    
    struct sample *next;
} *g_sample_list_head = NULL, *g_sample_list_tail = NULL;

int main(int argc, char **argv)
{
    unsigned char buf[100*1024]; 

    FILE *logf = NULL;
    unsigned char counter = 0;

    if(argc<2) {
        fprintf(stderr, "usage: %s generator.log\n", argv[0]);
        exit(1);
    }

    logf = fopen(argv[1], "r");
    if(NULL==logf) {
        dbg_print("Cannot open log file\n");
        exit(1);
    }

    // read generator.log and save into a list
    do {
        struct sample *p = malloc(sizeof(*p));

        if(!p) {
            dbg_print("malloc failed\n");
            exit(1);
        }

        fscanf(logf, "%ld %d %ld\n", &p->diff_ms, &p->sleep_ms, &p->buf_size);
        p->next = NULL;

        if(p->buf_size > sizeof(buf)) {
            dbg_print("log file has larger buffer %ld than we can have %ld\n",
                    p->buf_size, sizeof(buf));
            exit(1);
        }

        dbg_print("%ld %d %ld\n", p->diff_ms, p->sleep_ms,
                p->buf_size);

        // first sample in list
        if(!g_sample_list_head) {
            g_sample_list_head = p;
            g_sample_list_tail = p;
        }
        else {
            g_sample_list_tail->next = p;
            g_sample_list_tail = p;
        }

    } while(!feof(logf));
    fclose(logf);

    // write+sleep according to the sample list
    while(g_sample_list_head) {
        int i=0;
        unsigned long buf_size = 0;
        
        buf_size = g_sample_list_head->buf_size;

        for(i=0; i<buf_size; ++i) {
            buf[i] = counter++;
        }

        write(1, buf, buf_size);
        usleep(g_sample_list_head->sleep_ms *1000);

        g_sample_list_head = g_sample_list_head->next;
    }

    dbg_print("quit\n");
    exit(0);
}


