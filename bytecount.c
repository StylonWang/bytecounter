#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <getopt.h>

int buffer_size = 40*1024;
int to_quit = 0;
int show_in_mbit = 0;
int warn_low_mark = 0, warn_high_mark = 1;

unsigned long calculate_byte_rate(struct timeval *pt1, struct timeval *pt2, 
                    unsigned long *temp_size, int duration_sec)
{
    unsigned long average_bytes = 0;
    unsigned long mili_sec;

    gettimeofday(pt2, NULL);
    if(pt2->tv_sec < pt1->tv_sec+duration_sec) {
        goto out;
    }

    mili_sec = (pt2->tv_sec-pt1->tv_sec)*1000;            

    if(pt2->tv_usec>=pt1->tv_usec) {
        mili_sec += (pt2->tv_usec-pt1->tv_usec)/1000;
    }
    else {
        mili_sec -= 1000;
        mili_sec += (pt2->tv_usec+1000000-pt1->tv_usec)/1000;
    }

#if 0
    fprintf(stderr, "%ld.%ld ~ %ld.%ld, %ld, %ld\n",
            (unsigned long)pt1->tv_sec, (unsigned long)pt1->tv_usec,
            (unsigned long)pt2->tv_sec, (unsigned long)pt2->tv_usec,
            mili_sec, *temp_size);
#endif
    if(!mili_sec) {
        fprintf(stderr, "internal exception!\n");
        goto out;
    }

    average_bytes = (*temp_size*1000) / mili_sec;
//    fprintf(stderr, "Avg. %ld bytes/sec\n", average_bytes);

    // reset
    *temp_size = 0;
    *pt1 = *pt2;

out:
        return average_bytes;
}

void signal_handler(int signo)
{
    to_quit = 1;
}

int main(int argc, char **argv)
{
	unsigned char *buf;
    FILE *inf;
    FILE *outf;
	unsigned long total_size = 0;
    unsigned long temp_size = 0;
    struct timeval t1, t2;
    int counter=0;

    while(1) {
        int c;

        if( -1 == (c = getopt(argc, argv, "?hmb:w:")) ) break;

        switch(c) {
        case '?':
        case 'h':
            fprintf(stderr, "%s [-b buffer_size] [-m] [-w low:high] \n", argv[0]);
            fprintf(stderr, "buffer size default %d bytes\n", buffer_size);
            fprintf(stderr, "-m show in mega-bits\n");
            fprintf(stderr, "-w post warning if stream bit rate is out of range.\n");
            fprintf(stderr, "\nThis tool calculates bytes flow from stdin and copy data to stdout\n\n");
            exit(1);
            break;

        case 'b':
            buffer_size = atoi(optarg);
            break;

        case 'm':
            show_in_mbit = 1;
            break;

        case 'w':
            {
                char *c = strchr(optarg, ':');
                char *c2 = 0;
                if(NULL==c) break;
                c2 = c+1;
                *c = 0;
                warn_low_mark = atoi(optarg);
                warn_high_mark = atoi(c2);
            }

        }
    }

    fprintf(stderr, "Use buffer %d bytes\n", buffer_size);
    
    if(warn_low_mark && warn_high_mark) {
        fprintf(stderr, "Warning low~high is %d~%d\n",
                warn_low_mark, warn_high_mark);
    }

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

    gettimeofday(&t1, NULL);

	while(!to_quit) {
        size_t sizer, sizew;
        unsigned long average_bytes;

        sizer = fread(buf, 1, buffer_size, inf);

        total_size += (unsigned long)sizer;
        temp_size += (unsigned long)sizer;

        average_bytes = calculate_byte_rate(&t1, &t2, &temp_size, 2);
        if(0==average_bytes) { // not time to calculate yet
            continue;
        }
        else if(counter++<3) { // ignore the initial numbers for more correct results
            //fprintf(stderr, "Ignore calc. %d\n", counter);
            continue;
        }
        else if( show_in_mbit ) {
                double mbits = ((double)average_bytes*8)/1024/1024;
                fprintf(stderr, "Avg. %.2f Mbits/sec\n", mbits);
                if(warn_low_mark && warn_high_mark && 
                   (mbits<warn_low_mark || mbits>warn_high_mark)
                  ) {
                    fprintf(stderr, "WARNING: bit rate %.2f Mbits out of range, after %ld total bytes\n",
                            mbits, total_size);
                    break; //quit
                }
        }
        else {
                fprintf(stderr, "Avg. %ld bytes/sec\n", average_bytes);
                if(warn_low_mark && warn_high_mark && 
                   (average_bytes<warn_low_mark || average_bytes>warn_high_mark)
                  ) {
                    fprintf(stderr, "WARNING: bit rate %ld MBytes out of range, after %ld total bytes\n",
                            average_bytes, total_size);
                    break; //quit
                }
        }
#if 0
        // calculate byte flow
        gettimeofday(&t2, NULL);
        if(t2.tv_sec >= t1.tv_sec+2) {
            unsigned long mili_sec = (t2.tv_sec-t1.tv_sec)*1000;            

            if(t2.tv_usec>=t1.tv_usec) {
                mili_sec += (t2.tv_usec-t1.tv_usec)/1000;
            }
            else {
                mili_sec -= 1000;
                mili_sec += (t2.tv_usec+1000000-t1.tv_usec)/1000;
            }

#if 0
            fprintf(stderr, "%ld.%ld ~ %ld.%ld, %ld, %ld\n",
                    (unsigned long)t1.tv_sec, (unsigned long)t1.tv_usec,
                    (unsigned long)t2.tv_sec, (unsigned long)t2.tv_usec,
                    mili_sec, temp_size);
#endif
            if(!mili_sec) {
                fprintf(stderr, "internal exception!\n");
                continue;
            }

            if( show_in_mbit ) {
                average_bytes = (temp_size*1000) / mili_sec;
                double mbits = ((double)average_bytes*8)/1024/1024;
                fprintf(stderr, "Avg. %.2f Mbits/sec\n", mbits);
                if(warn_low_mark && warn_high_mark && 
                   (mbits<warn_low_mark || mbits>warn_high_mark)
                  ) {
                    fprintf(stderr, "WARNING: bit rate %.2f Mbits out of range\n",
                            mbits);
                    break; //quit
                }
            }
            else {
                average_bytes = (temp_size*1000) / mili_sec;
                fprintf(stderr, "Avg. %ld bytes/sec\n", average_bytes);
                if(warn_low_mark && warn_high_mark && 
                   (average_bytes<warn_low_mark || average_bytes>warn_high_mark)
                  ) {
                    fprintf(stderr, "WARNING: bit rate %ld MBytes out of range\n",
                            average_bytes);
                    break; //quit
                }
            }

            // reset
            temp_size = 0;
            t1 = t2;
        }
#endif
        
        sizew = fwrite(buf, 1, sizer, outf);
	} // end of while loop

	fprintf(stderr, "Total %ld bytes read\n", total_size);
	return 0;
}
