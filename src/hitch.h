/* Hitch timers: bench_mark("what") says the code since the last mark was `what` (game thread only). Compiled in with
 * make XDEF=-DGBADS_HITCH_MARKS, which makes bench_frame write each frame's marks (bench.c); without it they are no code at all. */
#ifndef GBADS_HITCH_H
#define GBADS_HITCH_H

#ifdef GBADS_HITCH_MARKS
void bench_marks_begin(void);
void bench_mark(const char* what);
void bench_markf(const char* format, ...) __attribute__((format(printf, 1, 2)));
#else
#define bench_marks_begin() ((void) 0)
#define bench_mark(what) ((void) 0)
#define bench_markf(...) ((void) 0)
#endif

#endif
