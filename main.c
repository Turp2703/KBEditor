///// INCLUDES /////

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>

///// DEFINES /////

#define CTRL_KEY(k) ((k) & 0x1f)

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

struct editorConfig {
    int cursorX;
    int cursorY;
    int screenRows;
    int screenCols;
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
    switch (key) {
        case ARROW_LEFT:
            if(EditorConfig.cursorX != 0)
                EditorConfig.cursorX--;
            break;
        case ARROW_RIGHT:
            if(EditorConfig.cursorX != EditorConfig.screenCols - 1)
                EditorConfig.cursorX++;
            break;
        case ARROW_UP:
            if(EditorConfig.cursorY != 0)
                EditorConfig.cursorY--;
            break;
        case ARROW_DOWN:
            if(EditorConfig.cursorY != EditorConfig.screenRows - 1)
                EditorConfig.cursorY++;
            break;
    }
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
            EditorConfig.cursorX = EditorConfig.screenCols - 1;
            break;
        
        case PAGE_UP:
        case PAGE_DOWN:
            { // Can't declare inside switch without {}
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
        if (y == EditorConfig.screenRows / 3) 
            editorDrawWelcome(ab);
        else
            abAppend(ab, "~", 1);
    
        abAppend(ab, "\x1b[K", 3); // 'K' = '0K' = Clear line left to cursor
        if (y < EditorConfig.screenRows - 1)
            abAppend(ab, "\r\n", 2);
    }
}

void editorRefreshScreen(){
    AppendBuffer ab = ABUF_INIT;
    
    // '\x1b' = ESC_CHR, '[' = ESC_SEQ
    abAppend(&ab, "\x1b[?25l", 6); // '?25l' = Hide cursor
    abAppend(&ab, "\x1b[H", 3); // 'H' = '1;1H' = Move cursor
    
    editorDrawRows(&ab);
    
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", EditorConfig.cursorY + 1, EditorConfig.cursorX + 1);
    abAppend(&ab, buf, strlen(buf));
    
    abAppend(&ab, "\x1b[?25h", 6); // '?25h' = Show Cursor
    
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

///// INIT /////

void initEditor(){
    EditorConfig.cursorX = 0;
    EditorConfig.cursorY = 0;
    
    if (getWindowSize(&EditorConfig.screenRows, &EditorConfig.screenCols) == -1)
        die("getWindowSize");
}

int main(){
    enableRawMode();
    initEditor();
    
    while(1){
        editorRefreshScreen();
        editorProcessKeypress();
    }
    
    return 0;
}