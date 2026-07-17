// wp - VT100 word processor loosely based on antirez's kilo
//
// planned features:
// (x) basic text editing
// (x) file open/save
// (x) word line wrapping
// (x) utf8 support
// ( ) find/replace
// ( ) a nice status bar
// ( ) ctrl+arrows to move between words (horizontal) or phys rows (vertical), ctrl+backspace/del to delete a word
// ( ) shift+arrows to select, shift+del to delete a physical row
// ( ) indentation on the first lline of phys rows
// ( ) history, undo/redo
// ( ) somewhat decent performance

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
    ARROW_LEFT = 256,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    ESCAPE_KEY,
    UNKNOWN_TWO_CHAR = 0x222,
    UNKNOWN_THREE_CHAR = 0x333
};

enum editorMessage {
    SAVING = 1,
    SAVED
};

/* Editor */

typedef struct erow {
  int size;
  char *chars;

  int columns; // utf-8 size
} erow;

typedef struct elline {
    int refIdx;

    int size; // strictly in bytes
    int phys_size;
    
    int offset; // strictly in bytes
    int phys_offset;
} elline;

struct editorConfig {
    int cx, cy; // cursor position
    int bidx;
    int scroll;

    int screenrows;
    int screencols;

    int editflag;
    int insMode;
    int lastkey;

    int bytecount; // do I need this?
    int charcount;
    int wordcount;

    int statusmsg;

    int numrows;
    erow *row;

    int numllines;
    elline *lline;

    char *filename;

    struct termios backup_termios;
};

struct editorConfig E; // the editor state

/* utf-8 utils */
size_t utf8_next(const char *s, size_t pos) {
    int i = 1;
    while ((s[pos+i] & 0xC0) == 0x80) {
        i++;
    }
    return i;
}

size_t utf8_prev(const char *s, size_t pos) {
    if (pos == 0) return 0;
    int i = 1;
    while ((s[pos-i] & 0xC0) == 0x80) i++;
    return i;
}

/* Terminal */

int currentLLine() {
    return E.cy+E.scroll;
}

int physicalRow() {
    if (currentLLine() == E.numllines) return -1;
    return E.lline[currentLLine()].refIdx;
}

int physicalRowOffset() {
    if (currentLLine() == E.numllines) return 0;
    return E.lline[currentLLine()].offset;
}

int physicalRowColumnOffset() {
    if (currentLLine() == E.numllines) return 0;
    return E.lline[currentLLine()].phys_offset;
}

void clearScreen() {
    write(STDOUT_FILENO, "\e[2J", 4); // Erase In Display, entire screen
    write(STDOUT_FILENO, "\e[H", 3); // Cursor Position, zero
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
    if (write(STDOUT_FILENO, "\e[6n", 4) != 4) return -1;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    // we decipher the response
    buf[i] = '\0';
    if (buf[0] != '\e' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    
    // We get the size of the terminal using the trivial TIOCGWINSZ command (Terminal Input Output Control Get WINdow SiZe)
      if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // If TIOCGWINSZ fails, move the cursor to the depths of hell and pick him up
        if (write(STDOUT_FILENO, "\e[999C\e[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/* logical line processing */

int isPunctuation(char c) {
    // can i wrap on this character?
    return c == ' ' || c == ',' || c == '.' || c == ';' || c == ':' || c == '/' || c == '-' || c == '?' || c == '!';
}

void editorAppendLLine(int refIdx, int start, size_t len, int startcol, size_t collen) {
    E.lline = realloc(E.lline, sizeof(elline) * (E.numllines + 1));
    int at = E.numllines;
    E.lline[at].size = len;
    E.lline[at].refIdx = refIdx;
    E.lline[at].offset = start;
    E.lline[at].phys_size = collen;
    E.lline[at].phys_offset = startcol;
    E.numllines++;
}

// TODO: maybe don't recompute ALL THE LLINES, iterating EVERY CHARACTER, for every keypress?
void editorComputeLLines() {
    free(E.lline);

    // We also count lines, words and characters while recomputing llines, since we go over all of them.
    E.numllines = 0;
    E.wordcount = 0;
    E.charcount = 0;

    E.lline = NULL;
    for (int y = 0; y < E.numrows; ++y) {
        int len = E.row[y].size;
        E.row[y].columns = 0;

        E.bytecount += len; // add the row's size (in bytes) to the byte counter

        // if the row is too long, wrap it
        int lastspc = 0;
        int llen = 0; // length of current logical line
        int lstart = 0; // start of the current logical line

        int lastcol = 0;
        int columns = 0; // the actual rendered columns, for utf-8

        for (int x = 0; x < len;) {
            /*
            // divide at punctuation
            if (isPunctuation(E.row[y].chars[x]))  {
                // cutting here would overflow. cut at the last valid position.
                // if the last word is too long to fit in the line anyway, even if it was alone, then it is to be split
                if (x-lstart+1 >= E.screencols 
                    //&& x-lastspc < E.screencols
                ) {
                    if (lastspc-lstart+1 >= E.screencols) {
                        // if the single word is larger than the goddamn screen split it
                        editorAppendLLine(y, lstart, E.screencols-1);
                        lstart += E.screencols-1;
                    } else {
                        // wrap normally
                        editorAppendLLine(y, lstart, lastspc-lstart+1);
                        lstart = lastspc+1;
                    }
                }

                lastspc = x; // blocks of punctuation stick together.
                //while (isPunctuation(E.row[y].chars[x++]));

                E.wordcount++;
            }
            */
            if (columns == 30) {
                editorAppendLLine(y, lastspc, x-lastspc, lastcol, columns);
                lastspc = x;
                lastcol += columns;
                columns = 0;
            }
            int add = utf8_next(E.row[y].chars, x);
            if (add == 0) break;
            x += add;
            columns++;
            E.charcount++;
            E.row[y].columns++;
        }

        editorAppendLLine(y, lastspc, len-lastspc, lastcol, columns);
    }
}

/* rows processing */

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

void editorDrawStatbar(struct abuf *ab) {
    // render the status bar
    abAppend(ab, "\e[0;30m\e[47m} wp word processor  ", 33);

    char tmp[32];
    int len;

    if (E.lastkey > ' ' && E.lastkey < '~') {
        len = snprintf(tmp, sizeof(tmp), "%c", E.lastkey);
    } else {
        len = snprintf(tmp, sizeof(tmp), "0x%X", E.lastkey);
    }

    char buf[256];
    len = snprintf(buf, sizeof(buf), " [ %s ] ", tmp);

    abAppend(ab, buf, len);

    len = snprintf(buf, sizeof(buf), "line %d of %d ~ %d (%d)/%d (%d) ~ %dc %dw %s", 
        physicalRow()+1,
        E.numrows,
        (currentLLine() == E.numllines) ? 0 : E.cx + physicalRowColumnOffset(), 
        (currentLLine() == E.numllines) ? 0 : E.bidx, 
        (currentLLine() == E.numllines) ? 0 : E.row[physicalRow()].columns,
        (currentLLine() == E.numllines) ? 0 : E.row[physicalRow()].size,
        E.charcount,
        E.wordcount,
        E.editflag ? "* " : ""
    );

    abAppend(ab, buf, len);


    if (E.statusmsg != 0) {
        switch (E.statusmsg) {
            case SAVING:
                abAppend(ab, "Saving...", 9);
                break;
            case SAVED:
                abAppend(ab, "Saved", 5);
                break;
        }
    }
    
    abAppend(ab, "\e[0m", 4);
}

void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; ++y) {
        if (y == E.screenrows - 1) {
            editorDrawStatbar(ab);
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

        abAppend(ab, "\e[K", 3); // Erase In Line

        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\e[?25l", 6); // Hides the cursor
    
    abAppend(&ab, "\e[H", 3);  // Cursor Position, zero

    editorDrawRows(&ab);

    // move the cursor where we left it
    char buf[32];
    snprintf(buf, sizeof(buf), "\e[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\e[?25h", 6); // Brings the cursor back!

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
    E.statusmsg = SAVING;
    editorRefreshScreen();
    FILE *fp = fopen(filename, "w");
    if (!fp) die("fopen");
    
    for (int i = 0; i < E.numrows; ++i) {
        fwrite(E.row[i].chars, sizeof E.row[i].chars[0], E.row[i].size, fp);
        fputc('\r', fp);fputc('\n', fp);
    }

    fclose(fp);
    E.editflag = 0;
    E.statusmsg = SAVED;
}

/* input processing */

int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    // process escape characters: if we read 0e we read the next 2 characters
    // theres surprisingly many ways to do home and end
    if (c == '\e') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return ESCAPE_KEY; // the actual escape key
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return UNKNOWN_TWO_CHAR;
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\e';
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
                    case 'F': return END_KEY;
                    case 'H': return HOME_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            // <esc>-O??? I think this is for the numpad home/enter for the VT100
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return UNKNOWN_THREE_CHAR;
    } else {
        return c;
    }
}

void recomputeByteOffset() {
    if (currentLLine() >= E.numllines) {
        E.bidx = 0;
        return;
    }
    
    elline *l = &E.lline[currentLLine()];
    E.bidx = l->offset;
    char *chars = E.row[l->refIdx].chars;
    
    for (int i = 0; i < E.cx; ++i) {
        E.bidx += utf8_next(chars, E.bidx);
    }
}

void editorScrollView() {
    int usefulHeight = E.screenrows - 2;
    if (usefulHeight < 0) usefulHeight = 0; // hmmmm

    int targetLine = currentLLine();
    if (targetLine < 0) targetLine = 0;
    if (targetLine > E.numllines) targetLine = E.numllines;

    if (targetLine < E.scroll) {
        E.scroll = targetLine;
        E.cy = 0;
    } else if (targetLine > E.scroll + usefulHeight) {
        E.scroll = targetLine - usefulHeight;
        E.cy = usefulHeight;
    } else {
        E.cy = targetLine - E.scroll;
    }
}

int maxCx(int llineIdx) {
    int disallowed = (llineIdx + 1 < E.numllines) &&
                      (E.lline[llineIdx + 1].refIdx == E.lline[llineIdx].refIdx);
    return disallowed ? E.lline[llineIdx].phys_size - 1 : E.lline[llineIdx].phys_size;
}

void editorMoveCursor(int key) {
    int recompute = 0;

    switch (key) {
        case ARROW_LEFT:
            if (E.cx > 0) {
                E.cx--;
                if (physicalRow() != -1) {
                    E.bidx -= utf8_prev(E.row[physicalRow()].chars, E.bidx);
                }
            } else if (currentLLine() > 0) {
                int old_row = physicalRow();
                E.cy--;
                int prev_row = physicalRow();
                
                E.cx = (prev_row == old_row) ? (E.lline[currentLLine()].phys_size - 1) : E.lline[currentLLine()].phys_size;
                recompute = 1;
            }
            break;

        case ARROW_RIGHT:
            if (currentLLine() != E.numllines) {
                elline *l = &E.lline[currentLLine()];

                if (E.cx >= maxCx(currentLLine())) {
                    if (currentLLine() != E.numllines - 1) {
                        E.cy++;
                        E.cx = (E.cx == l->phys_size && E.lline[currentLLine()].offset != 0) ? 1 : 0;
                        recompute = 1;
                    }
                } else {
                    E.cx++;
                    E.bidx += utf8_next(E.row[physicalRow()].chars, E.bidx);
                }
            }
            break;

        case ARROW_UP:
            if (currentLLine() > 0) {
                E.cy--;
                if (E.cx > E.lline[currentLLine()].phys_size) {
                    E.cx = E.lline[currentLLine()].phys_size;
                }
                int m = maxCx(currentLLine()); if (E.cx > m) E.cx = m;
                recompute = 1;
            }
            break;

        case ARROW_DOWN:
            if (currentLLine() != E.numllines) {
                E.cy++;
                if (currentLLine() < E.numllines) {
                    if (E.cx > E.lline[currentLLine()].phys_size) {
                        E.cx = E.lline[currentLLine()].phys_size;
                    }
                    int m = maxCx(currentLLine()); if (E.cx > m) E.cx = m;
                } else {
                    E.cx = 0;
                }
                recompute = 1;
            }
            break;

        case HOME_KEY:
            while (currentLLine() > 0 && E.lline[currentLLine()].offset != 0) {
                E.cy--;
            }
            E.cx = 0;
            recompute = 1;
            break;

        case END_KEY:
            if (currentLLine() < E.numllines) {
                int target_row = E.lline[currentLLine()].refIdx;
                while (currentLLine() < E.numllines && E.lline[currentLLine()].refIdx == target_row) {
                    E.cy++;
                }
                E.cy--;
                E.cx = E.lline[currentLLine()].phys_size;
                recompute = 1;
            }
            break;
    }

    editorScrollView();

    if (1) {
        recomputeByteOffset();
    }
}

// TODO (URGENT): fix speed. this goes through ALL ROWS EVERY KEYPRESS. 
// AND it allows for the disallowed position at the end of the row.
void repositionCursor(int row, int byte) {
    int y=0;
    while (y < E.numllines && E.lline[y].refIdx != row)
        y++;

    if (y == E.numllines) return;
    
    while (y + 1 < E.numllines &&
           E.lline[y + 1].refIdx == row &&
           E.lline[y + 1].offset <= byte)
    {
        y++;
    }

    E.cy = y - E.scroll;
    E.bidx = byte;  
    
    int b_offset = E.lline[y].offset;
    E.cx = 0;

    while (b_offset < byte) {
        b_offset += utf8_next(E.row[E.lline[y].refIdx].chars, b_offset);
        E.cx++;
    }

    
}

void editorType(int c) {
    int row = physicalRow();
    int pos = E.bidx;

    if (currentLLine() != E.numllines) {
        int len = E.row[row].size;

        // make space for the new byte, unless replacing an existing one
        if (!E.insMode || pos == len) {
            E.row[row].size = len+1;
            E.row[row].chars = realloc(E.row[row].chars, len+1);

            // shift all the bytes after this forward by one
            memmove(E.row[row].chars + pos + 1, E.row[row].chars + pos, len-pos);
        }

        // put the new letter in!
        E.row[row].chars[pos] = c;

        // move the logical byte cursor forward
        E.bidx++;
    } else {
        // if typing on a tilde-row, spawn a new row and add the letter
        char buf[1];
        buf[0] = c;
        row = E.numrows;

        // spawn a new row
        editorAppendRow(buf, 1);
        
        // move the logical byte cursor forward
        E.bidx++;
    }
    
    // recompute the logical lines
    editorComputeLLines();

    // reposition the cursor
    repositionCursor(row, E.bidx);
    
    // trigger the edit flag
    E.editflag = 1;
}

void editorNewline() {
    int row = physicalRow();
    int pos = E.bidx; // This is in bytes ! It's the byte offset in the line.

    if (currentLLine() != E.numllines) {
        // if doing newline mid-row, we have to split it
        // if it's at the last character, we still split it, but don't do anything with the right hand.

        int len = E.row[row].size;

        // we create a buffer where we copy the row's content, since the original will be lost
        char *buf;
        buf = malloc(len);
        memcpy(buf, E.row[row].chars, len);

        // the left-hand of the split - gets as long as where the cursor is.
        int lh = pos;

        // we create a new row at the same position (pushing the other forward)
        // and fill it with the left-hand part's content.
        editorInsertRow(buf, lh, row);

        // the right-hand of the split - starts at byte lh, and is rh bytes long.
        int rh = len-lh;

        // begin the procedure to replace the new row (actually the old one, but pushed forward)'s content.
        // free old content
        free(E.row[row+1].chars);
        // malloc new buffer - length rh
        E.row[row+1].chars = malloc(rh);
        // copy the right-hand section of the buffer (the part that was left behind earlier)
        memcpy(E.row[row+1].chars, buf+lh, rh);
        // adjust the row's size
        E.row[row+1].size = rh;

        // free the buffer obviously
        free(buf);
    } else {
        // if doing newline on a tilde-row, spawn a new empty row
        editorAppendRow(NULL, 0);
  
    }

    // recompute the logical lines
    editorComputeLLines();

    // reposition the cursor to the new line
    repositionCursor(row+1, 0);

    E.editflag = 1;
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
    // If pressing backspace on a newline, just go to the line before.
    if (currentLLine() == E.numllines) {
        // move cursor up
        E.cy--;

        // move the cursor at the end of the line
        E.cx = E.lline[currentLLine()].phys_size;

        // adjust scrolling
        editorScrollView();
        return;
    }

    int row = physicalRow();
    int pos = E.bidx;

    // How many bytes need to be deleted
    int delta = utf8_prev(E.row[row].chars, pos);

    if (pos != 0) {
        // can delete characters before it

        // move all the bytes right before this position back by the delta, removing the bytes in the delta from existance
        memmove(E.row[row].chars + pos - delta, E.row[row].chars + pos, E.row[row].size - pos);
        E.row[row].size -= delta;

        // recompute the logical lines
        editorComputeLLines();

        // move the logical byte cursor backward
        E.bidx-=delta;

        // reposition the cursor
        repositionCursor(row, E.bidx);
    } else if (currentLLine() != 0) {
        // if at the first position and not in the first line, 
        // merge two lines, deleting the newline, puts the cursor after the first line

        // if it will land in The Cursed Position then just don't move the cursor at all
        if (E.lline[currentLLine()-1].phys_size != E.screencols) { // watch out: when ill do words, it will have to use a "wrap" property in the elline (TODO)
            // move cursor up
            E.cy--;

            // move the cursor at the end of the line
            E.cx = E.lline[currentLLine()].phys_size;
        }

        // adjust scrolling
        editorScrollView();

        // merge the line with the one before it
        editorMergeLines(row);

        // recomute the byte offset now that the lines are merged
        recomputeByteOffset();
    }

    E.editflag = 1;
}

void editorDelete() {
    // If trying to delete a nonexistant line, return
    if (currentLLine() == E.numllines) return;

    int row = physicalRow();
    int pos = E.bidx;

    // How many bytes need to be deleted
    int delta = utf8_next(E.row[row].chars, pos);

    if (E.cx != E.lline[currentLLine()].size) {
        // can delete characters after it

        // move all the bytes after this position back by the delta, removing the bytes in the delta from existance
        memmove(E.row[row].chars + pos, E.row[row].chars + pos + delta, E.row[row].size - pos - delta);
        E.row[row].size -= delta;

        // recompute the logical lines
        editorComputeLLines();
    } else if (currentLLine() != E.numllines-1) {
        // merge two lines, deleting the newline, puts the cursor after the first line
        editorMergeLines(row+1);
    }

    E.editflag = 1;
}

void editorProcessKeypress() {
    int c = editorReadKey();
    E.lastkey = c;

    switch (c) {
        case UNKNOWN_THREE_CHAR:
        case UNKNOWN_TWO_CHAR:
            break;
        
        case ESCAPE_KEY:
            break;

        case CTRL_KEY('q'):
            // Ctrl-Q to quit
            if (E.editflag) {
                // if modified, prompt the user (TODO)
                clearScreen();
                exit(0);
            } else {
                clearScreen();
                exit(0);
            }
            break;
        case CTRL_KEY('s'):
            // Ctrl-S to save
            if (E.filename == NULL) {
                // prompt the user to enter a filename
                
            } else {
                editorSave(E.filename);
            }
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
        case '\t':
            break;
        default:
            editorType(c);
    }
}

/* init */

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.bidx = 0;
    E.numrows = 0;
    E.row = NULL;
    E.numllines = 0;
    E.lline = NULL;
    E.scroll = 0;
    E.insMode = 0;
    E.wordcount = 0;
    E.charcount = 0;
    E.lastkey = 0x0;
    E.statusmsg = 0;
    E.editflag = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();

    if (argc == 2) {
        E.filename = argv[1];
        editorOpen(E.filename);
    } else {
        E.filename = NULL;
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}