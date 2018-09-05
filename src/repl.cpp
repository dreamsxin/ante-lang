#include "repl.h"
#include "target.h"
#include <vector>
#include <string>

#ifdef unix
#  include <unistd.h>
#  include <termios.h>
#  include <sys/ioctl.h>
#endif

using namespace std;
using namespace ante;
using namespace ante::parser;

extern char* lextxt;

extern "C" void* Ante_debug(Compiler *c, ArgTuple &tv);

namespace ante {

    unsigned int sl_pos = 0;
    unsigned int sl_history_pos = 0;
    vector<string> sl_history;

#ifdef unix
    winsize termSize;

#   define clearScreen() printf("\033[J")
#   define clearLine() printf("\033[2K\r")

#   define moveUp()    printf("\033[A")
#   define moveDown()  printf("\033[B")
#   define moveRight() printf("\033[C")
#   define moveLeft()  printf("\033[D")

    void updateTermSize(){
        winsize w;
        ioctl(0, TIOCGWINSZ, &w);
        if(w.ws_col != termSize.ws_col){
            termSize = w;
            clearScreen();
        }
    }

#elif defined(WIN32)
#  include <windows.h>
#  define getchar getchar_windows

	HANDLE h_in, h_out;
	DWORD cc, normal_mode, getch_mode;

	TCHAR getchar_windows() {
		TCHAR c = 0;
		SetConsoleMode(h_in, getch_mode);
		ReadConsole(h_in, &c, 1, &cc, NULL);
		SetConsoleMode(h_in, normal_mode);
		return c;
	}

	void clearLine() {
		DWORD numCharsWritten;
		CONSOLE_SCREEN_BUFFER_INFO csbi;

		// Get the number of character cells in the current buffer.
		if (!GetConsoleScreenBufferInfo(h_out, &csbi)){
			cerr << "Cannot get screen buffer info" << endl;
			return;
		}

		COORD homeCoords = { 0, csbi.dwCursorPosition.Y };
		DWORD cellsToWrite = csbi.dwSize.X;

		// Fill the entire screen with blanks.
		if (!FillConsoleOutputCharacter(h_out, (TCHAR) ' ', cellsToWrite, homeCoords, &numCharsWritten)) {
			cerr << "Error when attempting to clear screen" << endl;
			return;
		}

		// Get the current text attribute.
		if (!GetConsoleScreenBufferInfo(h_out, &csbi)) {
			cerr << "Error when getting screen buffer info" << endl;
			return;
		}

		// Set the buffer's attributes accordingly.
		if (!FillConsoleOutputAttribute(h_out, csbi.wAttributes, cellsToWrite, homeCoords, &numCharsWritten)) {
			cerr << "Error when attempting to fill attributes" << endl;
			return;
		}

		SetConsoleCursorPosition(h_out, homeCoords);
	}

    void moveToPos(unsigned int pos, unsigned int cur_pos, string &line){}

    void clearScreen(){}

    void updateTermSize(){}

    void moveUp(){}

    void moveDown(){}

    void moveRight(){}

    void moveLeft(){}
#endif

    pair<unsigned int, unsigned int> getCoordOfPos(unsigned int pos, string &s){
        unsigned int lin = 0;
        unsigned int col = 0;

        for(unsigned int i = 0; i < pos; i++){
            if(s[i] == '\n'){
                lin++;
                col = 0;
            }else{
                col++;
            }
        }
        return {col, lin};
    }

    vector<unsigned int> getLineLengths(string &lines){
        vector<unsigned int> ret;
        unsigned int len = 0;
        for(auto &c : lines){
            if(c == '\n'){
                ret.push_back(len);
                len = 0;
            }else{
                len++;
            }
        }
        return ret;
    }

    void moveBackToOriginFrom(pair<unsigned int, unsigned int> c){
        for(unsigned int i = 0; i < c.first; i++){
            moveLeft();
        }

        for(unsigned int i = 0; i < c.second; i++){
            moveUp();
        }
    }

    void clearLines(unsigned int lines){
        for(unsigned int i = 0; i < lines; i++){
            clearLine();
            if(i != lines - 1)
                moveDown();
        }
    }

    void moveToPos(unsigned int pos, unsigned int cur_pos, string &line){
        auto len = getCoordOfPos(cur_pos, line);

        //account for the fact that ": " precedes
        //the first line and thus every other line starts
        //2 columns before 0,0
        moveBackToOriginFrom(len);
        if(len.second != 0){
            moveRight();
            moveRight();
        }

        auto c = getCoordOfPos(pos, line);

        //opposite of the above adjustment
        if(c.second != 0){
            moveLeft();
            moveLeft();
        }

        for(unsigned int i = 0; i < c.first; i++){
            moveRight();
        }

        for(unsigned int i = 0; i < c.second; i++){
            moveDown();
        }
    }



    void appendHistory(string &line){
        if(!line.empty() && (sl_history.empty() or line != sl_history.back()))
            sl_history.push_back(line);
    }

    void previousLineInHistory(string &line){
        if(sl_history_pos == sl_history.size()){
            appendHistory(line);
        }

        if(sl_history_pos > 0){
            sl_history_pos--;
            line = sl_history[sl_history_pos];
            sl_pos = line.length();
        }
    }

    void nextLineInHistory(string &line){
        if(sl_history_pos < sl_history.size() - 1){
            sl_history_pos++;
            line = sl_history[sl_history_pos];
            sl_pos = line.length();
        }else if(sl_history_pos == sl_history.size() - 1){
            line = "";
            sl_history_pos++;
            sl_pos = 0;
        }
    }

    void insertCharAt(string &line, char c, unsigned int pos){
        if(pos == line.length()){
            line += c;
        }else{
            line = line.substr(0, pos) + c + line.substr(pos);
        }
    }

    void handleEscSeq(string &line){
        if(getchar() == '['){
            char escSeq = getchar();
            if(escSeq == 68 && sl_pos > 0){ //move left
                sl_pos--;
            }else if(escSeq == 67 && sl_pos < line.length()){ //right
                sl_pos++;
            }else if(escSeq == 65){ //up
                previousLineInHistory(line);
            }else if(escSeq == 66){ //down
                nextLineInHistory(line);
            }
        }
    }

    bool lastCharIsOpenBracket(string &line){
        for(auto it = line.rbegin(); it != line.rend(); it++){
            if(*it != ' ' && *it != '\t' && *it != '\r' && *it != '\n'){
                return *it == '{';
            }
        }
        return false;
    }

    /**
     *  Called whenever return is pressed in the REPL
     *
     *  Returns true if the line is finished and should be
     *  evaluated.  This is only false if a \ precedes the
     *  newline character or the last non-whitespace char
     *  is a {
     */
    bool handleNewline(string &line, char nlChar){
        if(!line.empty() && line[sl_pos-1] == '\\'){
            if(nlChar == '\r'){
                //overwrite backslash
                line[sl_pos-1] = '\r';
                insertCharAt(line, '\n', sl_pos++);
            }else{
                line[sl_pos-1] = '\n';
            }
            return false;
        }

        //lex through input to ensure all brackets are matched
        LOC_TY loc;
        auto l = Lexer(nullptr, line, 1, 1, false);
        while (l.next(&loc)){ /* do nothing*/ };

        //unmatched {
        if(l.getManualScopeLevel() > 0){
            if(nlChar == '\r')
                insertCharAt(line, '\r', sl_pos++);
            insertCharAt(line, '\n', sl_pos++);
            return false;
        }

        //append the line to the history before the newline is added
        appendHistory(line);
        sl_history_pos = sl_history.size();
        if(nlChar == '\r'){
            line += '\r';
        }
        line += '\n';
        return true;
    }

    void removeCharAt(unsigned int pos, string &line){
        if(pos > 0 && pos <= line.length()){
            line = line.substr(0, pos - 1) + line.substr(pos);
            sl_pos--;
        }
    }

    string getInputColorized(){
        string line = "";

        cout << "\r: " << flush;
        char inp = getchar();

        sl_pos = 0;

        while(inp){
            updateTermSize();

            //stop on newlines unless there is a \ before.
            if(inp == '\n' or inp == '\r'){
                if(handleNewline(line, inp))
                    break;
            }else if(inp == '\t'){
                line += "    "; //replace tabs with 4 spaces
                sl_pos += 4;
            }else if(inp == '\b' or inp == 127){
                removeCharAt(sl_pos, line);
            }else if(inp == '\033'){
                handleEscSeq(line);
            }else{
                insertCharAt(line, inp, sl_pos);
                sl_pos++;
            }

#if defined(unix) || defined(_WIN32)
            //use lexer for syntax highlighting
            LOC_TY loc;
			auto l = Lexer(nullptr, line, 1, 1, true);
			while (l.next(&loc)){ /* do nothing*/ };

            //move cursor from end of text to the current pos
            moveToPos(sl_pos, line.length(), line);
            inp = getchar();

            //reset line to :
            auto coords = getCoordOfPos(sl_pos, line);
            moveBackToOriginFrom(coords);

            auto max_coords = getCoordOfPos(line.length(), line);
            clearLines(max_coords.second + 1);
            moveBackToOriginFrom({0, max_coords.second});

            cout << ": ";
#endif
        }

#if defined(unix) || defined(_WIN32)
            LOC_TY loc;
			auto l = Lexer(nullptr, line, 1, 1, true);
			while (l.next(&loc)){ /* do nothing*/ };
#endif
        return line;
    }

    /**
     * Disables character echoing and enables per-character input for getchar
     */
    void setupTerm(){
#ifdef unix
        termios newt;
        tcgetattr(STDIN_FILENO, &newt);
        newt.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        ioctl(0, TIOCGWINSZ, &termSize);
#elif defined WIN32
		h_in = GetStdHandle(STD_INPUT_HANDLE);
		h_out = GetStdHandle(STD_OUTPUT_HANDLE);
		if (!h_in or !h_out) {
			fputs("Error when attempting to access windows terminal\n", stderr);
			exit(1);
		}
		GetConsoleMode(h_in, &normal_mode);
		getch_mode = normal_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
#endif
    }

    /** Undo any changes done by setupTerm */
    void resetTerm(){
#ifdef unix
        termios newt;
        tcgetattr(STDIN_FILENO, &newt);
        newt.c_lflag |= (ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        ioctl(0, TIOCGWINSZ, &termSize);
#elif defined WIN32
		h_in = GetStdHandle(STD_INPUT_HANDLE);
		h_out = GetStdHandle(STD_OUTPUT_HANDLE);
		if (!h_in or !h_out) {
			fputs("Error when attempting to access windows terminal\n", stderr);
			exit(1);
		}
		GetConsoleMode(h_in, &normal_mode);
		getch_mode = normal_mode | (ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
#endif
    }

    /**
     * Output a value from the REPL by using its print function if found.
     */
    void output(Compiler *c, TypedValue &tv){
        try {
            ArgTuple arg{c, tv};
            Ante_debug(c, arg);
        }catch(ante::CompilationError *err){
            cout << err->msg << endl;
            delete err;
        }
    }


    void startRepl(Compiler *c){
        cout << "Ante REPL v0.2.0\nType 'exit' to exit.\n";
        setupTerm();

        auto cmd = getInputColorized();

        while(cmd != "exit\n"){
            int flag;
            //Catch any lexing errors
            try{
                //lex and parse the new string
                setLexer(new Lexer(nullptr, cmd, /*line*/1, /*col*/1));
                yy::parser p{};
                flag = p.parse();
            }catch(CtError *e){
                delete e;
                continue;
            }

            if(flag == PE_OK){
                RootNode *expr = parser::getRootNode();

                //Compile each expression and hold onto the last value
                TypedValue val = c->getAST() ? mergeAndCompile(c, expr)
                               : (c->compUnit->ast.reset(expr), CompilingVisitor::compile(c, expr));

                //print val if it's not an error
                if(!!val and val.type->typeTag != TT_Void)
                    output(c, val);
            }

            cmd = getInputColorized();
        }

        resetTerm();
    }

    TypedValue mergeAndCompile(Compiler *c, RootNode *rn){
        scanImports(c, rn);
        move(rn->imports.begin(),
            next(rn->imports.begin(), rn->imports.size()),
            back_inserter(c->getAST()->imports));

        for(auto &t : rn->types){
            safeCompile(c, t);
            c->getAST()->types.emplace_back(move(t));
        }

        for(auto &t : rn->traits){
            safeCompile(c, t);
            c->getAST()->traits.emplace_back(move(t));
        }

        for(auto &t : rn->extensions){
            safeCompile(c, t);
            c->getAST()->extensions.emplace_back(move(t));
        }

        for(auto &t : rn->funcs){
            safeCompile(c, t);
            c->getAST()->funcs.emplace_back(move(t));
        }

        TypedValue ret;
        for(auto &e : rn->main){
            ret = safeCompile(c, e);
            c->getAST()->main.emplace_back(move(e));
        }
        return ret;
    }
}
