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

// ==========================================================================
// Start of smooth buffering

#define BUFFER_SIZE (40*1024)
struct buffer_node {
    char buffer[BUFFER_SIZE];
    int start;
    int end;

    struct buffer_node *prev;
    struct buffer_node *next;
};


typedef struct smooth_t {

    // pointer to queue head (incoming) and tail (outgoing)
    struct buffer_node *queue_head, *queue_tail;

    // current buffer level (water level)
    unsigned long buffer_curr_level;// = 0;
    unsigned long buffer_highest_level;// = 0;
    // the constant consumption we try to achieve
    // g_write_byte_rate will directly affect g_write_interval_ms and g_write_chunk_bytes
    unsigned long write_byte_rate;// = 0;
    unsigned long first_write_byte_rate;// = 0;

    unsigned long write_interval_ms;// = 0;
    unsigned long write_chunk_bytes;// = 0;
    unsigned long write_clock;// = 0;

    int initial_interval_ms; // = 10;
    int buffer_fd; // = -1;

    struct timeval incoming_t1, incoming_t2;
    unsigned long incoming_byte_rate; // = 0;
    unsigned long incoming_bytes_1; // = 0;

    // threading controls
    // thread handle
    pthread_t buffer_thread;
    // lock to protect: g_buffer_curr_level, g_queue_head, g_queue_tail
    pthread_mutex_t buffer_lock;

    struct timeval priming_start, priming_end;

    enum {
        e_Buffer_Init,
        e_Buffer_Priming,
        e_Buffer_Normal,
    } buffer_state;
} smooth_t;

#define MODULE "[smoother3]"

// turn on/off debug message
#if 1
#define dbg_print(fmt, args...)  \
        do {\
            fprintf(stderr, "%s " fmt, MODULE, ##args);\
        } while(0)
#else
    #define dbg_print(fmt, args...) do {} while(0)
#endif

static inline void smooth_usleep(long usec)
{
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = usec;

    select(0, NULL, NULL, NULL, &tv);
}

static inline unsigned long smooth_get_time_interval_in_ms(const struct timeval *pt1,
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


static struct buffer_node *buffer_node_allocate(void)
{
    struct buffer_node *node = malloc(sizeof(struct buffer_node));
    assert(node);
    memset(node, 0, sizeof(*node));

    return node;
}

static void buffer_node_free(struct buffer_node *node)
{
    assert(node);
    free(node);
}

static void push_to_queue(smooth_t *t, int fd, const void *buf, size_t nbyte)
{
    struct buffer_node *node = NULL;

    //fprintf(stderr, "%s + push to Q %ld\n", MODULE, nbyte);
    pthread_mutex_lock(&t->buffer_lock);

    // when queue is empty
    if(t->queue_head==NULL) {
        node = buffer_node_allocate();
        t->queue_head = t->queue_tail = node;
    }
    // when queue is ....uh.... not empty
    else {
        // head node can accomodate this buffer
        if((t->queue_head->end + nbyte) < sizeof(t->queue_head->buffer)) {
            node = t->queue_head;
        }   
        // need to allocate a new node
        else {
            // push to head of queue
            node = buffer_node_allocate();
            node->next = t->queue_head;
            t->queue_head->prev = node;
            t->queue_head = node;
        }
    }

    // copy data into the node
    memcpy(node->buffer+node->end, buf, nbyte);
    node->end += nbyte;

    //TODO: check if race condition by giving up lock here
    pthread_mutex_unlock(&t->buffer_lock);

    t->buffer_curr_level += nbyte;
    t->incoming_bytes_1 += nbyte;

    //dbg_print("%s - push to Q %ld\n", nbyte);
    
    // calculate incoming byte rate every 2 seconds
    if(t->incoming_t1.tv_sec==0 && t->incoming_t1.tv_usec==0) {
        gettimeofday(&t->incoming_t1, NULL);
    }
    else {
        gettimeofday(&t->incoming_t2, NULL);
        long diff_ms = smooth_get_time_interval_in_ms(&t->incoming_t1, &t->incoming_t2);
        if(diff_ms>1000) {
            t->incoming_byte_rate = t->incoming_bytes_1 * 1000 / diff_ms;

            dbg_print("new incoming rate %ld/sec\n", t->incoming_byte_rate);
            // reset timer and byte count
            t->incoming_bytes_1 = 0;
            t->incoming_t1 = t->incoming_t2;
        }
    }
}

static void adjust_consumption_rate(smooth_t *t, long offset_bytes)
{
    // TODO: also adjusts interval
    dbg_print("old rate=%ld, offset=%ld\n", t->write_byte_rate, offset_bytes);

    long new_rate = (long)t->write_byte_rate +offset_bytes;
    if(new_rate>0) {
        t->write_byte_rate = new_rate;
    }
    else {
        t->write_byte_rate = 0; // to a halt
    }
    t->write_interval_ms = t->initial_interval_ms;
    t->write_chunk_bytes = t->write_byte_rate / (1000/t->write_interval_ms);    
    dbg_print("new rate %ld, new chunk %ld\n", 
            t->write_byte_rate, t->write_chunk_bytes);
}

static void *buffer_thread_routine(void *data)
{
    unsigned long total_bytes = 0;
    unsigned long out_bytes = 0;
    struct timeval t1, t2;
    smooth_t *t = (smooth_t *)data;

    dbg_print("buffer thread started\n");
    gettimeofday(&t1, NULL);

    // write one chunk in every loop
    while(1) {
        long bytes;
        struct buffer_node *node= NULL;

        bytes = t->write_chunk_bytes;

        // keep our pace: write chunk bytes in each interval

        smooth_usleep(t->write_interval_ms * 1000); 
        t->write_clock++;

        // search for buffer nodes to satisfy this chunk write
        out_bytes += bytes;
        while(bytes) {

            pthread_mutex_lock(&t->buffer_lock);
            node = t->queue_tail;
            if(NULL==node) {
                //dbg_print("queue empty\n");
                pthread_mutex_unlock(&t->buffer_lock);
                smooth_usleep(10*1000); // no rush since queue will stay empty in short time
                continue;
            }

            // data in tail node is not larger than bytes to write
            // remove tail node from queue
            if( (node->end - node->start) <= bytes) {
                t->queue_tail = node->prev; //adjust queue tail
                t->buffer_curr_level -= (node->end - node->start);
                if(NULL==t->queue_tail) {
                    t->queue_head = NULL; // removed last node, now queue is empty
                }
                pthread_mutex_unlock(&t->buffer_lock);

                long size = node->end - node->start;
                write(t->buffer_fd, node->buffer+node->start, size);
                total_bytes += size;
                bytes -= size;
                buffer_node_free(node);
            }
            // tail node is larger than bytes, 
            // keep this node in queue and write out "bytes" of data.
            else {
                t->buffer_curr_level -= bytes;
                pthread_mutex_unlock(&t->buffer_lock);

                write(t->buffer_fd, node->buffer+node->start, bytes);
                total_bytes += bytes;
                node->start += bytes;
                bytes = 0;
            }
        } // end of writing bytes


        // monitor actual byte rate 
        // if too far with average incoming byte rate, adjust consumption speed
        gettimeofday(&t2, NULL);
        long diff_ms = smooth_get_time_interval_in_ms(&t1, &t2);
        if(diff_ms<500) continue;

        // diff_ms >= 500
        long average_out_rate = out_bytes * 1000 / diff_ms;
        dbg_print("re-calculate out rate %ld/%ld=%ld\n", out_bytes, diff_ms, average_out_rate);

        if(t->buffer_highest_level <= t->buffer_curr_level) {
            t->buffer_highest_level = t->buffer_curr_level;
        }
        dbg_print("curr level %ld, highest level %ld\n", t->buffer_curr_level, t->buffer_highest_level);

        if(average_out_rate > t->incoming_byte_rate) {
            long adjustment = (long)t->incoming_byte_rate - average_out_rate ;
            adjustment /= 20;

            dbg_print("too fast (%ld > %ld), slow down by %ld\n",
                    average_out_rate, t->incoming_byte_rate, adjustment);
            adjust_consumption_rate(t, adjustment );
        }
        else if(average_out_rate < t->incoming_byte_rate) {
            long adjustment = (long)t->incoming_byte_rate - average_out_rate;
            adjustment /= 20;
            dbg_print("too slow (%ld < %ld), speed up by %ld\n",
                    average_out_rate, t->incoming_byte_rate, adjustment);
            adjust_consumption_rate(t, adjustment );
        }
        // reset stop watch
        t1 = t2;
        out_bytes = 0;

        //monitor buffer level and make more adjustments, to avoid too much buffer
        if(t->buffer_curr_level >= t->incoming_byte_rate/2) {
            long adjustment = (t->buffer_curr_level - (long)t->incoming_byte_rate/2 )/20;
            dbg_print("buffer to high, speed up by %ld\n", adjustment);
            adjust_consumption_rate(t, adjustment );
        }


    } // end of thread loop

    return NULL;
}

size_t smooth_write(smooth_t *t, int fd, const void *buf, size_t nbyte)
{
    // State: init --> priming
    if(e_Buffer_Init==t->buffer_state) {
        t->buffer_fd = fd;

        pthread_mutex_init(&t->buffer_lock, NULL);

        gettimeofday(&t->priming_start, NULL);
        push_to_queue(t, fd, buf, nbyte);
        t->buffer_state = e_Buffer_Priming;

        dbg_print("init --> priming\n");
    }
    // State: priming
    else if(e_Buffer_Priming==t->buffer_state) {
        struct timeval t2;

        push_to_queue(t, fd, buf, nbyte);

        gettimeofday(&t2, NULL);
        long diff_ms = smooth_get_time_interval_in_ms(&t->priming_start, &t2);

        // State: priming --> normal
        if(diff_ms >= 700) {
            t->buffer_state = e_Buffer_Normal;
            dbg_print("priming --> normal\n");

            // determine consumption speed
            t->write_byte_rate = t->buffer_curr_level*1000/diff_ms;
            t->first_write_byte_rate = t->write_byte_rate;
            t->incoming_byte_rate = t->write_byte_rate;
            t->write_interval_ms = t->initial_interval_ms;
            t->write_chunk_bytes = t->write_byte_rate / (1000/t->write_interval_ms);    
            dbg_print("write rate %ld, chunk size=%ld, current level=%ld, %ld\n",
                    t->write_byte_rate, t->write_chunk_bytes,
                    t->buffer_curr_level, diff_ms);

            // create consumer thread
            int ret = pthread_create(&t->buffer_thread, NULL, buffer_thread_routine, t);
            if(ret<0) {
                dbg_print("cannot create thread: %s\n", strerror(errno));
                assert(0);
            }
        }
    }
    else if(e_Buffer_Normal==t->buffer_state) {
        push_to_queue(t, fd, buf, nbyte);
    }
    else {
        assert(0);
    }   

    return nbyte;
}

smooth_t *smooth_write_init(void)
{
    struct timeval t1, t2;

    smooth_t *t = malloc(sizeof(smooth_t));
    if(NULL==t) return NULL;
    memset(t, 0, sizeof(smooth_t));

    // initialized parameters
    t->initial_interval_ms = 10;
    t->buffer_fd = -1;
    t->buffer_state = e_Buffer_Init;

    gettimeofday(&t1, NULL);
    smooth_usleep(t->initial_interval_ms*1000);
    gettimeofday(&t2, NULL);

    long diff_ms = smooth_get_time_interval_in_ms(&t1, &t2);
    // this is due to system scheduling, so we typically sleep longer than 
    // requested.
    dbg_print("adjust initial interval from %d to %ld\n",
            t->initial_interval_ms, diff_ms);
    t->initial_interval_ms = diff_ms;

    return t;
}

// End of smooth buffering
// ==========================================================================

void signal_handler(int signo)
{
    fprintf(stderr, "%s signal %d received\n", MODULE, signo); 
    exit(0);
}

int main(int argc, char **argv)
{
    char buf[4096];
    smooth_t *t;

    t = smooth_write_init();
    if(!t) {
        fprintf(stderr, "cannot allocate context\n");
        exit(1);
    }
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
        wsz = smooth_write(t, 1, buf, sz);
        if(wsz<0) {
            exit(1);
        }
#endif
    } // end of while loop

    return 0;
}

