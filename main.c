///// INCLUDES /////

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>

///// DEFINES /////

#define CTRL_KEY(k) ((k) & 0x1f)

const int kTabStop = 4;

enum editorKey {
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

///// DATA /////

typedef struct editorRow {
    int size;
    int renderSize;
    char *chars;
    char *render;
} editorRow;

struct editorConfig {
    int cursorX;
    int cursorY;
    int renderX;
    int rowOffset;
    int colOffset;
    int screenRows;
    int screenCols;
    int numRows;
    editorRow *rows;
    char *filename;
    char statusMsg[80];
    time_t statusMsgTime;
    struct termios original_termios;
};
struct editorConfig EditorConfig;

///// TERMINAL /////

void die(const char *s){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode(){
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &EditorConfig.original_termios) == -1)
        die("tcsetattr");
}

void enableRawMode(){
    if(tcgetattr(STDIN_FILENO, &EditorConfig.original_termios) == -1)
        die("tcgetattr");
    atexit(disableRawMode);
    
    struct termios raw = EditorConfig.original_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editorReadKey() {
    char c;
    int nread;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) 
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) 
            return '\x1b';
        
        if (seq[0] == '[') {
            
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) 
                    return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '3': return DEL_KEY;
                        case '1': return HOME_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }
            else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        
        return '\x1b';
    } 
    else
        return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) 
        return -1;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R') 
            break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') 
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) 
        return -1;
    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) 
            return -1;
        return getCursorPosition(rows, cols);
    }
    else {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return 0;
    }
}

///// ROW OPERATIONS /////

int editorRowCursorToRender(editorRow *row, int cursorX){
    int renderX = 0;
    for (int j = 0; j < cursorX; j++) {
        if (row->chars[j] == '\t')
            renderX += (kTabStop - 1) - (renderX % kTabStop);
        renderX++;
    }
    return renderX;

    // TODO
    // int lastTab = -1;
    // for (int j = 0; j < cursorX; j++)
        // if (row->chars[j] == '\t')
            // lastTab = j;
    // if(lastTab != -1 && cursorX - lastTab < kTabStop)
        // return lastTab + kTabStop;
    // return cursorX; 
}

void editorUpdateRow(editorRow *row){
    int tabs = 0;
    int j;
    for(j = 0; j < row->size; j++)
        if(row->chars[j] == '\t') 
            tabs++;
        
    free(row->render);
    row->render = malloc(row->size + tabs * (kTabStop - 1) + 1);
    
    int idx = 0;
    for (j = 0; j < row->size; j++){
        if(row->chars[j] == '\t')
            do row->render[idx++] = ' ';
            while(idx % kTabStop != 0);
        else
            row->render[idx++] = row->chars[j];
    }
    row->render[idx] = '\0';
    row->renderSize = idx;
}

void editorAppendRow(char *s, size_t len){
    EditorConfig.rows = realloc(EditorConfig.rows, sizeof(editorRow) * (EditorConfig.numRows + 1));
    
    int current = EditorConfig.numRows;
    EditorConfig.rows[current].size = len;
    EditorConfig.rows[current].chars = malloc(len + 1);
    memcpy(EditorConfig.rows[current].chars, s, len);
    EditorConfig.rows[current].chars[len] = '\0';
    
    EditorConfig.rows[current].renderSize = 0;
    EditorConfig.rows[current].render = NULL;
    editorUpdateRow(&EditorConfig.rows[current]);
    
    EditorConfig.numRows++;
}

///// FILE I/O /////

void editorOpen(char *fileName){
    free(EditorConfig.filename);
    EditorConfig.filename = strdup(fileName);
    
    FILE *fp = fopen(fileName, "r");
    if(!fp)
        die("fopen");
    
    char *line = NULL;
    size_t lineCap = 0;
    ssize_t lineLength;
    while((lineLength = getline(&line, &lineCap, fp)) != -1){
        while(lineLength > 0 && (line[lineLength - 1] == '\n' || line[lineLength - 1] == '\r'))
            lineLength--;
        editorAppendRow(line, lineLength);
    }
    free(line);
    fclose(fp);
}

///// APPEND BUFFER /////

typedef struct aBuf {
    char *b;
    int len;
} AppendBuffer;

#define ABUF_INIT {NULL, 0}

void abAppend(AppendBuffer *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) 
        return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(AppendBuffer *ab) {
    free(ab->b);
}

///// INPUT /////

void editorMoveCursor(int key) {
    editorRow *row = (EditorConfig.cursorY >= EditorConfig.numRows) 
                    ? NULL 
                    : &EditorConfig.rows[EditorConfig.cursorY];
    
    switch (key) {
        case ARROW_LEFT:
            if(EditorConfig.cursorX != 0)
                EditorConfig.cursorX--;
            else if(EditorConfig.cursorY > 0)
                EditorConfig.cursorX = EditorConfig.rows[--EditorConfig.cursorY].size;
            break;
        case ARROW_RIGHT:
            if(row && EditorConfig.cursorX < row->size)
                EditorConfig.cursorX++;
            else if(row && EditorConfig.cursorX == row->size){
                EditorConfig.cursorY++;
                EditorConfig.cursorX = 0;
            }
            break;
        case ARROW_UP:
            if(EditorConfig.cursorY != 0)
                EditorConfig.cursorY--;
            break;
        case ARROW_DOWN:
            if(EditorConfig.cursorY < EditorConfig.numRows)
                EditorConfig.cursorY++;
            break;
    }
    
    row = (EditorConfig.cursorY >= EditorConfig.numRows) 
        ? NULL 
        : &EditorConfig.rows[EditorConfig.cursorY];
    int rowLength = row ? row->size : 0;
    if(EditorConfig.cursorX > rowLength)
        EditorConfig.cursorX = rowLength;
}

void editorProcessKeypress() {
    int c = editorReadKey();
    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
            
        case HOME_KEY:
            EditorConfig.cursorX = 0;
            break;
        case END_KEY:
            if(EditorConfig.cursorY < EditorConfig.numRows)
                EditorConfig.cursorX = EditorConfig.rows[EditorConfig.cursorY].size;
            break;
        
        case PAGE_UP:
        case PAGE_DOWN:
            { // Can't declare inside switch without {}
                if (c == PAGE_UP)
                    EditorConfig.cursorY = EditorConfig.rowOffset;
                else if (c == PAGE_DOWN) {
                    EditorConfig.cursorY = EditorConfig.rowOffset + EditorConfig.screenRows - 1;
                    if (EditorConfig.cursorY > EditorConfig.numRows) 
                        EditorConfig.cursorY = EditorConfig.numRows;
                }
                int times = EditorConfig.screenRows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        
        case ARROW_LEFT:
        case ARROW_RIGHT:
        case ARROW_UP:
        case ARROW_DOWN:
            editorMoveCursor(c);
            break;
    }
}

///// OUTPUT /////

void editorScroll(){
    EditorConfig.renderX = 0;
    if(EditorConfig.cursorY < EditorConfig.numRows)
        EditorConfig.renderX = editorRowCursorToRender(&EditorConfig.rows[EditorConfig.cursorY], EditorConfig.cursorX);
    
    if(EditorConfig.cursorY < EditorConfig.rowOffset)
        EditorConfig.rowOffset = EditorConfig.cursorY;
    if(EditorConfig.cursorY >= EditorConfig.rowOffset + EditorConfig.screenRows)
        EditorConfig.rowOffset = EditorConfig.cursorY - EditorConfig.screenRows + 1;;
    
    if(EditorConfig.renderX < EditorConfig.colOffset)
        EditorConfig.colOffset = EditorConfig.renderX;
    if(EditorConfig.renderX >= EditorConfig.colOffset + EditorConfig.screenCols)
        EditorConfig.colOffset = EditorConfig.renderX - EditorConfig.screenCols + 1;
}

void editorDrawWelcome(AppendBuffer *ab){
    char welcome[80];
    int welcomelen = snprintf(welcome, sizeof(welcome), "----- Kilo Based Editor -----");
    if (welcomelen > EditorConfig.screenCols) 
        welcomelen = EditorConfig.screenCols;
    
    int padding = (EditorConfig.screenCols - welcomelen) / 2;
    if (padding) {
      abAppend(ab, "~", 1);
      padding--;
    }
    while (padding--) 
        abAppend(ab, " ", 1);
    
    abAppend(ab, welcome, welcomelen);
}

void editorDrawRows(AppendBuffer *ab){
    for (int y = 0; y < EditorConfig.screenRows; y++){
        int currentRow = y + EditorConfig.rowOffset;
        if(currentRow >= EditorConfig.numRows){
            if (EditorConfig.numRows == 0 && y == EditorConfig.screenRows / 3)
                editorDrawWelcome(ab);
            else
                abAppend(ab, "~", 1);
        }
        else{
            int len = EditorConfig.rows[currentRow].renderSize - EditorConfig.colOffset;
            if(len < 0)
                len = 0;
            if(len > EditorConfig.screenCols)
                len = EditorConfig.screenCols;
            abAppend(ab, &EditorConfig.rows[currentRow].render[EditorConfig.colOffset], len);
        }
    
        abAppend(ab, "\x1b[K", 3); // 'K' = '0K' = Clear line left to cursor
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(AppendBuffer *ab){
    abAppend(ab, "\x1b[7m", 4); // '7m' = Invert colors
    
    char status[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines", 
                    EditorConfig.filename ? EditorConfig.filename : "[No Name]", 
                    EditorConfig.numRows);
    char rightStatus[80];
    int rightLen = snprintf(rightStatus, sizeof(rightStatus), "%d/%d ",
                    EditorConfig.cursorY + 1, EditorConfig.numRows);
    if (len > EditorConfig.screenCols) 
        len = EditorConfig.screenCols;
    abAppend(ab, status, len);
    
    while (len < EditorConfig.screenCols) {
        if(EditorConfig.screenCols - len == rightLen){
            abAppend(ab, rightStatus, rightLen);
            break;
        }
        else{
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3); // 'm' = Normal colors
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(AppendBuffer *ab){
    abAppend(ab, "\x1b[K", 3);
    int msgLen = strlen(EditorConfig.statusMsg);
    if (msgLen > EditorConfig.screenCols) 
        msgLen = EditorConfig.screenCols;
    if (msgLen && time(NULL) - EditorConfig.statusMsgTime < 5)
        abAppend(ab, EditorConfig.statusMsg, msgLen);
}

void editorRefreshScreen(){
    editorScroll();
    
    AppendBuffer ab = ABUF_INIT;
    
    // '\x1b' = ESC_CHR, '[' = ESC_SEQ
    abAppend(&ab, "\x1b[?25l", 6); // '?25l' = Hide cursor
    abAppend(&ab, "\x1b[H", 3); // 'H' = '1;1H' = Move cursor
    
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);
    
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH"
            , (EditorConfig.cursorY - EditorConfig.rowOffset) + 1
            , (EditorConfig.renderX - EditorConfig.colOffset) + 1);
    abAppend(&ab, buf, strlen(buf));
    
    abAppend(&ab, "\x1b[?25h", 6); // '?25h' = Show Cursor
    
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(EditorConfig.statusMsg, sizeof(EditorConfig.statusMsg), fmt, ap);
    va_end(ap);
    EditorConfig.statusMsgTime = time(NULL);
}

///// INIT /////

void initEditor(){
    EditorConfig.cursorX = 0;
    EditorConfig.cursorY = 0;
    EditorConfig.renderX = 0;
    EditorConfig.rowOffset = 0;
    EditorConfig.colOffset = 0;
    EditorConfig.numRows = 0;
    EditorConfig.rows = NULL;
    EditorConfig.filename = NULL;
    EditorConfig.statusMsg[0] = '\0';
    EditorConfig.statusMsgTime = 0;
    
    if (getWindowSize(&EditorConfig.screenRows, &EditorConfig.screenCols) == -1)
        die("getWindowSize");
    
    EditorConfig.screenRows -= 2; // Status
}

int main(int argc, char *argv[]){
    enableRawMode();
    initEditor();
    if(argc >= 2)
        editorOpen(argv[1]);
    
    editorSetStatusMessage("HELP: Ctrl-Q → quit");
    
    while(1){
        editorRefreshScreen();
        editorProcessKeypress();
    }
    
    return 0;
}