/* linenoise.c -- guerrilla line editing library against the idea that a
 * line editing lib needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 * 
 *   http://github.com/antirez/linenoise
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2013, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * ------------------------------------------------------------------------
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * CHA (Cursor Horizontal Absolute)
 *    Sequence: ESC [ n G
 *    Effect: moves cursor to column n
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward of n chars
 *
 * When multi line mode is enabled, we also use an additional escape
 * sequence. However multi line editing is disabled by default.
 *
 * CUU (Cursor Up)
 *    Sequence: ESC [ n A
 *    Effect: moves cursor up of n chars.
 *
 * CUD (Cursor Down)
 *    Sequence: ESC [ n B
 *    Effect: moves cursor down of n chars.
 *
 * The following are used to clear the screen: ESC [ H ESC [ 2 J
 * This is actually composed of two sequences:
 *
 * cursorhome
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED2 (Clear entire screen)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 * 
 */

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "linenoise.h"

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096
static char *unsupported_term[] = {"dumb","cons25",NULL};
static linenoiseCompletionCallback *completionCallback = NULL;

static struct termios orig_termios; /* In order to restore at exit.*/
static int rawmode = 0; /* For atexit() function to check if restore is needed*/
static int mlmode = 0;  /* Multi line mode. Default is single line. */
static int atexit_registered = 0; /* Register atexit just 1 time. */
static int history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
char **history = NULL;

/* The linenoiseState structure represents the state during line editing.
 * We pass this state to functions implementing specific editing
 * functionalities. */
struct linenoiseState {
    int fd;             /* Terminal file descriptor. */
    char *buf;          /* Edited line buffer. */
    size_t buflen;      /* Edited line buffer size. */
    const char *prompt; /* Prompt to display. */
    size_t plen;        /* Prompt length. */
    size_t pos;         /* Current cursor position. */
    size_t oldpos;      /* Previous refresh cursor position. */
    size_t len;         /* Current edited line length. */
    size_t cols;        /* Number of columns in terminal. */
    size_t maxrows;     /* Maximum num of rows used so far (multiline mode) */
    int history_index;  /* The history index we are currently editing. */
    char seq[2];        /* State memory for escape sequences */
    char seq2[2];       /* Extended escape sequences */
    linenoiseCompletions    completions;    /* Suggestions when in completion mode */
    size_t comp_i;      /* Current completion suggestion index */
    enum {
        ST_START,
        ST_COMPLETION,
        ST_ESC_SEQ_0,
        ST_ESC_SEQ_1,
        ST_ESC_SEQ2_0,
        ST_ESC_SEQ2_1,
    } state;            /* Input handling states */
};

static void linenoiseAtExit(void);
int linenoiseHistoryAdd(const char *line);
static void refreshLine(struct linenoiseState *l);

/* ======================= Low level terminal handling ====================== */

/* Set if to use or not the multi line mode. */
void linenoiseSetMultiLine(int ml) {
    mlmode = ml;
}

/* Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences. */
static int isUnsupportedTerm(void) {
    char *term = getenv("TERM");
    int j;

    if (term == NULL) return 0;
    for (j = 0; unsupported_term[j]; j++)
        if (!strcasecmp(term,unsupported_term[j])) return 1;
    return 0;
}

/* Raw mode: 1960 magic shit. */
static int enableRawMode(int fd) {
    struct termios raw;

    if (!isatty(STDIN_FILENO)) goto fatal;
    if (!atexit_registered) {
        atexit(linenoiseAtExit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

    raw = orig_termios;  /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - choing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer.
     * We want read to return every single byte, without timeout. */
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
    rawmode = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

static void disableRawMode(int fd) {
    /* Don't even check the return value as it's too late. */
    if (rawmode && tcsetattr(fd,TCSAFLUSH,&orig_termios) != -1)
        rawmode = 0;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int getColumns(void) {
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) return 80;
    return ws.ws_col;
}

/* Clear the screen. Used to handle ctrl+l */
void linenoiseClearScreen(void) {
    if (write(STDIN_FILENO,"\x1b[H\x1b[2J",7) <= 0) {
        /* nothing to do, just to avoid warning. */
    }
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void linenoiseBeep(void) {
    fprintf(stderr, "\x7");
    fflush(stderr);
}

/* ============================== Completion ================================ */

/* Free a list of completion option populated by linenoiseAddCompletion(). */
static void freeCompletions(linenoiseCompletions *lc) {
    size_t i;
    for (i = 0; i < lc->len; i++)
        free(lc->cvec[i]);
    if (lc->cvec != NULL)
        free(lc->cvec);
}

static void completeLineShow(struct linenoiseState *ls);

/* This is an helper function for linenoiseHandle() and is called when the
 * user types the <tab> key in order to begin the process of completing
 * the string currently in the input.
 * 
 * The state of the editing is encapsulated into the pointed linenoiseState
 * structure as described in the structure definition.
 *
 * Returns 0 when the completion mode should actually begin, 1 otherwise. */
static int completeLineStart(struct linenoiseState *ls) {
    linenoiseCompletions *comps = &ls->completions;

    comps->len = 0;
    comps->cvec = NULL;

    completionCallback(ls->buf,comps);

    /* Empty line, nothing to complete! */
    if (comps->len == 0) {
        linenoiseBeep();

        freeCompletions(comps);
        return 1;
    }

    ls->comp_i = 0;

    /* Show first completion suggestion */
    completeLineShow(ls);

    return 0;
}

/* Update the prompt with the current completion suggestion */
static void completeLineShow(struct linenoiseState *ls) {
    linenoiseCompletions *comps = &ls->completions;

    /* Show completion or original buffer */
    if (ls->comp_i < comps->len) {
        struct linenoiseState saved = *ls;

        ls->buf = comps->cvec[ls->comp_i];
        ls->len = ls->pos = strlen(ls->buf);
        refreshLine(ls);
        ls->len = saved.len;
        ls->pos = saved.pos;
        ls->buf = saved.buf;
    } else {
        refreshLine(ls);
    }
}

/* Handles a single character input for the purposes of completing.
 *
 * Returns 0 when completion is still ongoing, and 1 when the process
 * is finished */
static int completeLineHandle(struct linenoiseState *ls, char c) {
    linenoiseCompletions *comps = &ls->completions;
    int stop = 0;

    switch(c) {
    case 9: /* tab */
        ls->comp_i = (ls->comp_i+1) % (comps->len+1);
        if (ls->comp_i == comps->len) linenoiseBeep();
        break;
    case 27: /* escape */
        /* Re-show original buffer */
        if (ls->comp_i < comps->len) refreshLine(ls);
        stop = 1;
        break;
    default:
        /* Update buffer and return */
        if (ls->comp_i < comps->len) {
            int nwritten = snprintf(ls->buf,ls->buflen,"%s",comps->cvec[ls->comp_i]);
            ls->len = ls->pos = nwritten;
        }
        stop = 1;
        break;
    }

    if (stop) {
        freeCompletions(comps);
    }
    else {
        /* Show current suggestion if still completing */
        completeLineShow(ls);
    }

    return stop;
}

/* Register a callback function to be called for tab-completion. */
void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn) {
    completionCallback = fn;
}

/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void linenoiseAddCompletion(linenoiseCompletions *lc, char *str) {
    size_t len = strlen(str);
    char *copy = malloc(len+1);
    memcpy(copy,str,len+1);
    lc->cvec = realloc(lc->cvec,sizeof(char*)*(lc->len+1));
    lc->cvec[lc->len++] = copy;
}

/* =========================== Line editing ================================= */

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshSingleLine(struct linenoiseState *l) {
    char seq[64];
    size_t plen = strlen(l->prompt);
    int fd = l->fd;
    char *buf = l->buf;
    size_t len = l->len;
    size_t pos = l->pos;
    
    while((plen+pos) >= l->cols) {
        buf++;
        len--;
        pos--;
    }
    while (plen+len > l->cols) {
        len--;
    }

    /* Cursor to left edge */
    snprintf(seq,64,"\x1b[0G");
    if (write(fd,seq,strlen(seq)) == -1) return;
    /* Write the prompt and the current buffer content */
    if (write(fd,l->prompt,strlen(l->prompt)) == -1) return;
    if (write(fd,buf,len) == -1) return;
    /* Erase to right */
    snprintf(seq,64,"\x1b[0K");
    if (write(fd,seq,strlen(seq)) == -1) return;
    /* Move cursor to original position. */
    snprintf(seq,64,"\x1b[0G\x1b[%dC", (int)(pos+plen));
    if (write(fd,seq,strlen(seq)) == -1) return;
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshMultiLine(struct linenoiseState *l) {
    char seq[64];
    int plen = strlen(l->prompt);
    int rows = (plen+l->len+l->cols-1)/l->cols; /* rows used by current buf. */
    int rpos = (plen+l->oldpos+l->cols)/l->cols; /* cursor relative row. */
    int rpos2; /* rpos after refresh. */
    int old_rows = l->maxrows;
    int fd = l->fd, j;

    /* Update maxrows if needed. */
    if (rows > (int)l->maxrows) l->maxrows = rows;

#ifdef LN_DEBUG
    FILE *fp = fopen("/tmp/debug.txt","a");
    fprintf(fp,"[%d %d %d] p: %d, rows: %d, rpos: %d, max: %d, oldmax: %d",
        (int)l->len,(int)l->pos,(int)l->oldpos,plen,rows,rpos,(int)l->maxrows,old_rows);
#endif

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */
    if (old_rows-rpos > 0) {
#ifdef LN_DEBUG
        fprintf(fp,", go down %d", old_rows-rpos);
#endif
        snprintf(seq,64,"\x1b[%dB", old_rows-rpos);
        if (write(fd,seq,strlen(seq)) == -1) return;
    }

    /* Now for every row clear it, go up. */
    for (j = 0; j < old_rows-1; j++) {
#ifdef LN_DEBUG
        fprintf(fp,", clear+up");
#endif
        snprintf(seq,64,"\x1b[0G\x1b[0K\x1b[1A");
        if (write(fd,seq,strlen(seq)) == -1) return;
    }

    /* Clean the top line. */
#ifdef LN_DEBUG
    fprintf(fp,", clear");
#endif
    snprintf(seq,64,"\x1b[0G\x1b[0K");
    if (write(fd,seq,strlen(seq)) == -1) return;
    
    /* Write the prompt and the current buffer content */
    if (write(fd,l->prompt,strlen(l->prompt)) == -1) return;
    if (write(fd,l->buf,l->len) == -1) return;

    /* If we are at the very end of the screen with our prompt, we need to
     * emit a newline and move the prompt to the first column. */
    if (l->pos &&
        l->pos == l->len &&
        (l->pos+plen) % l->cols == 0)
    {
#ifdef LN_DEBUG
        fprintf(fp,", <newline>");
#endif
        if (write(fd,"\n",1) == -1) return;
        snprintf(seq,64,"\x1b[0G");
        if (write(fd,seq,strlen(seq)) == -1) return;
        rows++;
        if (rows > (int)l->maxrows) l->maxrows = rows;
    }

    /* Move cursor to right position. */
    rpos2 = (plen+l->pos+l->cols)/l->cols; /* current cursor relative row. */
#ifdef LN_DEBUG
    fprintf(fp,", rpos2 %d", rpos2);
#endif
    /* Go up till we reach the expected positon. */
    if (rows-rpos2 > 0) {
#ifdef LN_DEBUG
        fprintf(fp,", go-up %d", rows-rpos2);
#endif
        snprintf(seq,64,"\x1b[%dA", rows-rpos2);
        if (write(fd,seq,strlen(seq)) == -1) return;
    }
    /* Set column. */
#ifdef LN_DEBUG
    fprintf(fp,", set col %d", 1+((plen+(int)l->pos) % (int)l->cols));
#endif
    snprintf(seq,64,"\x1b[%dG", 1+((plen+(int)l->pos) % (int)l->cols));
    if (write(fd,seq,strlen(seq)) == -1) return;

    l->oldpos = l->pos;

#ifdef LN_DEBUG
    fprintf(fp,"\n");
    fclose(fp);
#endif
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
static void refreshLine(struct linenoiseState *l) {
    if (mlmode)
        refreshMultiLine(l);
    else
        refreshSingleLine(l);
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
int linenoiseEditInsert(struct linenoiseState *l, int c) {
    if (l->len < l->buflen) {
        if (l->len == l->pos) {
            l->buf[l->pos] = c;
            l->pos++;
            l->len++;
            l->buf[l->len] = '\0';
            if ((!mlmode && l->plen+l->len < l->cols) /* || mlmode */) {
                /* Avoid a full update of the line in the
                 * trivial case. */
                if (write(l->fd,&c,1) == -1) return -1;
            } else {
                refreshLine(l);
            }
        } else {
            memmove(l->buf+l->pos+1,l->buf+l->pos,l->len-l->pos);
            l->buf[l->pos] = c;
            l->len++;
            l->pos++;
            l->buf[l->len] = '\0';
            refreshLine(l);
        }
    }
    return 0;
}

/* Move cursor on the left. */
void linenoiseEditMoveLeft(struct linenoiseState *l) {
    if (l->pos > 0) {
        l->pos--;
        refreshLine(l);
    }
}

/* Move cursor on the right. */
void linenoiseEditMoveRight(struct linenoiseState *l) {
    if (l->pos != l->len) {
        l->pos++;
        refreshLine(l);
    }
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define LINENOISE_HISTORY_NEXT 0
#define LINENOISE_HISTORY_PREV 1
void linenoiseEditHistoryNext(struct linenoiseState *l, int dir) {
    if (history_len > 1) {
        /* Update the current history entry before to
         * overwrite it with the next one. */
        free(history[history_len - 1 - l->history_index]);
        history[history_len - 1 - l->history_index] = strdup(l->buf);
        /* Show the new entry */
        l->history_index += (dir == LINENOISE_HISTORY_PREV) ? 1 : -1;
        if (l->history_index < 0) {
            l->history_index = 0;
            return;
        } else if (l->history_index >= history_len) {
            l->history_index = history_len-1;
            return;
        }
        strncpy(l->buf,history[history_len - 1 - l->history_index],l->buflen);
        l->buf[l->buflen-1] = '\0';
        l->len = l->pos = strlen(l->buf);
        refreshLine(l);
    }
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
void linenoiseEditDelete(struct linenoiseState *l) {
    if (l->len > 0 && l->pos < l->len) {
        memmove(l->buf+l->pos,l->buf+l->pos+1,l->len-l->pos-1);
        l->len--;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Backspace implementation. */
void linenoiseEditBackspace(struct linenoiseState *l) {
    if (l->pos > 0 && l->len > 0) {
        memmove(l->buf+l->pos-1,l->buf+l->pos,l->len-l->pos);
        l->pos--;
        l->len--;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Delete the previosu word, maintaining the cursor at the start of the
 * current word. */
void linenoiseEditDeletePrevWord(struct linenoiseState *l) {
    size_t old_pos = l->pos;
    size_t diff;

    while (l->pos > 0 && l->buf[l->pos-1] == ' ')
        l->pos--;
    while (l->pos > 0 && l->buf[l->pos-1] != ' ')
        l->pos--;
    diff = old_pos - l->pos;
    memmove(l->buf+l->pos,l->buf+old_pos,l->len-old_pos+1);
    l->len -= diff;
    refreshLine(l);
}

/*
 * Returns 1 when an input is ready, 2 when an error has occured, and
 * 0 otherwise.
 */
static int linenoiseHandle(struct linenoiseState *l, char c)
{
    int rc;

    /* Typically we process each input only once, but in the case of
     * completion mode we'll run through this inner while(1) twice. */
    while(1) {
        switch(l->state) {
        case ST_START:
            switch(c) {
            case 9:     /* tab */
                /* Only autocomplete if the callback is set */
                if (completionCallback != NULL) {
                    rc = completeLineStart(l);

                    /* We should transition the state to completion mode! */
                    if (rc == 0) {
                        l->state = ST_COMPLETION;
                    }
                }
                break;

            case 13:    /* enter */
                history_len--;
                free(history[history_len]);
                return -1;
            case 3:     /* ctrl-c */
                errno = EAGAIN;
                return -2;
            case 127:   /* backspace */
            case 8:     /* ctrl-h */
                linenoiseEditBackspace(l);
                break;
            case 4:     /* ctrl-d, remove char at right of cursor, or of the
                           line is empty, act as end-of-file. */
                if (l->len > 0) {
                    linenoiseEditDelete(l);
                } else {
                    history_len--;
                    free(history[history_len]);
                    return -2;
                }
                break;
            case 20:    /* ctrl-t, swaps current character with previous. */
                if (l->pos > 0 && l->pos < l->len) {
                    int aux = l->buf[l->pos-1];
                    l->buf[l->pos-1] = l->buf[l->pos];
                    l->buf[l->pos] = aux;
                    if (l->pos != l->len-1) l->pos++;
                    refreshLine(l);
                }
                break;
            case 2:     /* ctrl-b */
                linenoiseEditMoveLeft(l);
                break;
            case 6:     /* ctrl-f */
                linenoiseEditMoveRight(l);
                break;
            case 16:    /* ctrl-p */
                linenoiseEditHistoryNext(l, LINENOISE_HISTORY_PREV);
                break;
            case 14:    /* ctrl-n */
                linenoiseEditHistoryNext(l, LINENOISE_HISTORY_NEXT);
                break;
            case 27:    /* escape sequence */
                /* Prepare to handle the next two bytes as representing the escape sequence. */
                l->state = ST_ESC_SEQ_0;
                break;

            default:
                if (linenoiseEditInsert(l,c)) return 2;
                break;
            case 21: /* Ctrl+u, delete the whole line. */
                l->buf[0] = '\0';
                l->pos = l->len = 0;
                refreshLine(l);
                break;
            case 11: /* Ctrl+k, delete from current to end of line. */
                l->buf[l->pos] = '\0';
                l->len = l->pos;
                refreshLine(l);
                break;
            case 1: /* Ctrl+a, go to the start of the line */
                l->pos = 0;
                refreshLine(l);
                break;
            case 5: /* ctrl+e, go to the end of the line */
                l->pos = l->len;
                refreshLine(l);
                break;
            case 12: /* ctrl+l, clear screen */
                linenoiseClearScreen();
                refreshLine(l);
                break;
            case 23: /* ctrl+w, delete previous word */
                linenoiseEditDeletePrevWord(l);
                break;
            }
            break;

        case ST_COMPLETION:
            rc = completeLineHandle(l, c);

            /* Hit stop condition, we're done with completion but this
             * input still needs to be processed */
            if (rc) {
                /* Go back to the start to handle this input */
                l->state = ST_START;
                
                /* This 'continue' means we repeat the inner while(1) again */
                continue;
            }
            break;

        case ST_ESC_SEQ_0:
            l->seq[0] = c;
            l->state = ST_ESC_SEQ_1;
            break;

        case ST_ESC_SEQ_1:
            l->seq[1] = c;
            l->state = ST_START;

            if (l->seq[0] == 91 && l->seq[1] == 68) {
                /* Left arrow */
                linenoiseEditMoveLeft(l);
            } else if (l->seq[0] == 91 && l->seq[1] == 67) {
                /* Right arrow */
                linenoiseEditMoveRight(l);
            } else if (l->seq[0] == 91 && (l->seq[1] == 65 || l->seq[1] == 66)) {
                /* Up and Down arrows */
                linenoiseEditHistoryNext(l,
                    (l->seq[1] == 65) ? LINENOISE_HISTORY_PREV :
                                        LINENOISE_HISTORY_NEXT);
            } else if (l->seq[0] == 91 && l->seq[1] > 48 && l->seq[1] < 55) {
                /* extended escape, will require an additional two bytes to be read */
                l->state = ST_ESC_SEQ2_0;
            }
            break;

        case ST_ESC_SEQ2_0:
            l->seq2[0] = c;
            l->state = ST_ESC_SEQ2_1;
            break;

        case ST_ESC_SEQ2_1:
            l->seq2[1] = c;
                
            if (l->seq[1] == 51 && l->seq2[0] == 126) {
                /* Delete key. */
                linenoiseEditDelete(l);
            }

            l->state = ST_START;
            break;
        }

        /* Break out of the inner while(1) loop if we're completely
         * finished handling this input! */
        break;
    }

    return 0;
}

/* Setup a linenoise state structure */
static void linenoiseSetupState(struct linenoiseState *ls, int fd, char *buf, size_t buflen, const char *prompt)
{
    ls->fd = fd;
    ls->buf = buf;
    ls->buflen = buflen - 1; /* Make sure there is always space for the nulterm */
    ls->prompt = prompt;
    ls->plen = strlen(prompt);
    ls->oldpos = ls->pos = 0;
    ls->len = 0;
    ls->cols = getColumns();
    ls->maxrows = 0;
    ls->history_index = 0;
    ls->state = ST_START;

    /* Buffer starts empty. */
    ls->buf[0] = '\0';
}

/* This function is the core of the line editing capability of linenoise.
 * It expects 'fd' to be already in "raw mode" so that every key pressed
 * will be returned ASAP to read().
 *
 * The resulting string is put into 'buf' when the user type enter, or
 * when ctrl+d is typed.
 *
 * The function returns the length of the current buffer. */
static int linenoiseEdit(int fd, char *buf, size_t buflen, const char *prompt)
{
    struct linenoiseState l;

    linenoiseSetupState(&l, fd, buf, buflen, prompt);

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    linenoiseHistoryAdd("");
    
    if (write(fd,prompt,l.plen) == -1) return -1;
    while(1) {
        char c;
        int nread;
        int rc;

        nread = read(fd,&c,1);
        if (nread <= 0) return l.len;

        rc = linenoiseHandle(&l, c);

        if (rc == -1) {
            return l.len;
        }
        else if (rc == -2) {
            return -1;
        }
    }

    return -1;
}

/* This function calls the line editing function linenoiseEdit() using
 * the STDIN file descriptor set in raw mode. */
static int linenoiseRaw(char *buf, size_t buflen, const char *prompt) {
    int fd = STDIN_FILENO;
    int count;

    if (buflen == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!isatty(STDIN_FILENO)) {
        if (fgets(buf, buflen, stdin) == NULL) return -1;
        count = strlen(buf);
        if (count && buf[count-1] == '\n') {
            count--;
            buf[count] = '\0';
        }
    } else {
        if (enableRawMode(fd) == -1) return -1;
        count = linenoiseEdit(fd, buf, buflen, prompt);
        disableRawMode(fd);
        printf("\n");
    }
    return count;
}

/* The high level function that is the main API of the linenoise library.
 * This function checks if the terminal has basic capabilities, just checking
 * for a blacklist of stupid terminals, and later either calls the line
 * editing function or uses dummy fgets() so that you will be able to type
 * something even in the most desperate of the conditions. */
char *linenoise(const char *prompt) {
    char buf[LINENOISE_MAX_LINE];
    int count;

    if (isUnsupportedTerm()) {
        size_t len;

        printf("%s",prompt);
        fflush(stdout);
        if (fgets(buf,LINENOISE_MAX_LINE,stdin) == NULL) return NULL;
        len = strlen(buf);
        while(len && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
            len--;
            buf[len] = '\0';
        }
        return strdup(buf);
    } else {
        count = linenoiseRaw(buf,LINENOISE_MAX_LINE,prompt);
        if (count == -1) return NULL;
        return strdup(buf);
    }
}

/* ================================ History ================================= */

/* Free the history, but does not reset it. Only used when we have to
 * exit() to avoid memory leaks are reported by valgrind & co. */
static void freeHistory(void) {
    if (history) {
        int j;

        for (j = 0; j < history_len; j++)
            free(history[j]);
        free(history);
    }
}

/* At exit we'll try to fix the terminal to the initial conditions. */
static void linenoiseAtExit(void) {
    disableRawMode(STDIN_FILENO);
    freeHistory();
}

/* Using a circular buffer is smarter, but a bit more complex to handle. */
int linenoiseHistoryAdd(const char *line) {
    char *linecopy;

    if (history_max_len == 0) return 0;
    if (history == NULL) {
        history = malloc(sizeof(char*)*history_max_len);
        if (history == NULL) return 0;
        memset(history,0,(sizeof(char*)*history_max_len));
    }
    linecopy = strdup(line);
    if (!linecopy) return 0;
    if (history_len == history_max_len) {
        free(history[0]);
        memmove(history,history+1,sizeof(char*)*(history_max_len-1));
        history_len--;
    }
    history[history_len] = linecopy;
    history_len++;
    return 1;
}

/* Set the maximum length for the history. This function can be called even
 * if there is already some history, the function will make sure to retain
 * just the latest 'len' elements if the new history length value is smaller
 * than the amount of items already inside the history. */
int linenoiseHistorySetMaxLen(int len) {
    char **new;

    if (len < 1) return 0;
    if (history) {
        int tocopy = history_len;

        new = malloc(sizeof(char*)*len);
        if (new == NULL) return 0;

        /* If we can't copy everything, free the elements we'll not use. */
        if (len < tocopy) {
            int j;

            for (j = 0; j < tocopy-len; j++) free(history[j]);
            tocopy = len;
        }
        memset(new,0,sizeof(char*)*len);
        memcpy(new,history+(history_len-tocopy), sizeof(char*)*tocopy);
        free(history);
        history = new;
    }
    history_max_len = len;
    if (history_len > history_max_len)
        history_len = history_max_len;
    return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave(char *filename) {
    FILE *fp = fopen(filename,"w");
    int j;
    
    if (fp == NULL) return -1;
    for (j = 0; j < history_len; j++)
        fprintf(fp,"%s\n",history[j]);
    fclose(fp);
    return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int linenoiseHistoryLoad(char *filename) {
    FILE *fp = fopen(filename,"r");
    char buf[LINENOISE_MAX_LINE];
    
    if (fp == NULL) return -1;

    while (fgets(buf,LINENOISE_MAX_LINE,fp) != NULL) {
        char *p;
        
        p = strchr(buf,'\r');
        if (!p) p = strchr(buf,'\n');
        if (p) *p = '\0';
        linenoiseHistoryAdd(buf);
    }
    fclose(fp);
    return 0;
}

int linenoiseSetupContext(linenoiseContext **context, int fd, const char *prompt)
{
    struct linenoiseState *l;

    if (context == NULL) {
        return -1;
    }

    /* We need to create the context now */
    if (*context == NULL) {
        l = (struct linenoiseState *) malloc(sizeof (*l));

        /* Failed to allocate memory */
        if (l == NULL) {
            return -1;
        }

        size_t buflen = LINENOISE_MAX_LINE;
        char *buf = (char *) malloc(buflen);

        if (buf == NULL) {
            free(l);
            return -1;
        }

        linenoiseSetupState(l, fd, buf, buflen, prompt);

        *context = (linenoiseContext *) l;
    }

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    linenoiseHistoryAdd("");

    return 0;
}

int linenoiseSetPrompt(linenoiseContext *context, const char *prompt)
{
    if (context == NULL) {
        return -1;
    }

    struct linenoiseState *l = (struct linenoiseState *) context;

    l->prompt = prompt;
    l->plen = strlen(prompt);
    refreshLine(l);

    return 0;
}

int linenoiseStartInput(linenoiseContext *context)
{
    if (context == NULL) {
        return -1;
    }

    struct linenoiseState *l = (struct linenoiseState *) context;

    if (enableRawMode(l->fd) == -1) return -1;

    linenoiseSetupState(l, l->fd, l->buf, l->buflen, l->prompt);
    refreshLine(l);

    return 0;
}

int linenoiseHandleInput(linenoiseContext *context, char c, char **line)
{
    if (context == NULL) {
        return -1;
    }

    struct linenoiseState *l = (struct linenoiseState *) context;

    if (line == NULL) {
        return -1;
    }

    *line = NULL;
    int rc = linenoiseHandle(l, c);

    if (rc == -1) {
        char *output = strndup(l->buf, l->len + 1);
        if (output == NULL) return -1;

        output[l->len] = '\0';
        *line = output;
        disableRawMode(l->fd);
        printf("\n");
    }
    else if (rc == -2) {
        disableRawMode(l->fd);
        printf("\n");
        return -1;
    }

    return 0;
}

int linenoiseClearLine(linenoiseContext *context)
{
    if (context == NULL) {
        return -1;
    }

    struct linenoiseState *l = (struct linenoiseState *) context;

    char seq[64];

    /* Cursor to left edge, erase to right, cursor back to left edge */
    int seqsize = snprintf(seq,sizeof(seq),"\x1b[0G" "\x1b[0K" "\x1b[0G");
    if (write(l->fd,seq,seqsize) == -1) return -1;

    return 0;
}

int linenoiseRefreshLine(linenoiseContext *context)
{
    if (context == NULL) {
        return -1;
    }

    struct linenoiseState *l = (struct linenoiseState *) context;
    refreshLine(l);

    return 0;
}

