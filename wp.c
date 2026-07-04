// wp - VT100 word processor loosely based on antirez's kilo
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>

/* Defines */

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum editorMessage {
    SAVING = 1,
    SAVED
};

/* Editor */

typedef struct erow {
  int size;
  char *chars;
} erow;

typedef struct elline {
  int size;
  int refIdx;
  int offset;
} elline;

struct editorConfig {
    int cx, cy; // cursor position
    int scroll;

    int screenrows;
    int screencols;

    int insMode;
    char lastKeypress;

    int charcount;
    int wordcount;

    int statusMessage;

    int numrows;
    erow *row;

    int numllines;
    elline *lline;

    char *filename;

    struct termios backup_termios;
};

struct editorConfig E; // the editor state

/* Terminal */

int currentRow() {
    return E.cy+E.scroll;
}

int physicalRow() {
    if (currentRow() == E.numllines) return E.numrows;
    return E.lline[currentRow()].refIdx;
}

int physicalRowOffset() {
    if (currentRow() == E.numllines) return E.numrows;
    return E.lline[currentRow()].offset;
}

void clearScreen() {
    write(STDOUT_FILENO, "\x1b[2J", 4); // Erase In Display, entire screen
    write(STDOUT_FILENO, "\x1b[H", 3); // Cursor Position, zero
}

void die(const char *s) {
    clearScreen();
    perror(s);
    exit(1);
}

void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.backup_termios);
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.backup_termios) == -1) die("tcgetattr");

    atexit(disableRawMode);

    struct termios raw = E.backup_termios;

    // we turn off:
    // 1. the terminal echoing characters
    // 2. canonical mode which is input sent on newline
    // 3. signals like ctrlc and ctrlz
    // 4. ctrlv and ctrlo which fucks stuff up
    // 5. xon so ctrls and ctrlq which stop/resume software flow 
    // 6. crnl because ctrl+m (carriage return) is being translated to newline
    // 7. all output post processing (opost)

    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN); // local flags
    raw.c_iflag &= ~(IXON | ICRNL); // input flags
    raw.c_oflag &= ~(OPOST); // output flags

    // Legacy raw mode, idk what that does but i have to carry the flame
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP);
    raw.c_cflag |= (CS8);

    // handle timeout for read
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    // we ask the terminal where the fuck the cursor is
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    // we decipher the response
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    
    // We get the size of the terminal using the trivial TIOCGWINSZ command (Terminal Input Output Control Get WINdow SiZe)
      if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // If TIOCGWINSZ fails, move the cursor to the depths of hell and pick him up
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/* logical line processing */

void editorAppendLLine(int refIdx, int start, size_t len) {
    E.lline = realloc(E.lline, sizeof(elline) * (E.numllines + 1));
    int at = E.numllines;
    E.lline[at].size = len;
    E.lline[at].refIdx = refIdx;
    E.lline[at].offset = start;
    E.numllines++;
}

void editorComputeLLines() {
    free(E.lline);
    E.numllines = 0;
    E.lline = NULL;
    for (int i = 0; i < E.numrows; ++i) {
        int len = E.row[i].size;
        int x = 0;
        while (len >= E.screencols) {
            editorAppendLLine(i, x, E.screencols-1);
            x += E.screencols-1;
            len -= E.screencols-1;
        }
        editorAppendLLine(i, x, len);
    }
}

/* rows */

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

void editorInsertRow(char *s, size_t len, int pos) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[pos + 1], &E.row[pos], sizeof(erow) * (E.numrows - pos)); // actually shift all the rows after pos by one
    E.row[pos].size = len;
    E.row[pos].chars = malloc(len + 1);
    memcpy(E.row[pos].chars, s, len);
    E.row[pos].chars[len] = '\0';
    E.numrows++;
}

void editorRemoveRow(int pos) {
    memmove(&E.row[pos], &E.row[pos + 1], sizeof(erow) * (E.numrows - pos)); // actually shift all the rows after pos by minus one
    E.numrows--;
}

/* append buffer, reinventing the string */

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/* output processing */

void editorDrawStatusBar(struct abuf *ab) {
    // render the status bar
    abAppend(ab, "} wp word processor  ", 21);

    char tmp[32];
    int len;

    if (E.lastKeypress > ' ' && E.lastKeypress < '~') {
        len = snprintf(tmp, sizeof(tmp), "%c", E.lastKeypress);
    } else {
        len = snprintf(tmp, sizeof(tmp), "0x%X", E.lastKeypress);
    }

    char buf[256];
    //len = snprintf(buf, sizeof(buf), "%*s", E.screencols-22, tmp);

    len = snprintf(buf, sizeof(buf), "line %d of %d ~ %d/%d ~ %dc %dw ", 
        physicalRow()+1,
        E.numrows,
        (currentRow() == E.numllines) ? 0 : E.cx + physicalRowOffset(), 
        (currentRow() == E.numllines) ? 0 : E.row[physicalRow()].size,
        E.charcount,
        E.wordcount
    );

    abAppend(ab, buf, len);


    if (E.statusMessage != 0) {
        char msg[256];
        switch (E.statusMessage) {
            case SAVING:
                len = snprintf(msg, sizeof(msg), "Saving...");
                break;
            case SAVED:
                len = snprintf(msg, sizeof(msg), "Saved");
                break;
        }

        abAppend(ab, msg, len);
    }
    
}

void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; ++y) {
        if (y == E.screenrows - 1) {
            editorDrawStatusBar(ab);
        } else if (y >= E.numllines-E.scroll) {
            abAppend(ab, "~", 3);
        } else {
            int len = E.lline[y+E.scroll].size;
            int refIdx = E.lline[y+E.scroll].refIdx;
            int start = E.lline[y+E.scroll].offset;
            char *buf;
            buf = malloc(len);
            memcpy(buf, E.row[refIdx].chars + start, len);
            
            abAppend(ab, buf, len);
            free(buf);
        }

        abAppend(ab, "\x1b[K", 3); // Erase In Line

        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // Hides the cursor
    
    abAppend(&ab, "\x1b[H", 3);  // Cursor Position, zero

    editorDrawRows(&ab);

    // move the cursor where we left it
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // Brings the cursor back!

    write(STDOUT_FILENO, ab.b, ab.len); // print the whole buffer
    abFree(&ab);
}

/* file i/o */

void editorOpen(char *filename) {
    // open file
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");
    
    while (1) {
        char *line = NULL;
        size_t linecap = 0;
        ssize_t linelen;

        // Getline does the memory management for me and returns the line length and the allocated memory
        linelen = getline(&line, &linecap, fp);

        if (linelen != -1) {
            while (linelen > 0 && (line[linelen - 1] == '\n' ||
                                line[linelen - 1] == '\r'))
            linelen--;
            E.charcount += linelen;
            editorAppendRow(line, linelen);

            free(line);
        } else break;
    }

    editorComputeLLines();
    
    fclose(fp);
}

void editorSave(char *filename) {
    // open file
    E.statusMessage = SAVING;
    editorRefreshScreen();
    FILE *fp = fopen(filename, "w");
    if (!fp) die("fopen");
    
    for (int i = 0; i < E.numrows; ++i) {
        fwrite(E.row[i].chars, sizeof E.row[i].chars[0], E.row[i].size, fp);
        fputc('\r', fp);fputc('\n', fp);
    }

    fclose(fp);
    E.statusMessage = SAVED;
}

/* input processing */

int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    // process escape characters: if we read 0x1b we read the next 2 characters
    // theres surprisingly many ways to do home and end
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                switch (seq[1]) {
                    case '1': return HOME_KEY;
                    case '3': return DEL_KEY;
                    case '4': return END_KEY;

                    case '5': return PAGE_UP;
                    case '6': return PAGE_DOWN;

                    case '7': return HOME_KEY;
                    case '8': return END_KEY;
                }
                }
            } else {
                switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            // <esc>-O???
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        // just the escape character, somehow?
        return '\x1b';
    } else {
        return c;
    }
}

void editorMoveCursor(int key) {
    // move and clamp cursor position.
    // handle scrolling, wrapping.
    switch (key) {
        case ARROW_LEFT:
            if (E.cx == 0 && currentRow() != 0) {
                int oldRow = physicalRow();
                // balance scroll
                if (E.cy != 0) {
                    E.cy--;
                } else if (E.scroll != 0) {
                    E.scroll--;
                }

                // shift it to the last character if this is a different physical line, otherwise skip last position
                if (oldRow == physicalRow())
                    E.cx = E.lline[currentRow()].size-1;
                else
                    E.cx = E.lline[currentRow()].size;
            } else if (E.cx != 0)
                // if not at the first position, go left
                E.cx--;
            break;
        case ARROW_RIGHT:
            if (currentRow() != E.numllines) {
                if (E.cx == E.lline[currentRow()].size || (E.cx == E.lline[currentRow()].size - 1 && currentRow() != E.numllines-1 && E.lline[currentRow()+1].offset != 0)) {
                    // if this is either:
                    // 1. the last character in the last lline of a row
                    // 2. the second to last character of NOT the last lline in a row, or the last lline in general
                    // then change line and go to start of lline

                    // balance scroll
                    if (E.cy != E.screenrows - 2) {
                        E.cy++;
                    } else {
                        E.scroll++;
                    }

                    // dirty fix if it made a mistake
                    if (E.cx == E.lline[currentRow()-1].size && currentRow()-1 != E.numllines-1 && E.lline[currentRow()].offset != 0)
                        E.cx = 1;
                    else
                        E.cx = 0;
                    
                } else 
                    // go right normally
                    E.cx++;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            } else if (E.scroll != 0) {
                E.scroll--;
            }
            if (currentRow() != E.numllines && E.cx > E.lline[currentRow()].size) {
                E.cx = E.lline[currentRow()].size;
            }
            break;
        case ARROW_DOWN:
            if (currentRow() != E.numllines) {
                if (E.cy != E.screenrows - 2) {
                    E.cy++;
                } else {
                    E.scroll++;
                }
                if (currentRow() != E.numllines && E.cx > E.lline[currentRow()].size) {
                    E.cx = E.lline[currentRow()].size;
                } else if (currentRow() == E.numllines) E.cx = 0;
            }
            break;
        case HOME_KEY:
            {
                // we go back until the lline that starts the row
                int homeLLine = currentRow();
                while (E.lline[homeLLine].offset != 0) homeLLine--;

                // we set the x to 0 of course
                E.cx = 0;

                // we move the cursor there, accounting for scroll
                E.cy = homeLLine-E.scroll;

                // if the cursor is off screen because of the scroll, scroll until it's at the top again
                if (E.cy < 0) {
                    E.scroll += E.cy;
                    E.cy = 0;
                }
            }
            break;
        case END_KEY:
            if (currentRow() != E.numllines) {
                // we go forward until the lline that starts the NEXT row, then we go back one lline
                int endLLine = currentRow();
                while (endLLine != E.numllines && E.lline[endLLine].refIdx == E.lline[currentRow()].refIdx) endLLine++;
                endLLine--;

                // we set the x to the length of the lline
                E.cx = E.lline[endLLine].size;

                // we move the cursor there, accounting for scroll
                E.cy = endLLine-E.scroll;

                // if the cursor is off screen because of the scroll, scroll until it's at the bottom again
                if (E.cy > E.screencols) {
                    E.scroll -= (E.cy - E.screencols + 1);
                    E.cy = E.screencols - 1;
                }
            }
            break;
    }
}

void editorType(int c) {
    if (currentRow() != E.numllines) {
        int row = physicalRow();
        int pos = physicalRowOffset() + E.cx;

        int len = E.row[row].size;

        // make space for the new letter, unless replacing an existing one
        if (!E.insMode || pos == len) {
            E.row[row].size = len+1;
            E.row[row].chars = realloc(E.row[row].chars, len+1);
            memmove(E.row[row].chars + pos + 1, E.row[row].chars + pos, len-pos);
        }

        // put the new letter in!
        E.row[row].chars[pos] = c;
    } else {
        // if typing on a tilde-row, spawn a new row and add the letter
        char buf[1];
        buf[0] = c;
        editorAppendRow(buf, 1);
    }
    // recompute the logical lines
    editorComputeLLines();

    // move the cursor to the right
    editorMoveCursor(ARROW_RIGHT);

    // increment the character count
    E.charcount++;
}

void editorNewline() {
    int row = physicalRow();
    int pos = physicalRowOffset() + E.cx;

    if (currentRow() != E.numllines) {
        int len = E.row[row].size;
        char *buf;
        buf = malloc(len);
        memcpy(buf, E.row[row].chars, len);

        int lh = len;
        if (pos < lh) lh = pos;

        editorInsertRow(buf, lh, row);

        int rh = len-lh;
        free(E.row[row+1].chars);
        E.row[row+1].chars  = malloc(rh);
        memcpy(E.row[row+1].chars, buf+lh, rh);
        free(buf);
        E.row[row+1].size = rh;
        
        if (E.cy != E.screenrows - 2) {
            E.cy++;
        } else {
            E.scroll++;
        }

        E.cx = 0;
    } else {
        // if doing newline on a tilde-row, spawn a new empty row
        editorAppendRow(NULL, 0);
        E.cx = 0; // should alr be 0
        // go to the newline and balance for scroll
        if (E.cy != E.screenrows - 2) {
            E.cy++;
        } else {
            E.scroll++;
        }
    }
    editorComputeLLines();
}

void editorMergeLines(int row) {
    // merges a row with the one before it.
    
    if (row != E.numrows) {
        char *buf;
        int len = E.row[row].size + E.row[row-1].size;
        buf = malloc(len);

        mempcpy(buf, E.row[row-1].chars, E.row[row-1].size);
        mempcpy(buf+E.row[row-1].size, E.row[row].chars, E.row[row].size);

        E.row[row-1].chars = realloc(E.row[row-1].chars, len);
        memcpy(E.row[row-1].chars, buf, len);
        free(buf);
        E.row[row-1].size = len;

        editorRemoveRow(row);
    }

    editorComputeLLines();
}

void editorBackspace() {
    if (currentRow() == E.numllines) {
        editorMoveCursor(ARROW_LEFT);
        return;
    }

    int row = physicalRow();
    int pos = physicalRowOffset() + E.cx;

    if (pos != 0) {
        // can delete characters before it
        int old_ref = E.lline[currentRow()].refIdx;
        memmove(E.row[row].chars + pos - 1, E.row[row].chars + pos, E.row[row].size-pos);
        E.row[row].size--;
        editorComputeLLines();

        editorMoveCursor(ARROW_LEFT);

        // fix the fact that when you empty an lline it gets confused and doesn't go up because it,
        //rightfully, sticks to the zeroth character, forgetting that now it's not the EOL
        if (E.cx == 0 && (currentRow() == E.numllines || E.lline[currentRow()].refIdx != old_ref)) editorMoveCursor(ARROW_LEFT);
        E.charcount--;
    } else if (currentRow() != 0) {
        // merge two lines, deleting the newline, puts the cursor after the first line
        E.cx =  E.lline[currentRow()-1].size;

        if (E.cy != 0) {
            E.cy--;
        } else if (E.scroll != 0) {
            E.scroll--;
        }

        editorMergeLines(row);
    }
}

void editorDelete() {
    if (currentRow() == E.numllines) return;

    int row = physicalRow();
    int pos = physicalRowOffset() + E.cx;

    if (E.cx != E.lline[currentRow()].size) {
        // can delete characters after it
        memmove(E.row[row].chars + pos, E.row[row].chars + pos + 1, E.row[row].size-pos - 1);
        E.row[row].size--;
        editorComputeLLines();

        E.charcount--;
    } else if (currentRow() != E.numllines-1) {
        // merge two lines, deleting the newline, puts the cursor after the first line
        editorMergeLines(row+1);
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();
    E.lastKeypress = c;

    switch (c) {
        case CTRL_KEY('q'):
            // Ctrl-Q to quit
            clearScreen();
            exit(0);
            break;
        case CTRL_KEY('s'):
            // Ctrl-S to save
            editorSave(E.filename);
            break;
        case PAGE_UP:
        case PAGE_DOWN:
        {
            // pageup moves the cursor by the whole height of the screen
            int times = E.screenrows;
            while (times--)
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
        case HOME_KEY:
        case END_KEY:
            editorMoveCursor(c);
            break;
        case BACKSPACE:
            editorBackspace();
            break;
        case DEL_KEY:
            editorDelete();
            break;
        case '\r':
            editorNewline();
            break;
        default:
            editorType(c);
    }
}

/* init */

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;
    E.row = NULL;
    E.numllines = 0;
    E.lline = NULL;
    E.scroll = 0;
    E.insMode = 0;
    E.wordcount = 0;
    E.charcount = 0;
    E.lastKeypress = 0x0;
    E.statusMessage = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();

    if (argc == 2) {
        E.filename = argv[1];
        editorOpen(E.filename);
    } else {
        E.filename = "test.txt";
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}