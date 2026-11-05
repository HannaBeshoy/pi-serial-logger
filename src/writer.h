#ifndef WRITER_H
#define WRITER_H
/* The child process: reads CSV lines from the pipe and appends them to DIR/YYYY-MM-DD.csv.
   Returns the exit status (0 when the pipe was closed normally). */
int writer_main(int pipe_r, const char *dir);
#endif
