#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>

#define MODULE "[smoother]"

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

// ==========================================================================
// Start of smooth buffering

struct buffer_node {
    char *buffer;
    size_t start;
    size_t nbyte;

    struct buffer_node *prev;
    struct buffer_node *next;
};

// pointer to queue head (incoming) and tail (outgoing)
static struct buffer_node *g_queue_head=NULL, *g_queue_tail=NULL;

// When buffer level is above g_buffer_max_level, do whatever we can to reduce 
// to "what"(?) level.
#define BUFFER_MAX (50*1024)
static const unsigned long g_buffer_max_level = BUFFER_MAX; 
// Until buffer level reaches g_buffer_start_level, we do not write out
// data, instead we do: 1. measure incoming rate. 2. push data into queue
static unsigned long g_buffer_start_level = BUFFER_MAX/2;
// When buffer level reaches beyond g_buffer_threshold_high, we increase
// consumption speed.
static unsigned long g_buffer_threshold_high = BUFFER_MAX *8 /10; // 80%
// When buffer level drops below g_buffer_threshold_low, we decrease
// consumption speed.
static unsigned long g_buffer_threshold_low = BUFFER_MAX *3 /10; // 30%
// current buffer level (water level)
static unsigned long g_buffer_curr_level = 0;
// the constant consumption we try to achieve
static unsigned long g_write_byte_rate = 0;
static unsigned long g_first_write_byte_rate = 0;
// g_write_byte_rate will directly affect g_write_interval_ms and g_write_chunk_bytes
static unsigned long g_write_interval_ms = 0;
static unsigned long g_write_chunk_bytes = 0;
static unsigned long g_write_clock = 0;

static int g_initial_interval_ms = 10;
static int g_buffer_fd = -1;

// threading controls
// thread handle
pthread_t g_buffer_thread;
// lock to protect: g_buffer_curr_level, g_queue_head, g_queue_tail
pthread_mutex_t g_buffer_lock;

struct timeval g_priming_start, g_priming_end;

enum {
    e_Buffer_Init,
    e_Buffer_Priming,
    e_Buffer_Normal,
} g_buffer_state = e_Buffer_Init;

static struct buffer_node *buffer_node_allocate(const void *buf, size_t nbyte)
{
    struct buffer_node *node = malloc(sizeof(struct buffer_node));

    assert(node);

    node->prev = node->next = NULL;
    node->start = 0;
    node->nbyte = nbyte;
    node->buffer = malloc(nbyte);
    assert(node->buffer);
    memcpy(node->buffer, buf, nbyte);

    return node;
}

static void buffer_node_free(struct buffer_node *node)
{
    assert(node);
    assert(node->buffer);

    free(node->buffer);
    free(node);
}

static void push_to_queue(int fd, const void *buf, size_t nbyte)
{
    struct buffer_node *newnode = buffer_node_allocate(buf, nbyte);

    //fprintf(stderr, "%s + push to Q %ld\n", MODULE, nbyte);
    pthread_mutex_lock(&g_buffer_lock);

    // when queue is empty
    if(g_queue_head==NULL && g_queue_tail==NULL) {
        g_queue_head = g_queue_tail = newnode;
    }
    // when queue is ....uh.... not empty
    // push to head of queue
    else {
        newnode->next = g_queue_head;
        g_queue_head->prev = newnode;
        g_queue_head = newnode;
    }

    g_buffer_curr_level += nbyte;
    pthread_mutex_unlock(&g_buffer_lock);

    //fprintf(stderr, "%s - push to Q %ld\n", MODULE, nbyte);
}

static void adjust_consumption_rate(long offset_bytes)
{
    // TODO: also adjusts interval
    fprintf(stderr, "%s old rate=%ld, offset=%ld\n", MODULE, 
            g_write_byte_rate, offset_bytes);

    long new_rate = (long)g_write_byte_rate +offset_bytes;
    if(new_rate>0) {
        g_write_byte_rate = new_rate;
    }
    else {
        g_write_byte_rate = 0; // to a halt
    }
    g_write_interval_ms = g_initial_interval_ms;
    g_write_chunk_bytes = g_write_byte_rate / (1000/g_write_interval_ms);    
    fprintf(stderr, "%s new rate %ld, new chunk %ld\n", MODULE,
            g_write_byte_rate, g_write_chunk_bytes);
}

void *buffer_thread_routine(void *data)
{
    unsigned long last_rate_adjust_clock = 0; 
    unsigned long total_bytes = 0;
    struct timeval t1; //, t2;

    fprintf(stderr, "%s buffer thread started\n", MODULE);
    gettimeofday(&t1, NULL);

    // write one chunk in every loop
    while(1) {
        long bytes;
        struct buffer_node *node= NULL;

        bytes = g_write_chunk_bytes;

        // keep our pace: write chunk bytes in each interval

        usleep(g_write_interval_ms * 1000); 
        g_write_clock++;

#if 0
        gettimeofday(&t2, NULL);
        long diff_ms = get_time_interval_in_ms(&t1, &t2);
        
        if((g_write_clock % 2) == 0) {
            fprintf(stderr, "%s @%ld ms, level %ld\n", MODULE, 
                    diff_ms, g_buffer_curr_level);
        }  
#endif

        // search for buffer nodes to satisfy this chunk write
        while(bytes) {

            pthread_mutex_lock(&g_buffer_lock);
            node = g_queue_tail;
            if(NULL==node) {
                //fprintf(stderr, "%s queue empty\n", MODULE);
                pthread_mutex_unlock(&g_buffer_lock);
                continue;
            }

            // tail node is not larger than bytes to write
            // remove tail node from queue
            if(node->nbyte <= bytes) {
                g_queue_tail = node->prev; //adjust queue tail
                g_buffer_curr_level -= node->nbyte;
                if(NULL==g_queue_tail) {
                    g_queue_head = NULL; // removed last node, now queue is empty
                }
                pthread_mutex_unlock(&g_buffer_lock);

                write(g_buffer_fd, node->buffer+node->start, node->nbyte);
                total_bytes += node->nbyte;
                bytes -= node->nbyte;
                buffer_node_free(node);
            }
            // tail node is larger than bytes, 
            // keep this node in queue and write out "bytes" of data.
            else {
                g_buffer_curr_level -= bytes;
                pthread_mutex_unlock(&g_buffer_lock);

                write(g_buffer_fd, node->buffer+node->start, bytes);
                total_bytes += bytes;
                node->start += bytes;
                node->nbyte -= bytes;
                bytes = 0;
            }
        } // end of writing bytes

        // monitor buffer level and make adjustments

        // level too high, increase consumption speed
        // but don't adjust too often
        if(g_buffer_curr_level>g_buffer_threshold_high &&
                g_write_clock-last_rate_adjust_clock > 10) {

            fprintf(stderr, "%s level too high (%ld/%ld) @ %ld, speed up\n", MODULE,
                    g_buffer_curr_level, g_buffer_threshold_high,
                    g_write_clock);
            //adjust_consumption_rate( g_buffer_max_level/10 ); // +5%
            adjust_consumption_rate( g_first_write_byte_rate/50 ); // +2%
            last_rate_adjust_clock = g_write_clock;
        }
        
        // level too low, decrease consumption speed
        // but don't adjust too often
        if(g_buffer_curr_level<g_buffer_threshold_low &&
                g_write_clock-last_rate_adjust_clock > 10) {

            fprintf(stderr, "%s level too low (%ld/%ld) @ %ld, slow down\n", MODULE,
                    g_buffer_curr_level, g_buffer_threshold_low,
                    g_write_clock);
            //adjust_consumption_rate( -1 * (long)(g_buffer_max_level/10) ); // -5%
            adjust_consumption_rate( -1 * (long)(g_first_write_byte_rate/50) ); // -2%
            last_rate_adjust_clock = g_write_clock;
        }

    } // end of thread loop

    return NULL;
}

static size_t smooth_write(int fd, const void *buf, size_t nbyte)
{
    // State: init --> priming
    if(e_Buffer_Init==g_buffer_state) {
        g_buffer_fd = fd;

        pthread_mutex_init(&g_buffer_lock, NULL);

        gettimeofday(&g_priming_start, NULL);
        push_to_queue(fd, buf, nbyte);
        g_buffer_state = e_Buffer_Priming;

        fprintf(stderr, "%s init --> priming\n", MODULE);
    }
    // State: priming
    else if(e_Buffer_Priming==g_buffer_state) {

        push_to_queue(fd, buf, nbyte);

        // State: priming --> normal
        if(g_buffer_curr_level >= g_buffer_start_level) {
            g_buffer_state = e_Buffer_Normal;

            fprintf(stderr, "%s priming --> normal\n", MODULE);
            // calculate incoming rate
            gettimeofday(&g_priming_end, NULL);
            long diff_ms = get_time_interval_in_ms(&g_priming_start, &g_priming_end);
            g_write_byte_rate = g_buffer_curr_level*1000/diff_ms;
            g_first_write_byte_rate = g_write_byte_rate;
            g_write_interval_ms = g_initial_interval_ms;
            g_write_chunk_bytes = g_write_byte_rate / (1000/g_write_interval_ms);    
            fprintf(stderr, "%s write rate %ld, chunk size=%ld, current level=%ld, %ld\n",
                    MODULE,
                    g_write_byte_rate, g_write_chunk_bytes,
                    g_buffer_curr_level, diff_ms);

            // create consumer thread
            int ret = pthread_create(&g_buffer_thread, NULL, buffer_thread_routine, NULL);
            if(ret<0) {
                fprintf(stderr, "%s cannot create thread: %s\n", MODULE, strerror(errno));
                assert(0);
            }
        }
    }
    else if(e_Buffer_Normal==g_buffer_state) {
        push_to_queue(fd, buf, nbyte);
    }
    else {
        assert(0);
    }   

    return nbyte;
}

void smooth_write_init(void)
{
    struct timeval t1, t2;

    gettimeofday(&t1, NULL);
    usleep(g_initial_interval_ms*1000);
    gettimeofday(&t2, NULL);

    long diff_ms = get_time_interval_in_ms(&t1, &t2);
    // this is due to system scheduling, so we typically sleep longer than 
    // requested.
    fprintf(stderr, "%s adjust initial interval from %d to %ld\n", MODULE,
            g_initial_interval_ms, diff_ms);
    g_initial_interval_ms = diff_ms;
}

// End of smooth buffering
// ==========================================================================

void signal_handler(int signo)
{
    fprintf(stderr, "%s signal %d received\n", MODULE, signo); 
    fprintf(stderr, "%s current level %ld\n", MODULE, g_buffer_curr_level); 
    exit(0);
}

int main(int argc, char **argv)
{
    char buf[4096];

    smooth_write_init();
    signal(SIGINT, signal_handler);

    while(1) {
        ssize_t sz;
        ssize_t wsz;

        sz = read(0, buf, sizeof(buf));
        if(sz<0) {
            fprintf(stderr, "%s read failed: %s\n", MODULE, strerror(errno));
            fflush(stderr);
            break;
        }
        else if(sz==0) {
            fprintf(stderr, "%s EOL\n", MODULE);
            fflush(stderr);
            break;
        }

#if 0
        wsz = write(1, buf, sz);
        if(wsz!=sz) {
            fprintf(stderr, "%s short write!\n", MODULE);
            break;
        }
#else
        wsz = smooth_write(1, buf, sz);
#endif
    } // end of while loop

    return 0;
}

