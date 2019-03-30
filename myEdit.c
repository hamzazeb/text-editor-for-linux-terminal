/*** includes ****/
//The exact set of features available when you compile a source file is controlled by which feature test macros you define.
#define _DEFAULT_SOURCE		// feature test macro ( for getline function )
#define _BSD_SOURCE
#define _GNU_SOURCE					


#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>	// open(), O_RDWR, and O_CREAT etc also
#include <sys/types.h>	// for ssize_t data type (sizeof(ssize_t) is greater or = sizeof(int))
#include <string.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h> 	// for window size easy way
#include <unistd.h>	// ftruncate() and close() etc


/**** defines ****/

# define CTRL_KEY(k) ((k) & 0x1f) // will AND the key we entered(along with Ctrl) with 0x1f(00011111).
#define EDITOR_VERSION "0.1"
#define EDITOR_TAB_STOP 8

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

/*** data ****/

typedef struct erow {			// for storing a row of characters
	int size;
	int rsize;			// for rendering tab, removing tab error in our editor
	char *chars;
	char *render;			// rendering chars for removing tab error, previously in our editor tab was filled by initial chars of that line
} erow;

struct editorConfig			// editor's global state
{
	int cx, cy;			// cursor positions ( cx=colums & cy=rows )
	int rx;				// for tab position
	int rowoff;			//this varible will keep track of what row of the file the user is currently scrolled to
	int coloff;			//this varible will keep track of what colum of the file the user is currently scrolled to
	int screenrows;
	int screencols;
	int numrows;			// for number of rows	
	erow *row;
	char *filename;
	struct termios orig_termios;
};

struct editorConfig E;

void editorRefreshScreen();
char *editorPrompt(char *prompt);

/*** terminal ****/

void die(const char *s)
{
	write(STDOUT_FILENO, "\x1b[2J", 4);	// here 2J means clear the whole screen without moving our cursor
	write(STDOUT_FILENO, "\x1b[H", 3);	// reposition our cursor to top left
	
	perror(s);
	exit(1);
}

void disableRawMode()
{
	if( tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		die("Error in tcsetattr");
}

void enableRawMode()
{	
	if( tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
		die("Error in tcgetattr");
	atexit(disableRawMode);		// when returning from main or run exit() in main then this will execute

	struct termios raw = E.orig_termios;
	
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); //input flag, ICRNL used to fix Ctrl-M, IXON used to disable Ctlr-S and Ctrl-Q
	raw.c_oflag &= ~(OPOST);	//output flag, here I assume POST stands for post-processing-of-output
	raw.c_cflag |= (CS8);		//control flag, it sets character size CS to 8 bits each byte
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);	//local flag, ECHO property is used to print out at terminal, ICANON is a flag used to read byte by byte, ISIG is a flag used to diable Ctrl-C and Ctrl-Z, IEXTEN flag is used to fix Ctrl-V and Ctrl-O.
	raw.c_cc[VMIN] = 0;	// cc stands for control character, min num of bytes of input needed before read can return.
	raw.c_cc[VTIME] = 1;	// max amount of time to wait before read returns. It is 1/10 of second.
	if( tcsetattr(STDIN_FILENO ,TCSAFLUSH, &raw) == -1)
		die("Error in tcsetattr");
}


int editorReadKey()
{
	int nread;		// how many char read, in our case we will read 1 char
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1 )	// here it will wait until it read 1 character
	{
		if (nread == -1 && errno != EAGAIN)
			die("Error in reading character");
	}
							// when ever we press an arrow keys then it write 3 characters \x1b, [ and A/B/C/D
	if (c == '\x1b') {				// after reading escape character in c we read two more characters in seq
		char seq[3];
		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';		// If either of these reads time out (after 0.1 seconds), then we assume the user just pressed the Escape key and return that
 		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
 	
		if (seq[0] == '[') {
      			if (seq[1] >= '0' && seq[1] <= '9') {	// Page Up is sent as <esc>[5~ and Page Down is sent as <esc>[6~
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
			// why return '\x1b'? it will run while loop in main() again for refreshing and for processkey.
	  return '\x1b';			// if not A/B/C/D/5/6 then we assume the user just pressed the Escape key and return that
	} else {
	  return c;
	}
}

int getCursorPosition(int *rows, int *cols) 
{
	char buf[32];
	unsigned int i = 0;
	
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) 
		return -1;
	while (i < sizeof(buf) - 1) 
	{
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

int getWindowSize(int *rows, int *cols)				// used to get the size of window rows and cols of window
{
	struct winsize ws;
	
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) 
	{
    		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) 	// two escape sequence, C is to move forward and B is to move downward, here we want to move our cursor bottom right position thats why we gave 999(high value). C and B commands will prevent the cursor by moving ahead of the screen.
			return -1;
 		return getCursorPosition(rows, cols);
	}
	else
	{
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {	// getting proper tab position to move
  	int rx = 0;
  	int j;
  	for (j = 0; j < cx; j++) {
    		if (row->chars[j] == '\t')
      			rx += (EDITOR_TAB_STOP - 1) - (rx % EDITOR_TAB_STOP);
    		rx++;
  	}
  	return rx;
}

void editorUpdateRow(erow *row) {		// here we are copying all rows int row->render
	int tabs = 0;  	
	int j;	
	for (j = 0; j < row->size; j++)	// here we count the tabs in order to know how much memory to allocate for render.
		if (row->chars[j] == '\t') tabs++;

	free(row->render);
	row->render = malloc(row->size + tabs*(EDITOR_TAB_STOP - 1) + 1);
  	
  	int idx = 0;
  	for (j = 0; j < row->size; j++) {
    		if (row->chars[j] == '\t') {
      			row->render[idx++] = ' ';
      			while (idx % EDITOR_TAB_STOP != 0) row->render[idx++] = ' ';	// copy blank space at upto 8 places if '\t' in line.
    		} else {
  			row->render[idx++] = row->chars[j];	// copying chars from row->chars to row->render	
		  }  	
	}
  	row->render[idx] = '\0';
  	row->rsize = idx;				// no of chars copied to render
}

void editorAppendRow(char *s, size_t len) {	// this is for multiple rows
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));	// here we want editorAppendRow() to allocate space for a new erow, and then copy the given string to a new erow at the end of the E.row array.

  	int at = E.numrows;
  	E.row[at].size = len;
  	E.row[at].chars = malloc(len + 1);
  	memcpy(E.row[at].chars, s, len);
  	E.row[at].chars[len] = '\0';

  	E.row[at].rsize = 0;
  	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);

  	E.numrows++;
}

void editorInsertRow(int at, char *s, size_t len) {
  	if (at < 0 || at > E.numrows) return;

  	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
  
  	E.row[at].size = len;
  	E.row[at].chars = malloc(len + 1);
  	memcpy(E.row[at].chars, s, len);
  	E.row[at].chars[len] = '\0';
  	E.row[at].rsize = 0;

  	E.row[at].render = NULL;
  	editorUpdateRow(&E.row[at]);

  	E.numrows++;
}


void editorFreeRow(erow *row) {
  	free(row->render);
  	free(row->chars);
}

void editorDelRow(int at) {
  	if (at < 0 || at >= E.numrows) return;

  	editorFreeRow(&E.row[at]);
  	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1)); // remove current row and move next all rows at its place
  	E.numrows--;
}

void editorRowInsertChar(erow *row, int at, int c) {
  	if (at < 0 || at > row->size) at = row->size;
  	row->chars = realloc(row->chars, row->size + 2);
  	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  	row->size++;
  	row->chars[at] = c;
  	editorUpdateRow(row);
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  	row->chars = realloc(row->chars, row->size + len + 1);	// allocate new size rowsize + len(size of s row)
  	memcpy(&row->chars[row->size], s, len);			// copy all chars(s) at last of row
  	row->size += len;
  	row->chars[row->size] = '\0';
  	editorUpdateRow(row);
}

void editorRowDelChar(erow *row, int at) {
  	if (at < 0 || at >= row->size) return;	

  	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);	// memmove() is same like memcpy but it is more reliable then memcpy()
  	row->size--;
  	editorUpdateRow(row);
}

/*** editor operations ***/

void editorInsertChar(int c) {
  	if (E.cy == E.numrows) {
    		editorInsertRow(E.numrows, "", 0);
  	}
  	editorRowInsertChar(&E.row[E.cy], E.cx, c);
  	E.cx++;
}

void editorInsertNewline() {
  	if (E.cx == 0) {
    		editorInsertRow(E.cy, "", 0);
  	} else {
    		erow *row = &E.row[E.cy];
    		editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    		row = &E.row[E.cy];
    		row->size = E.cx;
    		row->chars[row->size] = '\0';
    		editorUpdateRow(row);
  	  }
  
	E.cy++;
  	E.cx = 0;
}

void editorDelChar() {	// delete character
  	if (E.cy == E.numrows) return; // If the cursor’s past the end of the file, then there is nothing to delete, and we return
	if (E.cx == 0 && E.cy == 0) return; // begining of file

  	erow *row = &E.row[E.cy];
  	if (E.cx > 0) {
    		editorRowDelChar(row, E.cx - 1);	// E.cx is cursor position, E.cx-1 is character(right before cursor position) which will be deleted
    		E.cx--;
  	} else {
    		E.cx = E.row[E.cy - 1].size;
    		editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size); // will send & of previeus row where our cursor is going after backspace, and churrent row's all chars and its size.
    		editorDelRow(E.cy);			// will delete current row
    		E.cy--;					// -- in no of rows
	  }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) { //it will converts our array of erow structs into a single string that is ready to be written out to a file.
  	int totlen = 0;
  	int j;
  	for (j = 0; j < E.numrows; j++)
    		totlen += E.row[j].size + 1;	// total lengh of all rows + 1(for adding enter at last of each row)
  	*buflen = totlen;
  	char *buf = malloc(totlen);
  	char *p = buf;
  	for (j = 0; j < E.numrows; j++) {
    		memcpy(p, E.row[j].chars, E.row[j].size);	// will copy row[j] characters to p(buf)
    		p += E.row[j].size;				// 
    		*p = '\n';					// add enter at last of p(buf)
    		p++;						// move to next location of p(buf)
  	}
  	
	return buf;						// will return string back
}

void editorOpen(char *filename) {
	free(E.filename);
	E.filename = strdup(filename);	

	FILE *fp = fopen(filename, "r");
	  if (!fp) die("fopen");

	  char *line = NULL;			// initially store NULL to line
	  size_t linecap = 0;			// line capacity is 0 initially
	  ssize_t linelen;
	  while ((linelen = getline(&line, &linecap, fp)) != -1)	// lieline = number of characters we are getting from user
	  {// yahan while loop ma hum na har line ko end kar lia ha kun ka erow ma sirf aik line store ho sakti ha isi lia is ma or koi chance nai ha new line store karny ka.
	    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) 
	    	linelen--;

	    editorInsertRow(E.numrows, line, linelen);
	  }
	free(line);
	fclose(fp);
}

void editorSave() {
  	if (E.filename == NULL)	{			// If it’s a new file
    		E.filename = editorPrompt("Save as: %s");
  	}	

  	int len;
  	char *buf = editorRowsToString(&len);

  	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);	// 0644 gives the owner perm to read/write, and everyone else to read the file
	if (fd != -1) { 					// error handling
		if (ftruncate(fd, len) != -1) { 		// ftruncate() sets the file’s size to the specified length
			if (write(fd, buf, len) == len){	
				close(fd);
  				free(buf);
				return;
			}
		}
		close(fd);
	} 

	free(buf);
}


/*** append buffer ***/
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


/**** output ****/

void editorScroll() {
	E.rx = 0;
  	if (E.cy < E.numrows) {
    		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);	// here we are setting our cursor to proper tab position if there is tab in line.
  	}

  	if (E.cy < E.rowoff) 
	{
   		E.rowoff = E.cy;
  	}
  	if (E.cy >= E.rowoff + E.screenrows) // E.cy will check if cursor is moving below the bottom if yes then it will add E.rowoff
	{
    		E.rowoff = E.cy - E.screenrows + 1; // E.rowoff will be rows below(only) the bottom of screen, +1 for one extra row at last
  	}	
	if (E.rx < E.coloff) {
	    	E.coloff = E.rx;
  	}
  	if (E.rx >= E.coloff + E.screencols) {
    		E.coloff = E.rx - E.screencols + 1;
  	}
}

void editorDrawRows(struct abuf *ab)
{
	int y;
	for ( y=0; y<E.screenrows; y++)			// screenrows we get using getWindowSize() below
	{
	   int filerow = y + E.rowoff;			// will display the correct range of lines of the file for scrolling
	   if (filerow >= E.numrows) {			// it will decide to either write ~ on screen or new row
		if (E.numrows == 0 && y == E.screenrows/3) 
		{
		      char welcome[180], welcome1[180];
		      int welcomelen = snprintf(welcome, sizeof(welcome),"Text Editor by: Hamza Zeb(1410) | Saud Ahmad(1393) | Sohail Khan(1497)\n");

int welcomelen1 = snprintf(welcome1, sizeof(welcome1),"\n\rInstructions: Ctrl+Q->Exit | Ctrl+S->Save");
		      if (welcomelen > E.screencols) welcomelen = E.screencols;
			if (welcomelen1 > E.screencols) welcomelen1 = E.screencols;
	/*	      int padding = (E.screencols - welcomelen) / 2;
		      if (padding) 
			{
			        abAppend(ab, " ", 1);	// welcome message line 
			        padding--;
      			}
		      while (padding--) abAppend(ab, " ", 1);
	*/	
			abAppend(ab, welcome, welcomelen);
			abAppend(ab, welcome1, welcomelen1);
		} 
		else 
		{
		      abAppend(ab, "~", 1);
		}
	   }
	   else {			// this else will write rows at terminal
      			int len = E.row[filerow].rsize - E.coloff;	// E.row[filerow].size = row size, E.coloff = current cursor position in row ( 30 char - 0 coloff=30, 30 char - 32 coloff=-2)
      			if (len < 0) len = 0;				// if cursor is move ahead of num of chars in row then len = 0
      			if (len > E.screencols) len = E.screencols;	//this if will truncate 
      			abAppend(ab, &E.row[filerow].render[E.coloff], len);
    		}
	//	abAppend(ab, "~", 1);	// will draw ~ at start of each row
		abAppend(ab, "\x1b[K", 3); // K means clear current line, by default it has 0 which means erase from active position to the end of line. It replace [2J	    	
		if (y < E.screenrows - 1) 	// will prevent to enter a new line at last.
		{	
      			abAppend(ab, "\r\n", 2);;	
		}
	}
}

void editorRefreshScreen()
{
	editorScroll();

	struct abuf ab = ABUF_INIT;
	//write(STDOUT_FILENO, "\x1b[2J", 4); 	//write out the escape scequence\x1b to the editor,  // here 2J means clear the whole screen without moving our cursor
	//write(STDOUT_FILENO, "\x1b[H", 3);	// setting our cursor to the left top of our editor
	
	abAppend(&ab, "\x1b[?25l", 6);	//RM(reset mode) hide cursor to prevent annoying flicker effect
  	abAppend(&ab, "\x1b[H", 3);	// reposition our cursor to top left

	editorDrawRows(&ab);		// will draw teldas and welcome message
  	
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);	// reposition our cursor to these locations
	abAppend(&ab, buf, strlen(buf));

  	abAppend(&ab, "\x1b[?25h", 6);	//SM(set mode) display cursor to prevent annoying flicker effect

	write(STDOUT_FILENO, ab.b, ab.len);	// it will write buffers content to screen. If we remove this then there will be no writing shown in our editor(terminal)
  	abFree(&ab);		// free buffer
	
	//write(STDOUT_FILENO, "\x1b[H", 3);	// reposition our cursor to top left
}

/**** input ****/

char *editorPrompt(char *prompt) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';
  while (1) {
    editorRefreshScreen();
    int c = editorReadKey();
    if (c == '\x1b') {
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
  }
}

void editorMoveCursor(int key) {
	  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];	// here we use the ternary operator to check if the cursor is on an actual line. If it is, then the row variable will point to the erow that the cursor is on.

	switch (key) {
		    case ARROW_LEFT:
      			if (E.cx != 0) {			// prevent moving cursor out of the screen
        			E.cx--;			// x-axis, move accross x-axis(horizontal)
      			} else if (E.cy > 0) 
			  {
        			E.cy--;
		        	E.cx = E.row[E.cy].size;	// we are setting E.cx to end of row by getting length of row
      			  }
      			break;
    		case ARROW_RIGHT:
      			if (row && E.cx < row->size) {		// here it will check if no of chars == E.cx then our cursor will no furthur move towards right, it will stick after the last char in a line.
        			E.cx++;
      			} else if (row && E.cx == row->size) 
			  {
        			E.cy++;
        			E.cx = 0;			// we are setting E.cx to the start of row
      			  }
      			break;
    		case ARROW_UP:
      			if (E.cy != 0) {			// prevent moving cursor out of the screen
        			E.cy--;			// y-axis, move accross y-axis(verticle)
      			}
      			break;
    		case ARROW_DOWN:
      			if (E.cy < E.numrows) {		// prevent moving cursor out of the bottom of file
        			E.cy++;
      			}
	      		break;
	}
  	
	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  	int rowlen = row ? row->size : 0;			// here we are getting no of chars in a row
  	if (E.cx > rowlen) {					// we are pointing our cursor at the end of each row
    		E.cx = rowlen;
  	}

}

void editorProcessKeypress()
{
	int c = editorReadKey();

	switch (c)
	{
		case '\r':			// enter key
      			editorInsertNewline();
      			break;

		case CTRL_KEY('q'):
		 write(STDOUT_FILENO, "\x1b[2J", 4);		// clear whole screen
		 write(STDOUT_FILENO, "\x1b[H", 3);		// reposition our cursor to top left
		 exit(0);
		 break;

		case CTRL_KEY('s'):
      			editorSave();
      			break;    		

		case HOME_KEY:
			E.cx = 0;			// move at start of line
			break;

		case END_KEY:
			if (E.cy < E.numrows)		// will check our cursor is pointing within file's rows, just for ensurity purpose
        			E.cx = E.row[E.cy].size;	// will get row size and move cursor at end of row
			break;
// in modern computers Backspace key is mapped to 127, and the Delete key is mapped to the escape sequence <esc>[3~
		case BACKSPACE:
		case CTRL_KEY('h'): // this will sends the control code 8, which is originally what the Backspace character would send back in the day
    		case DEL_KEY:
      			if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);	// if we press del key then it will move to next char and del that char other wise if we press backspace then it will simply replace previous car with next one.
      			editorDelChar();
      			break;

		case PAGE_UP:
    		case PAGE_DOWN:
      			{		// we use {} here coz we need a variable times which we cann't create directly in switch.
        			if (c == PAGE_UP) {
          				E.cy = E.rowoff;	// position cursor to top of screen
        			} else if (c == PAGE_DOWN) {
          				E.cy = E.rowoff + E.screenrows - 1; // position cursor to bottom of second screen if total rows of file is equal to 30 and screenrows are 20 then rowoff will be 10.
          				if (E.cy > E.numrows) E.cy = E.numrows;
        			  }        			
				int times = E.screenrows;		// will move at top or bottom
        			while (times--)				// run upto windowsize->rows times
          			editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN); // will move cursor up-word or down-word
      			}
      			break;

		case ARROW_UP:
    		case ARROW_DOWN:
    		case ARROW_LEFT:
    		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
// Ctrl-L is traditionally used to refresh the screen in terminal programs. In our text editor, the screen refreshes after any keypress, so we don’t have to do anything else to implement that feature
		case CTRL_KEY('l'):
    		case '\x1b': // escape sequences that we aren’t handling (such as the F1–F12 keys), and the way we wrote editorReadKey(), pressing those keys will be equivalent to pressing the Escape key. We ignore those keypresses
      			break;

    		default:
      			editorInsertChar(c);
      			break;
	}
}

/**** init ****/

void initEditor()
{
	E.cx = 0;		// by default set out cursor to this point
	E.cy = 0;
	E.rx = 0;		// for tab position initially 0
	E.rowoff = 0;		// default value of the variable that will keep track of what row of the file the user is currently scrolled to
				// We initialize it to 0, which means we’ll be scrolled to the top of the file by default
	E.coloff = 0;
	E.numrows = 0;		// by default set number of rows = 0
	E.row = NULL;		// initially put NULL in E.row pointer, we use this for storing multiple rows
	E.filename = NULL;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1 )
		die("Error in getWindowSize");
}
	
int main(int argc, char *argv[])
{
	enableRawMode();
	initEditor();

	if (argc >= 2) 
	{
	  editorOpen(argv[1]);
	}

	while(1)
	{
	editorRefreshScreen();
	editorProcessKeypress();
	}

	return 0;
}
