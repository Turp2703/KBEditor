///// INCLUDES /////

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
const int kQuitTimes = 3;

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

enum editorHighlight{
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

///// DATA /////

struct editorSyntax{
    char *fileType;
    char **fileMatch;
    char **keywords;
    char *singleLineCommentStart;
    char *multiLineCommentStart;
    char *multiLineCommentEnd;
    int flags;
};

typedef struct editorRow {
    int index;
    int size;
    int renderSize;
    char *chars;
    char *render;
    unsigned char *highlighting;
    int hlOpenComment;
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
    int dirtyFlag;
    char *filename;
    char statusMsg[80];
    time_t statusMsgTime;
    struct editorSyntax *syntax;
    struct termios original_termios;
};
struct editorConfig EditorConfig;

///// FILETYPES /////

char *HLCExtensions[] = { ".c", ".h", ".cpp", NULL };
char *HLCkeywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL
};

struct editorSyntax HLDB[] = {
    {
        "c",
        HLCExtensions,
        HLCkeywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

///// PROTOTYPES /////

void editorSetStatusMessage(const char *fmt, ...);

void editorRefreshScreen();

char *editorPrompt(char *prompt, void (*callback)(char *, int));

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

///// SYNTAX HIGHLIGHTING /////

int isSeparator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(editorRow *row){
    row->highlighting = realloc(row->highlighting, row->renderSize);
    memset(row->highlighting, HL_NORMAL, row->renderSize);
    
    if(EditorConfig.syntax == NULL)
        return;
    
    char **keywords = EditorConfig.syntax->keywords;
    
    char *scs = EditorConfig.syntax->singleLineCommentStart;
    char *mcs = EditorConfig.syntax->multiLineCommentStart;
    char *mce = EditorConfig.syntax->multiLineCommentEnd;
    int scsLength = scs ? strlen(scs) : 0;
    int mcsLength = mcs ? strlen(mcs) : 0;
    int mceLength = mce ? strlen(mce) : 0;
    
    int prevSep = 1;
    int inString = 0;
    int inComment = row->index > 0 && EditorConfig.rows[row->index - 1].hlOpenComment;
    
    int i = 0;
    while(i < row->renderSize){
        char c = row->render[i];
        unsigned char prevHl = (i > 0) ? row->highlighting[i - 1] : HL_NORMAL;
        
        if(scsLength && !inString && !inComment){
            if(!strncmp(&row->render[i], scs, scsLength)){
                memset(&row->highlighting[i], HL_COMMENT, row->renderSize - i);
                break;
            }
        }
        
        if (mcsLength && mceLength && !inString) {
            if (inComment) {
                row->highlighting[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mceLength)) {
                    memset(&row->highlighting[i], HL_MLCOMMENT, mceLength);
                    i += mceLength;
                    inComment = 0;
                    prevSep = 1;
                    continue;
                }
                else {
                    i++;
                    continue;
                }
            } 
            else if (!strncmp(&row->render[i], mcs, mcsLength)) {
                memset(&row->highlighting[i], HL_MLCOMMENT, mcsLength);
                i += mcsLength;
                inComment = 1;
                continue;
            }
        }
        
        if(EditorConfig.syntax->flags & HL_HIGHLIGHT_STRINGS){
            if(inString){
                row->highlighting[i] = HL_STRING;
                if(c == '\\' && i + 1 < row->renderSize){
                    row->highlighting[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if(c == inString)
                    inString = 0;
                i++;
                prevSep = 1;
                continue;
            }
            else{
                if(c == '"' || c == '\''){
                    inString = c;
                    row->highlighting[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }
        
        if(EditorConfig.syntax->flags & HL_HIGHLIGHT_NUMBERS){
            if((isdigit(c) && (prevSep || prevHl == HL_NUMBER)) || (c == '.' && prevHl == HL_NUMBER)){
                row->highlighting[i] = HL_NUMBER;
                i++;
                prevSep = 0;
                continue;
            }
        }
        
        if (prevSep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int kwLength = strlen(keywords[j]);
                int kw2 = keywords[j][kwLength - 1] == '|';
                if (kw2) 
                    kwLength--;
                if (!strncmp(&row->render[i], keywords[j], kwLength) && isSeparator(row->render[i + kwLength])) {
                    memset(&row->highlighting[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, kwLength);
                    i += kwLength;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prevSep = 0;
                continue;
            }
        }
        
        prevSep = isSeparator(c);
        i++;
    }
    
    // Update after starting multi-line comment
    int changed = row->hlOpenComment != inComment;
    row->hlOpenComment = inComment;
    if(changed && row->index + 1 < EditorConfig.numRows)
        editorUpdateSyntax(&EditorConfig.rows[row->index + 1]);
}

int editorSyntaxToColor(int hl){
    switch(hl){
        case HL_COMMENT:
        case HL_MLCOMMENT: return 32;
        case HL_KEYWORD1: return 33;
        case HL_KEYWORD2: return 36;
        case HL_STRING: return 31;
        case HL_NUMBER: return 35;
        case HL_MATCH: return 34;
        default: return 37;
    }
}

void editorSelectSyntaxHighlight(){
    EditorConfig.syntax = NULL;
    if(EditorConfig.filename == NULL)
        return;
    
    char *ext = strrchr(EditorConfig.filename, '.');
    
    for(unsigned int j = 0; j < HLDB_ENTRIES; j++){
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while(s->fileMatch[i]){
            int isExt = (s->fileMatch[i][0] == '.');
            if ((isExt && ext && !strcmp(ext, s->fileMatch[i])) ||
                (!isExt && strstr(EditorConfig.filename, s->fileMatch[i]))) {
                EditorConfig.syntax = s;
                
                for(int fileRow = 0; fileRow < EditorConfig.numRows; fileRow++)
                    editorUpdateSyntax(&EditorConfig.rows[fileRow]);
                
                return;
            }
            i++;
        }
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
    /*
        int lastTab = -1;
        for (int j = 0; j < cursorX; j++)
            if (row->chars[j] == '\t')
                lastTab = j;
        if(lastTab != -1 && cursorX - lastTab < kTabStop)
            return lastTab + kTabStop;
        return cursorX; 
    */
}

int editorRowRenderToCursor(editorRow *row, int renderX){
    int currentRenderX = 0;
    int cursorX;
    for (cursorX = 0; cursorX < row->size; cursorX++) {
        if (row->chars[cursorX] == '\t')
            currentRenderX += (kTabStop - 1) - (currentRenderX % kTabStop);
        currentRenderX++;
        if (currentRenderX > renderX) 
            return cursorX;
    }
    return cursorX;
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
    
    editorUpdateSyntax(row);
}

void editorInsertRow(int pos, char *s, size_t len){
    if(pos < 0 || pos > EditorConfig.numRows)
        return;
    
    EditorConfig.rows = realloc(EditorConfig.rows, sizeof(editorRow) * (EditorConfig.numRows + 1));
    memmove(&EditorConfig.rows[pos + 1], &EditorConfig.rows[pos], sizeof(editorRow) * (EditorConfig.numRows - pos));
    for(int j = pos + 1; j <= EditorConfig.numRows; j++)
        EditorConfig.rows[j].index++;
    
    EditorConfig.rows[pos].index = pos;
    
    EditorConfig.rows[pos].size = len;
    EditorConfig.rows[pos].chars = malloc(len + 1);
    memcpy(EditorConfig.rows[pos].chars, s, len);
    EditorConfig.rows[pos].chars[len] = '\0';
    
    EditorConfig.rows[pos].renderSize = 0;
    EditorConfig.rows[pos].render = NULL;
    EditorConfig.rows[pos].highlighting = NULL;
    EditorConfig.rows[pos].hlOpenComment = 0;
    editorUpdateRow(&EditorConfig.rows[pos]);
    
    EditorConfig.numRows++;
    EditorConfig.dirtyFlag++;
}

void editorRowInsertChar(editorRow *row, int pos, int c){
    if(pos < 0 || pos > row->size)
        pos = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[pos + 1], &row->chars[pos], row->size - pos + 1);
    row->size++;
    row->chars[pos] = c;
    editorUpdateRow(row);
    EditorConfig.dirtyFlag++;
}

void editorRowDelChar(editorRow *row, int pos){
    if(pos < 0 || pos >= row->size)
        return;
    memmove(&row->chars[pos], &row->chars[pos + 1], row->size - pos);
    row->size--;
    editorUpdateRow(row);
    EditorConfig.dirtyFlag++;
}

void editorFreeRow(editorRow *row){
    free(row->render);
    free(row->chars);
    free(row->highlighting);
}

void editorDelRow(int pos){
    if(pos < 0 || pos >= EditorConfig.numRows)
        return;
    editorFreeRow(&EditorConfig.rows[pos]);
    memmove(&EditorConfig.rows[pos], &EditorConfig.rows[pos + 1], sizeof(editorRow) * (EditorConfig.numRows - pos - 1));
    for(int j = pos; j < EditorConfig.numRows - 1; j++)
        EditorConfig.rows[j].index--;
    EditorConfig.numRows--;
    EditorConfig.dirtyFlag++;
}

void editorRowAppendString(editorRow *row, char *s, size_t len){
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    EditorConfig.dirtyFlag++;
}

///// EDITOR OPERATIONS /////

void editorInsertChar(int c){
    if(EditorConfig.cursorY == EditorConfig.numRows)
        editorInsertRow(EditorConfig.numRows, "", 0);
    editorRowInsertChar(&EditorConfig.rows[EditorConfig.cursorY], EditorConfig.cursorX, c);
    EditorConfig.cursorX++;
}

void editorDelChar(){
    if(EditorConfig.cursorY == EditorConfig.numRows)
        return;
    if(EditorConfig.cursorX == 0 && EditorConfig.cursorY == 0)
        return;
    
    editorRow *row = &EditorConfig.rows[EditorConfig.cursorY];
    if(EditorConfig.cursorX > 0)
        editorRowDelChar(row, --EditorConfig.cursorX);
    else{
        EditorConfig.cursorX = EditorConfig.rows[EditorConfig.cursorY - 1].size;
        editorRowAppendString(&EditorConfig.rows[EditorConfig.cursorY - 1], row->chars, row->size);
        editorDelRow(EditorConfig.cursorY--);
    }
}

void editorInsertNewLine(){
    if(EditorConfig.cursorX == 0)
        editorInsertRow(EditorConfig.cursorY, "", 0);
    else{
        editorRow *row = &EditorConfig.rows[EditorConfig.cursorY];
        editorInsertRow(EditorConfig.cursorY + 1, &row->chars[EditorConfig.cursorX], row->size - EditorConfig.cursorX);
        row = &EditorConfig.rows[EditorConfig.cursorY];
        row->size = EditorConfig.cursorX;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    EditorConfig.cursorY++;
    EditorConfig.cursorX = 0;
}

///// FILE I/O /////

char *editorRowsToString(int *bufferLength){
    int totalLength = 0;
    for(int j = 0; j < EditorConfig.numRows; j++)
        totalLength += EditorConfig.rows[j].size + 1;
    *bufferLength = totalLength;
    
    char *buffer = malloc(totalLength);
    char *p = buffer;
    for(int j = 0; j < EditorConfig.numRows; j++){
        memcpy(p, EditorConfig.rows[j].chars, EditorConfig.rows[j].size);
        p += EditorConfig.rows[j].size;
        *p = '\n';
        p++;
    }
    
    return buffer;
}

void editorOpen(char *fileName){
    free(EditorConfig.filename);
    EditorConfig.filename = strdup(fileName);
    
    editorSelectSyntaxHighlight();
    
    FILE *fp = fopen(fileName, "r");
    if(!fp)
        die("fopen");
    
    char *line = NULL;
    size_t lineCap = 0;
    ssize_t lineLength;
    while((lineLength = getline(&line, &lineCap, fp)) != -1){
        while(lineLength > 0 && (line[lineLength - 1] == '\n' || line[lineLength - 1] == '\r'))
            lineLength--;
        editorInsertRow(EditorConfig.numRows, line, lineLength);
    }
    free(line);
    fclose(fp);
    EditorConfig.dirtyFlag = 0;
}

void editorSave(){
    if(EditorConfig.filename == NULL){
        EditorConfig.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if(EditorConfig.filename == NULL){
            editorSetStatusMessage("Save aborted");
            return;
        }
        editorSelectSyntaxHighlight();
    }
    
    int length;
    char *buffer = editorRowsToString(&length);
    
    int fd = open(EditorConfig.filename, O_RDWR | O_CREAT, 0644); // 0644 = Permissions
    
    if(fd != -1){
        if(ftruncate(fd, length) != -1){
            if(write(fd, buffer, length) == length){
                close(fd);
                free(buffer);
                EditorConfig.dirtyFlag = 0;
                editorSetStatusMessage("%d bytes written to disk.", length);
                return;
            }
        }
        close(fd);
    }
    free(buffer);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

///// FIND /////

void editorFindCallback(char *query, int key){
    static int lastMatch = -1;
    static int direction = 1;
    
    static int savedHlLine;
    static char *savedHl = NULL;
    
    if(savedHl){
        memcpy(EditorConfig.rows[savedHlLine].highlighting, savedHl, EditorConfig.rows[savedHlLine].renderSize);
        free(savedHl);
        savedHl = NULL;
    }
    
    if(key == '\r' || key == '\x1b'){
        lastMatch = -1;
        direction = 1;
        return;
    }
    else if(key == ARROW_RIGHT || key == ARROW_DOWN)
        direction = 1;
    else if(key == ARROW_LEFT || key == ARROW_UP)
        direction = -1;
    else{
        lastMatch = -1;
        direction = 1;
    }
    
    if(lastMatch == -1)
        direction = 1;
    int current = lastMatch;
    for(int i = 0; i < EditorConfig.numRows; i++){
        current += direction;
        if(current == -1)
            current = EditorConfig.numRows - 1;
        else if(current == EditorConfig.numRows)
            current = 0;
        
        editorRow *row = &EditorConfig.rows[current];
        char *match = strstr(row->render, query);
        if(match){
            lastMatch = current;
            EditorConfig.cursorY = current;
            EditorConfig.cursorX = editorRowRenderToCursor(row, match - row->render);
            EditorConfig.rowOffset = EditorConfig.numRows;
            
            savedHlLine = current;
            savedHl = malloc(row->renderSize);
            memcpy(savedHl, row->highlighting, row->renderSize);
            memset(&row->highlighting[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind(){
    int oldCursorX = EditorConfig.cursorX;
    int oldCursorY = EditorConfig.cursorY;
    int oldColOff = EditorConfig.colOffset;
    int oldRowOff = EditorConfig.rowOffset;
    
    char *query = editorPrompt("Search: %s (ESC/Arrows/Enter)", editorFindCallback);
    
    if(query)
        free(query);
    else{
        EditorConfig.cursorX = oldCursorX;
        EditorConfig.cursorY = oldCursorY;
        EditorConfig.colOffset = oldColOff;
        EditorConfig.rowOffset = oldRowOff;
    }
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

char *editorPrompt(char *prompt, void (*callback)(char *, int)){
    size_t bufferSize = 128;
    char *buffer = malloc(bufferSize);
    
    size_t bufferLength = 0;
    buffer[0] = '\0';
    
    while(1){
        editorSetStatusMessage(prompt, buffer);
        editorRefreshScreen();
        
        int c = editorReadKey();
        if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
            if(bufferLength != 0)
                buffer[--bufferLength] = '\0';
        }
        else if(c == '\x1b'){
            editorSetStatusMessage("");
            if(callback)
                callback(buffer, c);
            free(buffer);
            return NULL;
        }
        else if(c == '\r'){
            if(bufferLength != 0){
                editorSetStatusMessage("");
                if(callback)
                    callback(buffer, c);
                return buffer;
            }
        }
        else if(!iscntrl(c) && c < 128){
            if(bufferLength == bufferSize - 1){
                bufferSize *= 2;
                buffer = realloc(buffer, bufferSize);
            }
            buffer[bufferLength++] = c;
            buffer[bufferLength] = '\0';
        }
        
        if(callback)
            callback(buffer, c);
    }
}

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
    static int quitTimes = kQuitTimes;
    
    int c = editorReadKey();
    switch (c) {
        case '\r':
            editorInsertNewLine();
            break;
        
        case CTRL_KEY('q'):
            if(EditorConfig.dirtyFlag && quitTimes > 0){
                editorSetStatusMessage("WARNING! File has unsaved changes. Press Ctrl-Q %d more times to quit.", quitTimes);
                quitTimes--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
            
        case CTRL_KEY('s'):
            editorSave();
            break;
            
        case HOME_KEY:
            EditorConfig.cursorX = 0;
            break;
        case END_KEY:
            if(EditorConfig.cursorY < EditorConfig.numRows)
                EditorConfig.cursorX = EditorConfig.rows[EditorConfig.cursorY].size;
            break;
            
        case CTRL_KEY('f'):
            editorFind();
            break;
            
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if(c == DEL_KEY)
                editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
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
            
        case CTRL_KEY('l'):
        case '\x1b':
            break;
            
        default:
            editorInsertChar(c);
            break;
    }
    
    quitTimes = kQuitTimes;
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
            char *c = &EditorConfig.rows[currentRow].render[EditorConfig.colOffset];
            unsigned char *hl = &EditorConfig.rows[currentRow].highlighting[EditorConfig.colOffset];
            int currentColor = -1;
            for(int j = 0; j < len; j++){
                if(iscntrl((unsigned char)c[j])){
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);
                    if(currentColor != -1){
                        char buf[16];
                        int cLength = snprintf(buf, sizeof(buf), "\x1b[%dm", currentColor);
                        abAppend(ab, buf, cLength);
                    }
                }
                else if(hl[j] == HL_NORMAL){
                    if(currentColor != -1){
                        abAppend(ab, "\x1b[39m", 5);
                        currentColor = -1;
                    }
                    abAppend(ab, &c[j], 1);
                }
                else{
                    int color = editorSyntaxToColor(hl[j]);
                    if(color != currentColor){
                        currentColor = color;
                        char buf[16];
                        int cLen = snprintf(buf, sizeof(buf), "\x1b[1;%dm", color);
                        abAppend(ab, buf, cLen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5); // '39m' = Reset colors
        }
    
        abAppend(ab, "\x1b[K", 3); // 'K' = '0K' = Clear line left to cursor
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(AppendBuffer *ab){
    abAppend(ab, "\x1b[7m", 4); // '7m' = Invert colors
    
    char status[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", 
                    EditorConfig.filename ? EditorConfig.filename : "[No Name]", 
                    EditorConfig.numRows, EditorConfig.dirtyFlag ? "(modified)" : "");
    char rightStatus[80];
    int rightLen = snprintf(rightStatus, sizeof(rightStatus), "%s | %d/%d ",
                    EditorConfig.syntax ? EditorConfig.syntax->fileType : "No FT",
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
    EditorConfig.dirtyFlag = 0;
    EditorConfig.filename = NULL;
    EditorConfig.statusMsg[0] = '\0';
    EditorConfig.statusMsgTime = 0;
    EditorConfig.syntax = NULL;
    
    if (getWindowSize(&EditorConfig.screenRows, &EditorConfig.screenCols) == -1)
        die("getWindowSize");
    
    EditorConfig.screenRows -= 2; // Status
}

int main(int argc, char *argv[]){
    enableRawMode();
    initEditor();
    if(argc >= 2)
        editorOpen(argv[1]);
    
    editorSetStatusMessage("HELP: Ctrl-Q → Quit | Ctrl-S → Save | Ctrl-F → Find");
    
    while(1){
        editorRefreshScreen();
        editorProcessKeypress();
    }
    
    return 0;
}
