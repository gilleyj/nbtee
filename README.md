# bftee
A simple non-blocking named-pipe redirector with multiplexing

## Compiling

```
gcc bftee.c -o bftee
```

## Usage

Redirect your chatty program into the bftee executable with a named pipe and a size of a buffer:

```
Usage:
 outputProg 2>&1 | ./bftee [FIFO] [BufferSize]
```

Where:
 * FIFO - path to a named pipe, required argument
 * BufferSize - temporary Internal buffer size in case write to FIFO fails

### Example

```
mkfifo my_named_pipe
ping 8.8.4.4 2>&1 | ./bftee my_named_pipe 80 | gzip > compressed_output.gz &
cat my_named_pipe
```

