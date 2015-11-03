# bftee
A simple non-blocking named-pipe redirector with multiplexing

## Compiling

```
gcc nbtee.c -o nbtee
```

## Usage

Redirect your chatty program into the nbtee executable with a named pipe and a size of a buffer:

```
Usage:
 outputProg 2>&1 | ./nbtee [FIFO] [BufferSize]
```

Where:
 * FIFO - path to a named pipe, required argument
 * BufferSize - temporary Internal buffer size in case write to FIFO fails

### Example

```
mkfifo my_named_pipe
ping 8.8.4.4 2>&1 | ./nbtee my_named_pipe 80 | gzip > compressed_output.gz &
cat my_named_pipe
```

