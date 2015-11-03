/* bftee - clone stdin to stdout and to a buffered, non-blocking pipe 
    () racic@stackoverflow
    () fabraxias@stackoverflow
    () gilleyj@github
   WTFPL Licence */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

// the number of sBuffers that are being held at a maximum
#define BUFFER_SIZE 4096
#define BLOCK_SIZE 2048

typedef struct {
  char data[BLOCK_SIZE];
  int bytes;
} sBuffer;

typedef struct {
  sBuffer *data;  //array of buffers
  int bufferSize; // number of buffer in data
  int start;      // index of the current start buffer
  int end;        // index of the current end buffer
  int active;     // number of active buffer (currently in use)
  int maxUse;     // maximum number of buffers ever used
  int drops;      // number of discarded buffer due to overflow
  int sWrites;    // number of buffer written to stdout
  int pWrites;    // number of buffers written to pipe
} sQueue;

void InitQueue(sQueue*, int);              // Initialize the Queue
void PushToQueue(sQueue*, sBuffer*, int);  // Push the buffer onto the end of the Queue 
sBuffer *RetrieveFromQueue(sQueue*);       // returns the first entry of the buffer and removes it or NULL is buffer is empty
sBuffer *PeakAtQueue(sQueue*);             // returns the first entry of the buffer but does not remove it. Returns NULL on an empty buffer
void ShrinkInQueue(sQueue *queue, int);    // shrinks the first entry of the buffer by n-bytes. Buffer is removed if it is empty
void DelFromQueue(sQueue *queue);          // removes the first entry of the queue

static void sigUSR1(int);                  // signal handled for SUGUSR1 - used for stats output to stderr
static void sigINT(int);                   // signla handler for SIGKILL/SIGTERM - allows for a graceful stop ?

sQueue queue;                              // Buffer storing the overflow

volatile int quit;                         // trigger for quiting the main loop

int main(int argc, char *argv[]) {   
    int           readfd, writefd;
    struct stat   status;
    char          fifonam;
    sBuffer       buffer;
    ssize_t       bytes;
    int           bufferSize = BUFFER_SIZE;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, sigUSR1);
    signal(SIGTERM, sigINT);
    signal(SIGINT,  sigINT);

    /** Handle commandline args and open the pipe for non blocking writing **/

    if(argc < 2 || argc > 3) {   
        printf("Usage:\n outputProg 2>&1 | %s [FIFO] [BufferSize]\n"
               "FIFO - path to a named pipe, required argument ( mkfifo named_pipe )\n"
               "BufferSize - temporary Internal buffer size in case write to FIFO fails\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    fifonam = argv[1];
    if (argc == 3) {
      bufferSize = atoi(argv[2]);
      if (bufferSize == 0) bufferSize = BUFFER_SIZE;
    }

    readfd = open(fifonam, O_RDONLY | O_NONBLOCK);
    if(-1==readfd) {   
        perror("bftee: readfd: open()");
        exit(EXIT_FAILURE);
    }

    if(-1==fstat(readfd, &status)) {
        perror("bftee: fstat");
        close(readfd);
        exit(EXIT_FAILURE);
    }

    if(!S_ISFIFO(status.st_mode)) {
        printf("bftee: %s in not a fifo!\n", fifonam);
        close(readfd);
        exit(EXIT_FAILURE);
    }

    writefd = open(fifonam, O_WRONLY | O_NONBLOCK);
    if(-1==writefd) {
        perror("bftee: writefd: open()");
        close(readfd);
        exit(EXIT_FAILURE);
    }

    close(readfd);

    InitQueue(&queue, bufferSize);
    quit = 0;

    while(!quit) {
        // read from STDIN
        bytes = read(STDIN_FILENO, buffer.data, sizeof(buffer.data));

        // if read failed due to interrupt, then retry, otherwise STDIN has closed and we should stop reading
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0) break;

        // save the number if read bytes in the current buffer to be processed
        buffer.bytes = bytes;

        // this is a blocking write. As long as buffer is smaller than 4096 Bytes, 
        // the write is atomic to a pipe in Linux thus, this cannot be interrupted. 
        // however, to be save this should handle the error cases of partial or 
        // interrupted write none the less.
        bytes = write(STDOUT_FILENO, buffer.data, buffer.bytes);
        queue.sWrites++;

        if(-1==bytes) {
            perror("bftee: writing to stdout");
            break;
        }

        sBuffer *tmpBuffer = NULL;

        // if the queue is empty (tmpBuffer gets set to NULL) the this does nothing 
        // otherwise it tries to write the buffered data to the pipe. This continues 
        // until the Buffer is empty or the write fails.
        // NOTE: bytes cannot be -1  (that would have failed just before) when the 
        // loop is entered. 
        while ((bytes != -1) && (tmpBuffer = PeakAtQueue(&queue)) != NULL) {
           // write the oldest buffer to the pipe
           bytes = write(writefd, tmpBuffer->data, tmpBuffer->bytes);

           // the  written bytes are equal to the buffer size, the write is successful - remove the buffer and continue
           if (bytes == tmpBuffer->bytes) {
             DelFromQueue(&queue);
             queue.pWrites++;
           } else if (bytes > 0) {
             // on a positive bytes value there was a partial write. we shrink the 
             // current buffer and handle this as a write failure
             ShrinkInQueue(&queue, bytes);
             bytes = -1;
           }
        }

        // There are several cases here:
        // 1.) The Queue is empty -> bytes is still set from the write to STDOUT. 
        //     in this case, we try to write the read data directly to the pipe
        // 2.) The Queue was not empty but is now -> bytes is set from the last 
        //     write (which was successful) and is bigger 0. also try to write the data
        // 3.) The Queue was not empty and still is not -> there was a write 
        //     error before (even partial), and bytes is -1. Thus this line is skipped.
        if (bytes != -1) bytes = write(writefd, buffer.data, buffer.bytes);

        // again, there are several cases what can happen here
        // 1.) the write before was successful -> in this case bytes is equal to 
        //     buffer.bytes and nothing happens
        // 2.) the write just before is partial or failed all together - bytes is 
        //     either -1 or smaller than buffer.bytes -> add the remaining data to the queue
        // 3.) the write before did not happen as the buffer flush already had an 
        //     error. In this case bytes is -1 -> add the remaining data to the queue
        if (bytes != buffer.bytes)
          PushToQueue(&queue, &buffer, bytes);
        else 
          queue.pWrites++;
    }

    // once we are done with STDIN, try to flush the buffer to the named pipe
    if (queue.active > 0) {
       //set output buffer to block - here we wait until we can write everything to the named pipe
       // --> this does not seem to work - just in case there is a busy loop that waits for buffer flush aswell. 
       int saved_flags = fcntl(writefd, F_GETFL);
       int new_flags = saved_flags & ~O_NONBLOCK;
       int res = fcntl(writefd, F_SETFL, new_flags);

       sBuffer *tmpBuffer = NULL;
       //TODO: this does not handle partial writes yet
       while ((tmpBuffer = PeakAtQueue(&queue)) != NULL) {
         int bytes = write(writefd, tmpBuffer->data, tmpBuffer->bytes);
         if (bytes != -1) DelFromQueue(&queue);
       }
    }
    close(writefd);
}


/** Initialize a given Queue **/
void InitQueue (sQueue *queue, int bufferSize) {
  queue->data = calloc(bufferSize, sizeof(sBuffer));
  queue->bufferSize = bufferSize;
  queue->start = 0;
  queue->end = 0;
  queue->active = 0;
  queue->maxUse = 0;
  queue->drops = 0;
  queue->sWrites = 0;
  queue->pWrites = 0;
}

/** push a buffer onto end of the Queue**/
void PushToQueue(sQueue *queue, sBuffer *p, int offset) {

    if (offset < 0) offset = 0;      // offset cannot be smaller than 0 - if that is the case, we were given an error code. Set it to 0 instead
    if (offset == p->bytes) return;  // in this case there are 0 bytes to add to the queue. Nothing to write

    // this should never happen - offset cannot be bigger than the buffer itself. Panic action
    if (offset > p->bytes) { 
      perror("got more bytes to buffer than we read\n"); 
      exit(EXIT_FAILURE); 
    }

    // debug output on a partial write.
    // if (offset > 0 ) fprintf(stderr, "partial write to buffer\n");

    // copy the data from the buffer into the queue and remember its size
    memcpy(queue->data[queue->end].data, p->data + offset , p->bytes-offset);
    queue->data[queue->end].bytes = p->bytes - offset;

    // move the buffer forward
    queue->end = (queue->end + 1) % queue->bufferSize;

    // there is still space in the buffer
    if (queue->active < queue->bufferSize) {
        queue->active++;
        if (queue->active > queue->maxUse) queue->maxUse = queue->active;
    } else {
        // Overwriting the oldest. Move start to next-oldest
        queue->start = (queue->start + 1) % queue->bufferSize;
        queue->drops++;
    }
}

/** return the oldest entry in the Queue and remove it or return NULL in case the Queue is empty **/
sBuffer *RetrieveFromQueue(sQueue *queue) {
    if (!queue->active) { return NULL; }

    queue->start = (queue->start + 1) % queue->bufferSize;
    queue->active--;
    return &(queue->data[queue->start]);
}

/** return the oldest entry in the Queue or NULL if the Queue is empty. Does not remove the entry **/
sBuffer *PeakAtQueue(sQueue *queue) {
    if (!queue->active) { return NULL; }
    return &(queue->data[queue->start]);
}

/*** Shrinks the oldest entry i the Queue by bytes. Removes the entry if buffer of the oldest entry runs empty*/
void ShrinkInQueue(sQueue *queue, int bytes) {

  // cannot remove negative amount of bytes - this is an error case. Ignore it
  if (bytes <= 0) return;

  // remove the entry if the offset is equal to the buffer size
  if (queue->data[queue->start].bytes == bytes) {
    DelFromQueue(queue);
    return;
  };

  // this is a partial delete
  if (queue->data[queue->start].bytes > bytes) {
    //shift the memory by the offset
    memmove(queue->data[queue->start].data, queue->data[queue->start].data + bytes, queue->data[queue->start].bytes - bytes);
    queue->data[queue->start].bytes = queue->data[queue->start].bytes - bytes;
    return;
  }

  // panic is the are to remove more than we have the buffer
  if (queue->data[queue->start].bytes < bytes) {
    perror("we wrote more than we had - this should never happen\n");
    exit(EXIT_FAILURE);
    return;
  }
}

/** delete the oldest entry from the queue. Do nothing if the Queue is empty **/
void DelFromQueue(sQueue *queue) {
    if (queue->active > 0) {
      queue->start = (queue->start + 1) % queue->bufferSize;
      queue->active--;
    }
}

/** Stats output on SIGUSR1 **/
static void sigUSR1(int signo) {
  fprintf(stderr, "Buffer use: %i (%i/%i), STDOUT: %i PIPE: %i:%i\n", queue.active, queue.maxUse, queue.bufferSize, queue.sWrites, queue.pWrites, queue.drops);
}

/** handle signal for terminating **/
static void sigINT(int signo) {
  quit++;
  if (quit > 1) exit(EXIT_FAILURE);
}

