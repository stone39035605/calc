/*
 * Copyright (c) 1997 David I. Bell
 * Permission is granted to use, distribute, or modify this source,
 * provided that this copyright notice remains intact.
 *
 * Module to generate opcodes from the input tokens.
 */

#include <stdio.h>
#include "have_unistd.h"
#if defined(HAVE_UNISTD_H)
#include <unistd.h>
#endif

#include "calc.h"
#include "token.h"
#include "symbol.h"
#include "label.h"
#include "opcodes.h"
#include "string.h"
#include "func.h"
#include "conf.h"

static BOOL rdonce;	/* TRUE => do not reread this file */

FUNC *curfunc;

static BOOL getfilename(char *name, BOOL msg_ok, BOOL *once);
static BOOL getid(char *buf);
static void getshowstatement(void);
static void getfunction(void);
static void ungetfunction(void);
static void getbody(LABEL *contlabel, LABEL *breaklabel,
		    LABEL *nextcaselabel, LABEL *defaultlabel);
static void getdeclarations(int symtype);
static void getsimpledeclaration (int symtype);
static int getonevariable (int symtype);
static void getstatement(LABEL *contlabel, LABEL *breaklabel,
			 LABEL *nextcaselabel, LABEL *defaultlabel);
static void getobjdeclaration(int symtype);
static void getoneobj(long index, int symtype);
static void getobjvars(char *name, int symtype);
static void getmatdeclaration(int symtype);
static void getonematrix(int symtype);
static void creatematrix(void);
static void getsimplebody(void);
static void getcondition(void);
static void getmatargs(void);
static void getelement(void);
static void usesymbol(char *name, BOOL autodef);
static void definesymbol(char *name, int symtype);
static void getcallargs(char *name);
static void do_changedir(void);
static int getexprlist(void);
static int getopassignment(void);
static int getassignment(void);
static int getaltcond(void);
static int getorcond(void);
static int getandcond(void);
static int getrelation(void);
static int getsum(void);
static int getproduct(void);
static int getorexpr(void);
static int getandexpr(void);
static int getshiftexpr(void);
static int getreference(void);
static int getincdecexpr(void);
static int getterm(void);
static int getidexpr(BOOL okmat, BOOL autodef);
static long getinitlist(void);


/*
 * Read all the commands from an input file.
 * These are either declarations, or else are commands to execute now.
 * In general, commands are terminated by newlines or semicolons.
 * Exceptions are function definitions and escaped newlines.
 * Commands are read and executed until the end of file.
 * The toplevel flag indicates whether we are at the top interactive level.
 */
void
getcommands(BOOL toplevel)
{
	char name[MAXCMD+1+1];	/* program name */

	/* firewall */
	name[0] = '\0';
	name[MAXCMD+1] = '\0';

	/* getcommands */
	if (!toplevel)
		enterfilescope();
	for (;;) {
		(void) tokenmode(TM_NEWLINES);
		switch (gettoken()) {

		case T_DEFINE:
			getfunction();
			break;

		case T_UNDEFINE:
			ungetfunction();
			break;

		case T_EOF:
			if (!toplevel)
				exitfilescope();
			return;

		case T_HELP:
			if (!getfilename(name, FALSE, NULL)) {
				strcpy(name, DEFAULTCALCHELP);
			}
			givehelp(name);
			break;

		case T_READ:
			if (!getfilename(name, TRUE, &rdonce))
				break;
			if (!allow_read) {
				scanerror(T_NULL,
				    "read command disallowed by -m mode\n");
				break;
			}
			switch (opensearchfile(name,calcpath,CALCEXT,rdonce)) {
			case 0:
				getcommands(FALSE);
				closeinput();
				break;
			case 1:
				/* previously read and -once was given */
				break;
			case -2:
				scanerror(T_NULL, "Maximum input depth reached");
				break;
			default:
				scanerror(T_NULL, "Cannot open \"%s\"\n", name);
				break;
			}
			break;

		case T_WRITE:
			if (!getfilename(name, TRUE, NULL))
				break;
			if (!allow_write) {
				scanerror(T_NULL,
				    "write command disallowed by -m mode\n");
				break;
			}
			if (writeglobals(name))
				scanerror(T_NULL, "Error writing \"%s\"\n", name);
			break;

		case T_CD:
			do_changedir();
			break;
		case T_NEWLINE:
		case T_SEMICOLON:
			break;

		default:
			rescantoken();
			initstack();
			if (evaluate(FALSE))
				updateoldvalue(curfunc);
			freefunc(curfunc);
		}
	}
}


/*
 * Evaluate a line of statements.
 * This is done by treating the current line as a function body,
 * compiling it, and then executing it.  Returns TRUE if the line
 * successfully compiled and executed.  The last expression result
 * is saved in the f_savedvalue element of the current function.
 * The nestflag variable should be FALSE for the outermost evaluation
 * level, and TRUE for all other calls (such as the 'eval' function).
 * The function name begins with an asterisk to indicate specialness.
 *
 * given:
 *	nestflag		TRUE if this is a nested evaluation
 */
BOOL
evaluate(BOOL nestflag)
{
	char *funcname;
	int loop = 1;		/* 0 => end the main while loop */

	funcname = (nestflag ? "**" : "*");
	beginfunc(funcname, nestflag);
	if (gettoken() == T_LEFTBRACE) {
		getbody(NULL_LABEL, NULL_LABEL, NULL_LABEL, NULL_LABEL);
	} else {
		if (nestflag)
			(void) tokenmode(TM_DEFAULT);
		rescantoken();
		while (loop) {
			switch (gettoken()) {
				case T_SEMICOLON:
					break;
				case T_NEWLINE:
				case T_EOF:
					loop = 0;
					break;

				default:
					rescantoken();
					getstatement(NULL_LABEL, NULL_LABEL,
						NULL_LABEL, NULL_LABEL);
			}
		}
	}
	addop(OP_UNDEF);
	addop(OP_RETURN);
	checklabels();
	if (errorcount)
		return FALSE;
	calculate(curfunc, 0);
	return TRUE;
}

/*
 * Undefine one or more functions
 */
static void
ungetfunction(void)
{
	char *name;
	int type;

	for (;;) {
		switch (gettoken()) {
			case T_COMMA:
				continue;
			case T_SYMBOL:
				name = tokensymbol();
				type = getbuiltinfunc(name);
				if (type >= 0) {
					fprintf(stderr,
		 "Attempt to undefine builtin function \"%s\" ignored\n",
			name);
				continue;
				}
				rmuserfunc(name);
				continue;
			case T_MULT:
				rmalluserfunc();
				continue;
			default:
				rescantoken();
				return;
		}
	}
}


/*
 * Get a function declaration.
 * func = name '(' '' | name [ ',' name] ... ')' simplebody
 *	| name '(' '' | name [ ',' name] ... ')' body.
 */
static void
getfunction(void)
{
	char *name;		/* parameter name */
	int type;		/* type of token read */
	LABEL label;
	long index;

	(void) tokenmode(TM_DEFAULT);
	if (gettoken() != T_SYMBOL) {
		scanerror(T_NULL, "Function name was expected");
		return;
	}
	name = tokensymbol();
	type = getbuiltinfunc(name);
	if (type >= 0) {
		scanerror(T_SEMICOLON, "Using builtin function name");
		return;
	}
	beginfunc(name, FALSE);
	enterfuncscope();
	if (gettoken() != T_LEFTPAREN) {
		scanerror(T_SEMICOLON,
			"Left parenthesis expected for function");
		return;
	}
	index = 0;
	for (;;) {
		type = gettoken();
		if (type == T_RIGHTPAREN)
			break;
		if (type != T_SYMBOL) {
			scanerror(T_COMMA, "Bad function definition");
			return;
		}
		name = tokensymbol();
		switch (symboltype(name)) {
			case SYM_UNDEFINED:
			case SYM_GLOBAL:
			case SYM_STATIC:
				index = addparam(name);
				break;
			default:
				scanerror(T_NULL, "Parameter \"%s\" is already defined", name);
		}
		type = gettoken();
		if (type == T_ASSIGN) {
			clearlabel(&label);
			addopone(OP_PARAMADDR, index);
			addoplabel(OP_JUMPNN, &label);
			getopassignment();
			addop(OP_ASSIGNPOP);
			setlabel(&label);
			type = gettoken();
		}

		if (type == T_RIGHTPAREN)
			break;
		if (type != T_COMMA) {
			scanerror(T_COMMA, "Bad function definition");
			return;
		}
	}
	switch (gettoken()) {
		case T_ASSIGN:
			getsimplebody();
			break;
		case T_LEFTBRACE:
			getbody(NULL_LABEL, NULL_LABEL, NULL_LABEL,
				NULL_LABEL);
			break;
		default:
			scanerror(T_NULL,
				"Left brace or equals sign expected for function");
			return;
	}
	endfunc();
	exitfuncscope();
}


/*
 * Get a simple assignment style body for a function declaration.
 * simplebody = '=' assignment '\n'.
 */
static void
getsimplebody(void)
{
	(void) tokenmode(TM_NEWLINES);
	(void) getexprlist();
	addop(OP_RETURN);
}


/*
 * Get the body of a function, or a subbody of a function.
 * body = '{' [ declarations ] ... [ statement ] ... '}'
 *	| [ declarations ] ... [statement ] ... '\n'
 */
/*ARGSUSED*/
static void
getbody(LABEL *contlabel, LABEL *breaklabel, LABEL *nextcaselabel, LABEL *defaultlabel)
{
	int oldmode;

	oldmode = tokenmode(TM_DEFAULT);
	while (TRUE) {
		switch (gettoken()) {
		case T_RIGHTBRACE:
			(void) tokenmode(oldmode);
			return;

		case T_EOF:
			scanerror(T_SEMICOLON, "End-of-file in function body");
			return;

		default:
			rescantoken();
			getstatement(contlabel, breaklabel, nextcaselabel, defaultlabel);
		}
	}
}


/*
 * Get a line of possible local, global, or static variable declarations.
 * declarations = { LOCAL | GLOBAL | STATIC } onedeclaration
 *	[ ',' onedeclaration ] ... ';'.
 */
static void
getdeclarations(int symtype)
{
	while (TRUE) {
		switch (gettoken()) {
			case T_COMMA:
				continue;

			case T_NEWLINE:
			case T_SEMICOLON:
			case T_RIGHTBRACE:
				rescantoken();
				return;

			case T_SYMBOL:
				addopone(OP_DEBUG, linenumber());
				rescantoken();
				getsimpledeclaration(symtype);
				break;

			case T_MAT:
				addopone(OP_DEBUG, linenumber());
				getmatdeclaration(symtype);
				break;

			case T_OBJ:
				addopone(OP_DEBUG, linenumber());
				getobjdeclaration(symtype);
				addop(OP_POP);
				break;

			default:
				scanerror(T_SEMICOLON, "Bad syntax in declaration statement");
				return;
		}
	}
}


/*
 * Get declaration of a sequence of simple identifiers, as in
 *	global a, b = 1, c d = 2, d;
 * Subsequences end with "," or at end of line; spaces indicate
 * repeated assignment, e.g. "c d = 2" has the effect of "c = 2, d = 2".
 */
static void
getsimpledeclaration(int symtype)
{

	for (;;) {
		switch (gettoken()) {
			case T_SYMBOL:
				rescantoken();
				if (getonevariable(symtype))
					addop(OP_POP);
				continue;
			case T_COMMA:
				continue;
			default:
				rescantoken();
				return;
		}
	}
}


/*
 * Get one variable in a sequence of simple identifiers.
 * Returns 1 if the subsequence in which the variable occurs ends with
 * an assignment, e.g. for the variables b, c, d, in
 *	static a, b = 1, c d = 2, d;
 */
static int
getonevariable(int symtype)
{
	char *name;
	int res = 0;

	switch(gettoken()) {
		case T_SYMBOL:
			name = addliteral(tokensymbol());
			res = getonevariable(symtype);
			definesymbol(name, symtype);
			if (res) {
				usesymbol(name, FALSE);
				addop(OP_ASSIGNBACK);
			}
			return res;
		case T_ASSIGN:
			getopassignment();
			rescantoken();
			return 1;
		default:
			rescantoken();
			return 0;
	}
}

/*
 * Get a statement.
 * statement = IF condition statement [ELSE statement]
 *	| FOR '(' [assignment] ';' [assignment] ';' [assignment] ')' statement
 *	| WHILE condition statement
 *	| DO statement WHILE condition ';'
 *	| SWITCH condition '{' [caseclause] ... '}'
 *	| CONTINUE ';'
 *	| BREAK ';'
 *	| RETURN assignment ';'
 *	| GOTO label ';'
 *	| MAT name '[' value [ ':' value ] [',' value [ ':' value ] ] ']' ';'
 *	| OBJ type '{' arg [ ',' arg ] ... '}' ] ';'
 *	| OBJ type name [ ',' name ] ';'
 *	| PRINT assignment [, assignment ] ... ';'
 *	| QUIT [ string ] ';'
 *	| SHOW item ';'
 *	| body
 *	| assignment ';'
 *	| label ':' statement
 *	| ';'.
 *
 * given:
 *	contlabel		label for continue statement
 *	breaklabel		label for break statement
 *	nextcaselabel		label for next case statement
 *	defaultlabel		label for default case
 */
static void
getstatement(LABEL *contlabel, LABEL *breaklabel, LABEL *nextcaselabel, LABEL *defaultlabel)
{
	LABEL label;
	LABEL label1, label2, label3, label4;	/* locations for jumps */
	int type;
	BOOL printeol;
	int oldmode;

	addopone(OP_DEBUG, linenumber());
	switch (gettoken()) {
	case T_NEWLINE:
	case T_SEMICOLON:
		return;

	case T_GLOBAL:
		getdeclarations(SYM_GLOBAL);
		break;

	case T_STATIC:
		clearlabel(&label);
		addoplabel(OP_INITSTATIC, &label);
		getdeclarations(SYM_STATIC);
		setlabel(&label);
		break;

	case T_LOCAL:
		getdeclarations(SYM_LOCAL);
		break;

	case T_RIGHTBRACE:
		scanerror(T_NULL, "Extraneous right brace");
		return;

	case T_CONTINUE:
		if (contlabel == NULL_LABEL) {
			scanerror(T_SEMICOLON, "CONTINUE not within FOR, WHILE, or DO");
			return;
		}
		addoplabel(OP_JUMP, contlabel);
		break;

	case T_BREAK:
		if (breaklabel == NULL_LABEL) {
			scanerror(T_SEMICOLON, "BREAK not within FOR, WHILE, or DO");
			return;
		}
		addoplabel(OP_JUMP, breaklabel);
		break;

	case T_GOTO:
		if (gettoken() != T_SYMBOL) {
			scanerror(T_SEMICOLON, "Missing label in goto");
			return;
		}
		addop(OP_JUMP);
		addlabel(tokensymbol());
		break;

	case T_RETURN:
		switch (gettoken()) {
			case T_NEWLINE:
			case T_SEMICOLON:
				addop(OP_UNDEF);
				addop(OP_RETURN);
				return;
			default:
				rescantoken();
				(void) getexprlist();
				if (curfunc->f_name[0] == '*')
					addop(OP_SAVE);
				addop(OP_RETURN);
		}
		break;

	case T_LEFTBRACE:
		getbody(contlabel, breaklabel, nextcaselabel, defaultlabel);
		return;

	case T_IF:
		clearlabel(&label1);
		clearlabel(&label2);
		getcondition();
		switch(gettoken()) {
			case T_CONTINUE:
				if (contlabel == NULL_LABEL) {
					scanerror(T_SEMICOLON, "CONTINUE not within FOR, WHILE, or DO");
					return;
				}
				addoplabel(OP_JUMPNZ, contlabel);
				break;
			case T_BREAK:
				if (breaklabel == NULL_LABEL) {
					scanerror(T_SEMICOLON, "BREAK not within FOR, WHILE, or DO");
					return;
				}
				addoplabel(OP_JUMPNZ, breaklabel);
				break;
			case T_GOTO:
				if (gettoken() != T_SYMBOL) {
					scanerror(T_SEMICOLON, "Missing label in goto");
					return;
				}
				addop(OP_JUMPNZ);
				addlabel(tokensymbol());
				break;
			default:
				addoplabel(OP_JUMPZ, &label1);
				rescantoken();
				getstatement(contlabel, breaklabel, NULL_LABEL, NULL_LABEL);
				if (gettoken() != T_ELSE) {
					setlabel(&label1);
					rescantoken();
					return;
				}
				addoplabel(OP_JUMP, &label2);
				setlabel(&label1);
				getstatement(contlabel, breaklabel, NULL_LABEL, NULL_LABEL);
				setlabel(&label2);
				return;
		}
		if (gettoken() != T_SEMICOLON) /* This makes ';' optional */
			rescantoken();
		if (gettoken() != T_ELSE) {
			rescantoken();
			return;
		}
		getstatement(contlabel, breaklabel, NULL_LABEL, NULL_LABEL);
		return;

	case T_FOR:	/* for (a; b; c) x */
		oldmode = tokenmode(TM_DEFAULT);
		clearlabel(&label1);
		clearlabel(&label2);
		clearlabel(&label3);
		clearlabel(&label4);
		contlabel = NULL_LABEL;
		breaklabel = &label4;
		if (gettoken() != T_LEFTPAREN) {
			(void) tokenmode(oldmode);
			scanerror(T_SEMICOLON, "Left parenthesis expected");
			return;
		}
		if (gettoken() != T_SEMICOLON) {	/* have 'a' part */
			rescantoken();
			(void) getexprlist();
			addop(OP_POP);
			if (gettoken() != T_SEMICOLON) {
				(void) tokenmode(oldmode);
				scanerror(T_SEMICOLON, "Missing semicolon");
				return;
			}
		}
		if (gettoken() != T_SEMICOLON) {	/* have 'b' part */
			setlabel(&label1);
			contlabel = &label1;
			rescantoken();
			(void) getexprlist();
			addoplabel(OP_JUMPNZ, &label3);
			addoplabel(OP_JUMP, breaklabel);
			if (gettoken() != T_SEMICOLON) {
				(void) tokenmode(oldmode);
				scanerror(T_SEMICOLON, "Missing semicolon");
				return;
			}
		}
		if (gettoken() != T_RIGHTPAREN) {	/* have 'c' part */
			if (label1.l_offset <= 0)
				addoplabel(OP_JUMP, &label3);
			setlabel(&label2);
			contlabel = &label2;
			rescantoken();
			(void) getexprlist();
			addop(OP_POP);
			if (label1.l_offset > 0)
				addoplabel(OP_JUMP, &label1);
			if (gettoken() != T_RIGHTPAREN) {
				(void) tokenmode(oldmode);
				scanerror(T_SEMICOLON, "Right parenthesis expected");
				return;
			}
		}
		setlabel(&label3);
		if (contlabel == NULL_LABEL)
			contlabel = &label3;
		getstatement(contlabel, breaklabel, NULL_LABEL, NULL_LABEL);
		addoplabel(OP_JUMP, contlabel);
		setlabel(breaklabel);
		(void) tokenmode(oldmode);
		return;

	case T_WHILE:
		oldmode = tokenmode(TM_DEFAULT);
		contlabel = &label1;
		breaklabel = &label2;
		clearlabel(contlabel);
		clearlabel(breaklabel);
		setlabel(contlabel);
		getcondition();
		addoplabel(OP_JUMPZ, breaklabel);
		getstatement(contlabel, breaklabel, NULL_LABEL, NULL_LABEL);
		addoplabel(OP_JUMP, contlabel);
		setlabel(breaklabel);
		(void) tokenmode(oldmode);
		return;

	case T_DO:
		oldmode = tokenmode(TM_DEFAULT);
		contlabel = &label1;
		breaklabel = &label2;
		clearlabel(contlabel);
		clearlabel(breaklabel);
		clearlabel(&label3);
		setlabel(&label3);
		getstatement(contlabel, breaklabel, NULL_LABEL, NULL_LABEL);
		if (gettoken() != T_WHILE) {
			(void) tokenmode(oldmode);
			scanerror(T_SEMICOLON, "WHILE keyword expected for DO statement");
			return;
		}
		setlabel(contlabel);
		getcondition();
		addoplabel(OP_JUMPNZ, &label3);
		setlabel(breaklabel);
		(void) tokenmode(oldmode);
		return;

	case T_SWITCH:
		oldmode = tokenmode(TM_DEFAULT);
		breaklabel = &label1;
		nextcaselabel = &label2;
		defaultlabel = &label3;
		clearlabel(breaklabel);
		clearlabel(nextcaselabel);
		clearlabel(defaultlabel);
		getcondition();
		if (gettoken() != T_LEFTBRACE) {
			(void) tokenmode(oldmode);
			scanerror(T_SEMICOLON, "Missing left brace for switch statement");
			return;
		}
		addoplabel(OP_JUMP, nextcaselabel);
		rescantoken();
		getstatement(contlabel, breaklabel, nextcaselabel, defaultlabel);
		addoplabel(OP_JUMP, breaklabel);
		setlabel(nextcaselabel);
		if (defaultlabel->l_offset > 0)
			addoplabel(OP_JUMP, defaultlabel);
		else
			addop(OP_POP);
		setlabel(breaklabel);
		(void) tokenmode(oldmode);
		return;

	case T_CASE:
		if (nextcaselabel == NULL_LABEL) {
			scanerror(T_SEMICOLON, "CASE not within SWITCH statement");
			return;
		}
		clearlabel(&label1);
		addoplabel(OP_JUMP, &label1);
		setlabel(nextcaselabel);
		clearlabel(nextcaselabel);
		(void) getexprlist();
		if (gettoken() != T_COLON) {
			scanerror(T_SEMICOLON, "Colon expected after CASE expression");
			return;
		}
		addoplabel(OP_CASEJUMP, nextcaselabel);
		setlabel(&label1);
		getstatement(contlabel, breaklabel, nextcaselabel, defaultlabel);
		return;

	case T_DEFAULT:
		if (gettoken() != T_COLON) {
			scanerror(T_SEMICOLON, "Colon expected after DEFAULT keyword");
			return;
		}
		if (defaultlabel == NULL_LABEL) {
			scanerror(T_SEMICOLON, "DEFAULT not within SWITCH statement");
			return;
		}
		if (defaultlabel->l_offset > 0) {
			scanerror(T_SEMICOLON, "Multiple DEFAULT clauses in SWITCH");
			return;
		}
		clearlabel(&label1);
		addoplabel(OP_JUMP, &label1);
		setlabel(defaultlabel);
		addop(OP_POP);
		setlabel(&label1);
		getstatement(contlabel, breaklabel, nextcaselabel, defaultlabel);
		return;

	case T_ELSE:
		scanerror(T_SEMICOLON, "ELSE without preceeding IF");
		return;

	case T_SHOW:
		getshowstatement();
		break;

	case T_PRINT:
		printeol = TRUE;
		for (;;) {
			switch (gettoken()) {
				case T_RIGHTPAREN:
				case T_RIGHTBRACKET:
				case T_RIGHTBRACE:
				case T_NEWLINE:
				case T_EOF:
					rescantoken();
					/*FALLTHRU*/
				case T_SEMICOLON:
					if (printeol)
						addop(OP_PRINTEOL);
					return;
				case T_COMMA:
					addop(OP_PRINTSPACE);
					/*FALLTHRU*/
				case T_COLON:
					printeol = FALSE;
					break;
				case T_STRING:
					printeol = TRUE;
					addopone(OP_PRINTSTRING, tokenstring());
					break;
				default:
					printeol = TRUE;
					rescantoken();
					(void) getopassignment();
					addopone(OP_PRINT, (long) PRINT_NORMAL);
			}
		}

	case T_QUIT:
		switch (gettoken()) {
			case T_STRING:
				addopone(OP_QUIT, tokenstring());
				break;
			default:
				addopone(OP_QUIT, -1);
				rescantoken();
		}
		break;

	case T_ABORT:
		switch (gettoken()) {
			case T_STRING:
				addopone(OP_ABORT, tokenstring());
				break;
			default:
				addopone(OP_ABORT, -1);
				rescantoken();
		}
		break;

	case T_SYMBOL:
		if (nextchar() == ':') {	/****HACK HACK ****/
			definelabel(tokensymbol());
			if (gettoken() == T_RIGHTBRACE) {
				rescantoken();
				return;
			}
			rescantoken();
			getstatement(contlabel, breaklabel,
				NULL_LABEL, NULL_LABEL);
			return;
		}
		reread();
		/* fall into default case */

	default:
		rescantoken();
		type = getexprlist();
		if (contlabel || breaklabel || (curfunc->f_name[0] != '*')) {
			addop(OP_POP);
			break;
		}
		addop(OP_SAVE);
		if (isassign(type) || (curfunc->f_name[1] != '\0')) {
			addop(OP_POP);
			break;
		}
		addop(OP_PRINTRESULT);
		break;
	}
	for (;;) {
		switch (gettoken()) {
			case T_RIGHTBRACE:
			case T_NEWLINE:
			case T_EOF:
				rescantoken();
				return;
			case T_SEMICOLON:
				return;
			case T_NUMBER:
			case T_IMAGINARY:
				addopone(OP_NUMBER, tokennumber());
				scanerror(T_NULL, "Unexpected number");
				continue;
			default:
				scanerror(T_NULL, "Semicolon expected");
				return;
		}
	}
}


/*
 * Read in an object declaration.
 * This is of the following form:
 *	OBJ type [ '{' id [ ',' id ] ... '}' ]  [ objlist ].
 * The OBJ keyword has already been read.  Symtype is SYM_UNDEFINED if this
 * is an OBJ statement, otherwise this is part of a declaration which will
 * define new symbols with the specified type.
 */
static void
getobjdeclaration(int symtype)
{
	char *name;			/* name of object type */
	int count;			/* number of elements */
	int index;			/* current index */
	int i;				/* loop counter */
	int indices[MAXINDICES];	/* indices for elements */
	int oldmode;

	if (gettoken() != T_SYMBOL) {
		scanerror(T_SEMICOLON, "Object type name missing");
		return;
	}
	name = addliteral(tokensymbol());
	if (gettoken() != T_LEFTBRACE) {
		rescantoken();
		getobjvars(name, symtype);
		return;
	}
	/*
	 * Read in the definition of the elements of the object.
	 */
	count = 0;
	oldmode = tokenmode(TM_DEFAULT);
	for (;;) {
		switch (gettoken()) {
			case T_SYMBOL:
				if (count == MAXINDICES) {
					scanerror(T_SEMICOLON, "Too many elements in OBJ statement");
					(void) tokenmode(oldmode);
					return;
				}
				index = addelement(tokensymbol());
				for (i = 0; i < count; i++) {
					if (indices[i] == index) {
						scanerror(T_SEMICOLON, "Duplicate element name \"%s\"", tokensymbol());
						(void) tokenmode(oldmode);
						return;
					}
				}
				indices[count++] = index;
				if (gettoken() == T_COMMA)
					continue;
				rescantoken();
				if (gettoken() != T_RIGHTBRACE) {
					scanerror(T_SEMICOLON, "Bad object type definition");
					(void) tokenmode(oldmode);
					return;
				}
				/*FALLTHRU*/
			case T_RIGHTBRACE:
				(void) tokenmode(oldmode);
				(void) defineobject(name, indices, count);
				getobjvars(name, symtype);
				return;
			case T_NEWLINE:
				continue;
			default:
				scanerror(T_SEMICOLON, "Bad object type definition");
				(void) tokenmode(oldmode);
				return;
		}
	}
}

static void
getoneobj(long index, int symtype)
{
	char *symname;

	if (gettoken() == T_SYMBOL) {
		if (symtype == SYM_UNDEFINED) {
			rescantoken();
			(void) getidexpr(TRUE, TRUE);
		} else {
			symname = tokensymbol();
			definesymbol(symname, symtype);
			usesymbol(symname, FALSE);
		}
		getoneobj(index, symtype);
		addop(OP_ASSIGN);
		return;
	}
	rescantoken();
	addopone(OP_OBJCREATE, index);
	while (gettoken() == T_ASSIGN)
		(void) getinitlist();
	rescantoken();
}

/*
 * Routine to collect a set of variables for the specified object type
 * and initialize them as being that type of object.
 * Here
 *	objlist = name initlist [ ',' name initlist ] ... ';'.
 * If symtype is SYM_UNDEFINED, then this is an OBJ statement where the
 * values can be any variable expression, and no symbols are to be defined.
 * Otherwise this is part of a declaration, and the variables must be raw
 * symbol names which are defined with the specified symbol type.
 *
 * given:
 *	name		object name
 *	symtype		type of symbol to collect for
 */
static void
getobjvars(char *name, int symtype)
{
	long index;		/* index for object */

	index = checkobject(name);
	if (index < 0) {
		scanerror(T_SEMICOLON, "Object %s has not been defined yet", name);
		return;
	}
	for (;;) {
		getoneobj(index, symtype);
		if (gettoken() != T_COMMA) {
			rescantoken();
			return;
		}
		addop(OP_POP);
	}
}


static void
getmatdeclaration(int symtype)
{
	for (;;) {
		switch (gettoken()) {
			case T_SYMBOL:
				rescantoken();
				getonematrix(symtype);
				addop(OP_POP);
				continue;
			case T_COMMA:
				continue;
			default:
				rescantoken();
				return;
		}
	}
}


static void
getonematrix(int symtype)
{
	long dim;
	long index;
	long count;
	unsigned long patchpc;
	char *name;

	if (gettoken() == T_SYMBOL) {
		if (symtype == SYM_UNDEFINED) {
			rescantoken();
			(void) getidexpr(FALSE, TRUE);
		} else {
			name = tokensymbol();
			definesymbol(name, symtype);
			usesymbol(name, FALSE);
		}
		while (gettoken() == T_COMMA);
		rescantoken();
		getonematrix(symtype);
		addop(OP_ASSIGN);
		return;
	}
	rescantoken();

	if (gettoken() != T_LEFTBRACKET) {
		rescantoken();
		scanerror(T_SEMICOLON, "Left-bracket expected");
		return;
	}
	dim = 1;

	/*
	 * If there are no bounds given for the matrix, then they must be
	 * implicitly defined by a list of initialization values.  Put in
	 * a dummy number in the opcode stream for the bounds and remember
	 * its location.  After we know how many values are in the list, we
	 * will patch the correct value back into the opcode.
	 */
	if (gettoken() == T_RIGHTBRACKET) {
		clearopt();
		patchpc = curfunc->f_opcodecount + 1;
		addopone(OP_NUMBER, (long) -1);
		clearopt();
		addop(OP_ZERO);
		addopone(OP_MATCREATE, dim);
		addop(OP_ZERO);
		addop(OP_INITFILL);
		count = 0;
		if (gettoken() == T_ASSIGN)
			count = getinitlist();
		else
			rescantoken();
		index = addqconstant(itoq(count));
		if (index < 0)
			math_error("Cannot allocate constant");
		curfunc->f_opcodes[patchpc] = index;
		return;
	}

	/*
	 * This isn't implicit, so we expect expressions for the bounds.
	 */
	rescantoken();
	creatematrix();
	while (gettoken() == T_ASSIGN)
		(void) getinitlist();
	rescantoken();
}


static void
creatematrix(void)
{
	long dim;

	dim = 1;

	while (TRUE) {
		(void) getopassignment();
		switch (gettoken()) {
			case T_RIGHTBRACKET:
			case T_COMMA:
				rescantoken();
				addop(OP_ONE);
				addop(OP_SUB);
				addop(OP_ZERO);
				break;
			case T_COLON:
				(void) getopassignment();
				break;
			default:
				rescantoken();
		}
		switch (gettoken()) {
			case T_RIGHTBRACKET:
				addopone(OP_MATCREATE, dim);
				if (gettoken() == T_LEFTBRACKET) {
					creatematrix();
				} else {
					rescantoken();
					addop(OP_ZERO);
				}
				addop(OP_INITFILL);
				return;
			case T_COMMA:
				if (++dim <= MAXDIM)
					break;
				scanerror(T_SEMICOLON, "Only %ld dimensions allowed", MAXDIM);
				return;
			default:
				scanerror(T_SEMICOLON, "Illegal matrix definition");
				return;
		}
	}
}


/*
 * Get an optional initialization list for a matrix or object definition.
 * Returns the number of elements that are in the list, or -1 on parse error.
 *	initlist = { assignment [ , assignment ] ... }.
 */
static long
getinitlist(void)
{
	long index;
	int oldmode;

	oldmode = tokenmode(TM_DEFAULT);

	if (gettoken() != T_LEFTBRACE) {
		scanerror(T_SEMICOLON, "Missing brace for initialization list");
		(void) tokenmode(oldmode);
		return -1;
	}

	for (index = 0; ; index++) {
		switch(gettoken()) {
			case T_COMMA:
			case T_NEWLINE:
				continue;
			case T_RIGHTBRACE:
				(void) tokenmode(oldmode);
				return index;
			case T_LEFTBRACE:
				rescantoken();
				addop(OP_DUPLICATE);
				addopone(OP_ELEMADDR, index);
				(void) getinitlist();
				break;
			default:
				rescantoken();
				getopassignment();
		}
		addopone(OP_ELEMINIT, index);
		switch (gettoken()) {
			case T_COMMA:
			case T_NEWLINE:
				continue;

			case T_RIGHTBRACE:
				(void) tokenmode(oldmode);
				return index;

			default:
				scanerror(T_SEMICOLON, "Bad initialization list");
				(void) tokenmode(oldmode);
				return -1;
		}
	}
}


/*
 * Get a condition.
 * condition = '(' assignment ')'.
 */
static void
getcondition(void)
{
	if (gettoken() != T_LEFTPAREN) {
		scanerror(T_SEMICOLON, "Missing left parenthesis for condition");
		return;
	}
	(void) getexprlist();
	if (gettoken() != T_RIGHTPAREN) {
		scanerror(T_SEMICOLON, "Missing right parenthesis for condition");
		return;
	}
}


/*
 * Get an expression list consisting of one or more expressions,
 * separated by commas.  The value of the list is that of the final expression.
 * This is the top level routine for parsing expressions.
 * Returns flags describing the type of the last assignment or expression found.
 * exprlist = assignment [ ',' assignment ] ...
 */
static int
getexprlist(void)
{
	int	type;

	type = getopassignment();
	while (gettoken() == T_COMMA) {
		addop(OP_POP);
		type = getopassignment();
	}
	rescantoken();
	return type;
}


/*
 * Get an opassignment or possibly just an assignment or expression.
 * Returns flags describing the type of assignment or expression found.
 * assignment = lvalue '=' assignment
 *	| lvalue '+=' assignment
 *	| lvalue '-=' assignment
 *	| lvalue '*=' assignment
 *	| lvalue '/=' assignment
 *	| lvalue '%=' assignment
 *	| lvalue '//=' assignment
 *	| lvalue '&=' assignment
 *	| lvalue '|=' assignment
 *	| lvalue '<<=' assignment
 *	| lvalue '>>=' assignment
 *	| lvalue '^=' assignment
 *	| lvalue '**=' assignment
 *	| orcond.
 */
static int
getopassignment(void)
{
	int type;		/* type of expression */
	long op;		/* opcode to generate */

	type = getassignment();
	switch (gettoken()) {
		case T_PLUSEQUALS:	op = OP_ADD; break;
		case T_MINUSEQUALS:	op = OP_SUB; break;
		case T_MULTEQUALS:	op = OP_MUL; break;
		case T_DIVEQUALS:	op = OP_DIV; break;
		case T_SLASHSLASHEQUALS: op = OP_QUO; break;
		case T_MODEQUALS:	op = OP_MOD; break;
		case T_ANDEQUALS:	op = OP_AND; break;
		case T_OREQUALS:	op = OP_OR; break;
		case T_LSHIFTEQUALS: 	op = OP_LEFTSHIFT; break;
		case T_RSHIFTEQUALS: 	op = OP_RIGHTSHIFT; break;
		case T_POWEREQUALS:	op = OP_POWER; break;
		case T_HASHEQUALS:	op = OP_HASHOP; break;
		case T_TILDEEQUALS:	op = OP_XOR; break;
		case T_BACKSLASHEQUALS: op = OP_SETMINUS; break;

		default:
			rescantoken();
			return type;
	}
	if (isrvalue(type)) {
		scanerror(T_NULL, "Illegal assignment in getopassignment");
		(void) getopassignment();
		return (EXPR_RVALUE | EXPR_ASSIGN);
	}
	writeindexop();
	for(;;) {
		addop(OP_DUPLICATE);
		if (gettoken() == T_LEFTBRACE) {
			rescantoken();
			addop(OP_DUPVALUE);
			getinitlist();
			while (gettoken() == T_ASSIGN)
				getinitlist();
			rescantoken();
		} else {
			rescantoken();
			(void) getassignment();
		}
		addop(op);
		addop(OP_ASSIGN);
		switch (gettoken()) {
			case T_PLUSEQUALS:	op = OP_ADD; break;
			case T_MINUSEQUALS:	op = OP_SUB; break;
			case T_MULTEQUALS:	op = OP_MUL; break;
			case T_DIVEQUALS:	op = OP_DIV; break;
			case T_SLASHSLASHEQUALS: op = OP_QUO; break;
			case T_MODEQUALS:	op = OP_MOD; break;
			case T_ANDEQUALS:	op = OP_AND; break;
			case T_OREQUALS:	op = OP_OR; break;
			case T_LSHIFTEQUALS: 	op = OP_LEFTSHIFT; break;
			case T_RSHIFTEQUALS: 	op = OP_RIGHTSHIFT; break;
			case T_POWEREQUALS:	op = OP_POWER; break;
			case T_HASHEQUALS:	op = OP_HASHOP; break;
			case T_TILDEEQUALS:	op = OP_XOR; break;
			case T_BACKSLASHEQUALS: op = OP_SETMINUS; break;

			default:
				rescantoken();
				return EXPR_ASSIGN;
		}
	}
}


/*
 * Get an assignment (lvalue = ...) or possibly just an expression
 */

static int
getassignment (void)
{
	int type;		/* type of expression */

	switch(gettoken()) {
		case T_COMMA:
		case T_SEMICOLON:
		case T_NEWLINE:
		case T_RIGHTPAREN:
		case T_RIGHTBRACKET:
		case T_RIGHTBRACE:
		case T_EOF:
			addop(OP_UNDEF);
			rescantoken();
			return EXPR_RVALUE;
	}

	rescantoken();

	type = getaltcond();

	switch (gettoken()) {
		case T_NUMBER:
		case T_IMAGINARY:
			addopone(OP_NUMBER, tokennumber());
			type = (EXPR_RVALUE | EXPR_CONST);
			/*FALLTHRU*/
		case T_STRING:
		case T_SYMBOL:
		case T_OLDVALUE:
		case T_LEFTPAREN:
		case T_PLUSPLUS:
		case T_MINUSMINUS:
		case T_NOT:
			scanerror(T_NULL, "Missing operator");
			return type;
		case T_ASSIGN:
			break;

		default:
			rescantoken();
			return type;
	}
	if (isrvalue(type)) {
		scanerror(T_SEMICOLON, "Illegal assignment in getassignment");
		(void) getassignment();
		return (EXPR_RVALUE | EXPR_ASSIGN);
	}
	writeindexop();
	if (gettoken() == T_LEFTBRACE) {
		rescantoken();
		getinitlist();
		while (gettoken() == T_ASSIGN)
			getinitlist();
		rescantoken();
		return EXPR_ASSIGN;
	}
	rescantoken();
	(void) getassignment();
	addop(OP_ASSIGN);
	return EXPR_ASSIGN;
}


/*
 * Get a possible conditional result expression (question mark).
 * Flags are returned indicating the type of expression found.
 * altcond = orcond [ '?' orcond ':' altcond ].
 */
static int
getaltcond(void)
{
	int type;		/* type of expression */
	LABEL donelab;		/* label for done */
	LABEL altlab;		/* label for alternate expression */

	type = getorcond();
	if (gettoken() != T_QUESTIONMARK) {
		rescantoken();
		return type;
	}
	clearlabel(&donelab);
	clearlabel(&altlab);
	addoplabel(OP_JUMPZ, &altlab);
	type = getaltcond();
	if (gettoken() != T_COLON) {
		scanerror(T_SEMICOLON, "Missing colon for conditional expression");
		return EXPR_RVALUE;
	}
	addoplabel(OP_JUMP, &donelab);
	setlabel(&altlab);
	type |= getaltcond();
	setlabel(&donelab);
	return type;
}


/*
 * Get a possible conditional or expression.
 * Flags are returned indicating the type of expression found.
 * orcond = andcond [ '||' andcond ] ...
 */
static int
getorcond(void)
{
	int type;		/* type of expression */
	LABEL donelab;		/* label for done */

	clearlabel(&donelab);
	type = getandcond();
	while (gettoken() == T_OROR) {
		addoplabel(OP_CONDORJUMP, &donelab);
		type |= getandcond();
	}
	rescantoken();
	if (donelab.l_chain >= 0)
		setlabel(&donelab);
	return type;
}


/*
 * Get a possible conditional and expression.
 * Flags are returned indicating the type of expression found.
 * andcond = relation [ '&&' relation ] ...
 */
static int
getandcond(void)
{
	int type;		/* type of expression */
	LABEL donelab;		/* label for done */

	clearlabel(&donelab);
	type = getrelation();
	while (gettoken() == T_ANDAND) {
		addoplabel(OP_CONDANDJUMP, &donelab);
		type |= getrelation();
	}
	rescantoken();
	if (donelab.l_chain >= 0)
		setlabel(&donelab);
	return type;
}


/*
 * Get a possible relation (equality or inequality), or just an expression.
 * Flags are returned indicating the type of relation found.
 * relation = sum '==' sum
 *	| sum '!=' sum
 *	| sum '<=' sum
 *	| sum '>=' sum
 *	| sum '<' sum
 *	| sum '>' sum
 *	| sum.
 */
static int
getrelation(void)
{
	int type;		/* type of expression */
	long op;		/* opcode to generate */

	type = getsum();
	switch (gettoken()) {
		case T_EQ: op = OP_EQ; break;
		case T_NE: op = OP_NE; break;
		case T_LT: op = OP_LT; break;
		case T_GT: op = OP_GT; break;
		case T_LE: op = OP_LE; break;
		case T_GE: op = OP_GE; break;
		default:
			rescantoken();
			return type;
	}
	(void) getsum();
	addop(op);
	return EXPR_RVALUE;
}


/*
 * Get an expression made up of sums of products.
 * Flags indicating the type of expression found are returned.
 * sum = product [ {'+' | '-'} product ] ...
 */
static int
getsum(void)
{
	int type;		/* type of expression found */
	long op;		/* opcode to generate */

	type = EXPR_RVALUE;
	switch(gettoken()) {
		case T_PLUS:
			(void) getproduct();
			addop(OP_PLUS);
			break;
		case T_MINUS:
			(void) getproduct();
			addop(OP_NEGATE);
			break;
		default:
			rescantoken();
			type = getproduct();
	}
	for (;;) {
		switch (gettoken()) {
			case T_PLUS:	op = OP_ADD; break;
			case T_MINUS:	op = OP_SUB; break;
			default:
				rescantoken();
				return type;
		}
		(void) getproduct();
		addop(op);
		type = EXPR_RVALUE;
	}
}


/*
 * Get the product of arithmetic or expressions.
 * Flags indicating the type of expression found are returned.
 * product = orexpr [ {'*' | '/' | '//' | '%'} orexpr ] ...
 */
static int
getproduct(void)
{
	int type;		/* type of value found */
	long op;		/* opcode to generate */

	type = getorexpr();
	for (;;) {
		switch (gettoken()) {
			case T_MULT:	op = OP_MUL; break;
			case T_DIV:	op = OP_DIV; break;
			case T_MOD:	op = OP_MOD; break;
			case T_SLASHSLASH: op = OP_QUO; break;
			default:
				rescantoken();
				return type;
		}
		(void) getorexpr();
		addop(op);
		type = EXPR_RVALUE;
	}
}


/*
 * Get an expression made up of arithmetic or operators.
 * Flags indicating the type of expression found are returned.
 * orexpr = andexpr [ '|' andexpr ] ...
 */
static int
getorexpr(void)
{
	int type;		/* type of value found */

	type = getandexpr();
	while (gettoken() == T_OR) {
		(void) getandexpr();
		addop(OP_OR);
		type = EXPR_RVALUE;
	}
	rescantoken();
	return type;
}


/*
 * Get an expression made up of arithmetic and operators.
 * Flags indicating the type of expression found are returned.
 * andexpr = shiftexpr [ '&' shiftexpr ] ...
 */
static int
getandexpr(void)
{
	int type;		/* type of value found */
	long op;

	type = getshiftexpr();
	for (;;) {
		switch (gettoken()) {
			case T_AND: op = OP_AND; break;
			case T_HASH: op = OP_HASHOP; break;
			case T_TILDE: op = OP_XOR; break;
			case T_BACKSLASH: op = OP_SETMINUS; break;
			default:
				rescantoken();
				return type;
		}
		(void) getshiftexpr();
		addop(op);
		type = EXPR_RVALUE;
	}
}


/*
 * Get a shift or power expression.
 * Flags indicating the type of expression found are returned.
 * shift = '+' shift
 *	 | '-' shift
 *	 | '/' shift
 *	 | '\' shift
 *	 | '~' shift
 *	 | '#' shift
 *	 | reference '^' shiftexpr
 *	 | reference '<<' shiftexpr
 *	 | reference '>>' shiftexpr
 *	 | reference.
 */
static int
getshiftexpr(void)
{
	int type;		/* type of value found */
	long op;		/* opcode to generate */

	op = 0;
	switch (gettoken()) {
		case T_PLUS:		op = OP_PLUS; break;
		case T_MINUS:		op = OP_NEGATE; break;
		case T_NOT:		op = OP_NOT; break;
		case T_DIV:		op = OP_INVERT; break;
		case T_BACKSLASH:	op = OP_BACKSLASH; break;
		case T_TILDE: 		op = OP_COMP; break;
		case T_HASH:		op = OP_CONTENT; break;
	}
	if (op) {
		(void) getshiftexpr();
		addop(op);
		return EXPR_RVALUE;
	}
	rescantoken();
	type = getreference();
	switch (gettoken()) {
		case T_POWER:		op = OP_POWER; break;
		case T_LEFTSHIFT:	op = OP_LEFTSHIFT; break;
		case T_RIGHTSHIFT: 	op = OP_RIGHTSHIFT; break;
		default:
			rescantoken();
			return type;
	}
	(void) getshiftexpr();
	addop(op);
	return EXPR_RVALUE;
}


/*
 * set an address or dereference indicator
 * address = '&' term
 * dereference = '*' term
 */
static int
getreference(void)
{
	int type;

	switch(gettoken()) {
		case T_ANDAND:
			scanerror(T_NULL, "Non-variable operand for &");
		case T_AND:
			type = getreference();
			addop(OP_PTR);
			type = EXPR_RVALUE;
			break;
		case T_MULT:
			(void) getreference();
			addop(OP_DEREF);
			type = 0;
			break;
		case T_POWER:			/* '**' or '^' */
			(void) getreference();
			addop(OP_DEREF);
			addop(OP_DEREF);
			type = 0;
			break;
		default:
			rescantoken();
			type = getincdecexpr();
	}
	return type;
}


/*
 * get an increment or decrement expression
 * ++expr, --expr, expr++, expr--
 */
static int
getincdecexpr(void)
{
	int type;
	int tok;

	type = getterm();
	tok = gettoken();
	if (tok == T_PLUSPLUS || tok == T_MINUSMINUS) {
		if (isrvalue(type))
				scanerror(T_NULL, "Bad ++ usage");
		writeindexop();
		if (tok == T_PLUSPLUS)
			addop(OP_POSTINC);
		else
			addop(OP_POSTDEC);
		for (;;) {
			tok = gettoken();
			switch(tok) {
				case T_PLUSPLUS:
					addop(OP_PREINC);
					continue;
				case T_MINUSMINUS:
					addop(OP_PREDEC);
					continue;
				default:
					addop(OP_POP);
					break;
			}
			break;
		}
		type = EXPR_RVALUE | EXPR_ASSIGN;
	}
	if (tok == T_NOT) {
		addopfunction(OP_CALL, getbuiltinfunc("fact"), 1);
		tok = gettoken();
		type = EXPR_RVALUE;
	}
	rescantoken();
	return type;
}


/*
 * Get a single term.
 * Flags indicating the type of value found are returned.
 * term = lvalue
 *	| lvalue '[' assignment ']'
 *	| lvalue '++'
 *	| lvalue '--'
 *	| real_number
 *	| imaginary_number
 *	| '.'
 *	| string
 *	| '(' assignment ')'
 *	| function [ '(' [assignment  [',' assignment] ] ')' ]
 *	| '!' term
 */
static int
getterm(void)
{
	int type;		/* type of term found */
	int oldmode;

	type = 0;
	switch (gettoken()) {
		case T_NUMBER:
			addopone(OP_NUMBER, tokennumber());
			type = (EXPR_RVALUE | EXPR_CONST);
			break;

		case T_IMAGINARY:
			addopone(OP_IMAGINARY, tokennumber());
			type = (EXPR_RVALUE | EXPR_CONST);
			break;

		case T_OLDVALUE:
			addop(OP_OLDVALUE);
			type = 0;
			break;

		case T_STRING:
			addopone(OP_STRING, tokenstring());
			type = EXPR_RVALUE;
			break;

		case T_PLUSPLUS:
			if (isrvalue(getterm()))
				scanerror(T_NULL, "Bad ++ usage");
			writeindexop();
			addop(OP_PREINC);
			type = EXPR_ASSIGN;
			break;

		case T_MINUSMINUS:
			if (isrvalue(getterm()))
				scanerror(T_NULL, "Bad -- usage");
			writeindexop();
			addop(OP_PREDEC);
			type = EXPR_ASSIGN;
			break;

		case T_LEFTPAREN:
			oldmode = tokenmode(TM_DEFAULT);
			type = getexprlist();
			if (gettoken() != T_RIGHTPAREN)
				scanerror(T_SEMICOLON, "Missing right parenthesis");
			(void) tokenmode(oldmode);
			break;

		case T_MAT:
			getonematrix(SYM_UNDEFINED);
			while (gettoken() == T_COMMA) {
				addop(OP_POP);
				getonematrix(SYM_UNDEFINED);
			}
			rescantoken();
			type = EXPR_ASSIGN;
			break;

		case T_OBJ:
			getobjdeclaration(SYM_UNDEFINED);
			type = EXPR_ASSIGN;
			break;

		case T_SYMBOL:
			rescantoken();
			type = getidexpr(TRUE, FALSE);
			break;

		case T_LEFTBRACKET:
			scanerror(T_NULL, "Bad index usage");
			break;

		case T_PERIOD:
			scanerror(T_NULL, "Bad element reference");
			break;

		default:
			if (iskeyword(type)) {
				scanerror(T_NULL, "Expression contains reserved keyword");
				break;
			}
			rescantoken();
			scanerror(T_COMMA, "Missing expression");
	}
	if (type == 0) {
		for (;;) {
			switch (gettoken()) {
				case T_LEFTBRACKET:
					rescantoken();
					getmatargs();
					type = 0;
					break;
				case T_PERIOD:
					getelement();
					type = 0;
					break;
				case T_LEFTPAREN:
					scanerror(T_NULL, "Function calls not allowed as expressions");
				default:
					rescantoken();
				return type;
			}
		}
	}
	return type;
}


/*
 * Read in an identifier expressions.
 * This is a symbol name followed by parenthesis, or by square brackets or
 * element refernces.  The symbol can be a global or a local variable name.
 * Returns the type of expression found.
 */
static int
getidexpr(BOOL okmat, BOOL autodef)
{
	int type;
	char name[SYMBOLSIZE+1];	/* symbol name */
	int oldmode;

	type = 0;
	if (!getid(name))
		return type;
	switch (gettoken()) {
		case T_LEFTPAREN:
			oldmode = tokenmode(TM_DEFAULT);
			getcallargs(name);
			(void) tokenmode(oldmode);
			type = 0;
			break;
		case T_ASSIGN:
			autodef = TRUE;
			/* fall into default case */
		default:
			rescantoken();
			usesymbol(name, autodef);
	}
	/*
	 * Now collect as many element references and matrix index operations
	 * as there are following the id.
	 */
	for (;;) {
		switch (gettoken()) {
			case T_LEFTBRACKET:
				rescantoken();
				if (!okmat)
					return type;
				getmatargs();
				type = 0;
				break;
			case T_ARROW:
				addop(OP_DEREF);
				/*FALLTHRU*/
			case T_PERIOD:
				getelement();
				type = 0;
				break;
			case T_LEFTPAREN:
				scanerror(T_NULL, "Function calls not allowed as expressions");
			default:
				rescantoken();
				return type;
		}
	}
}


/*
 * Read in a filename for a read or write command.
 * Both quoted and unquoted filenames are handled here.
 * The name must be terminated by an end of line or semicolon.
 * Returns TRUE if the filename was successfully parsed.
 *
 * given:
 *	name		filename to read
 *	msg_ok		TRUE => ok to print error messages
 *	once		non-NULL => set to TRUE of -once read
 */
static BOOL
getfilename(char *name, BOOL msg_ok, BOOL *once)
{
	STRING *s;

	/* look at the next token */
	(void) tokenmode(TM_NEWLINES | TM_ALLSYMS);
	switch (gettoken()) {
		case T_STRING:
			s = findstring(tokenstring());
			strcpy(name, s->s_str);
			sfree(s);
			break;
		case T_SYMBOL:
			strcpy(name, tokensymbol());
			break;
		default:
			if (msg_ok)
				scanerror(T_SEMICOLON, "Filename expected");
			return FALSE;
	}

	/* determine if we care about a possible -once option */
	if (once != NULL) {
		/* we care about a possible -once option */
		if (strcmp(name, "-once") == 0) {
			/* -once option found */
			*once = TRUE;
			/* look for the filename */
			switch (gettoken()) {
				case T_STRING:
					s = findstring(tokenstring());
					strcpy(name, s->s_str);
					sfree(s);
					break;
				case T_SYMBOL:
					strcpy(name, tokensymbol());
					break;
				default:
					if (msg_ok)
						scanerror(T_SEMICOLON,
						    "Filename expected");
					return FALSE;
			}
		} else {
			*once = FALSE;
		}
	}

	/* look at the next token */
	switch (gettoken()) {
		case T_SEMICOLON:
		case T_NEWLINE:
		case T_EOF:
			break;
		default:
			if (msg_ok)
				scanerror(T_SEMICOLON,
				    "Missing semicolon after filename");
			return FALSE;
	}
	return TRUE;
}


/*
 * Read the show command to display useful information
 */
static void
getshowstatement(void)
{
	char name[5];
	long arg, index;

	switch (gettoken()) {
		case T_SYMBOL:
			strncpy(name, tokensymbol(), 4);
			name[4] = '\0';
			arg = stringindex("buil\000real\000func\000objf\000conf\000objt\000file\000size\000erro\000cust\000bloc\000cons\000glob\000stat\000numb\000redc\000stri\000lite\000opco\000", name);
			if (arg == 19) {
				if (gettoken() != T_SYMBOL) {
					rescantoken();
					scanerror(T_SEMICOLON, "Function name expected");
					return;
				}
				index = adduserfunc(tokensymbol());
				addopone(OP_SHOW, index + 19);
				return;
			}
			if (arg > 0)
				addopone(OP_SHOW, arg);
			else
				printf("Unknown SHOW parameter ignored\n");
			return;
		default:
			printf("SHOW command to be followed by at least ");
			printf("four letters of one of:\n");
			printf("\tblocks, builtin, config, constants, ");
			printf("custom, errors, files, functions,\n");
			printf("\tglobaltypes, objfunctions, objtypes, opcodes, sizes, ");
			printf("realglobals,\n");
			printf("\tstatics, numbers, redcdata, strings, literals\n");
			rescantoken();
			return;

	}
}


/*
 * Read in a set of matrix index arguments, surrounded with square brackets.
 * This also handles double square brackets for 'fast indexing'.
 */
static void
getmatargs(void)
{
	int dim;

	if (gettoken() != T_LEFTBRACKET) {
		scanerror(T_NULL, "Matrix indexing expected");
		return;
	}
	/*
	 * Parse all levels of the array reference
	 * Look for the 'fast index' first.
	 */
	if (gettoken() == T_LEFTBRACKET) {
		(void) getopassignment();
		if ((gettoken() != T_RIGHTBRACKET) ||
			(gettoken() != T_RIGHTBRACKET)) {
				scanerror(T_NULL, "Bad fast index usage");
				return;
		}
		addop(OP_FIADDR);
		return;
	}
	rescantoken();
	/*
	 * Normal indexing with the indexes separated by commas.
	 * Initialize the flag in the opcode to assume that the array
	 * element will only be referenced for reading.  If the parser
	 * finds that the element will be referenced for writing, then
	 * it will call writeindexop to change the flag in the opcode.
	 */
	dim = 1;
	for (;;) {
		(void) getopassignment();
		switch (gettoken()) {
			case T_RIGHTBRACKET:
				addoptwo(OP_INDEXADDR, (long) dim,
						(long) FALSE);
				return;
			case T_COMMA:
				dim++;
				break;
			default:
				rescantoken();
				scanerror(T_NULL, "Missing right bracket in array reference");
				return;
		}
	}
}


/*
 * Get an element of an object reference.
 * The leading period which introduces the element has already been read.
 */
static void
getelement(void)
{
	long index;
	char name[SYMBOLSIZE+1];

	if (!getid(name))
		return;
	index = findelement(name);
	if (index < 0) {
		scanerror(T_NULL, "Element \"%s\" is undefined", name);
		return;
	}
	addopone(OP_ELEMADDR, index);
}


/*
 * Read in a single symbol name and copy its value into the given buffer.
 * Returns TRUE if a valid symbol id was found.
 */
static BOOL
getid(char *buf)
{
	int type;

	type = gettoken();
	if (iskeyword(type)) {
		scanerror(T_NULL, "Reserved keyword used as symbol name");
		type = T_SYMBOL;
		*buf = '\0';
		return FALSE;
	}
	if (type != T_SYMBOL) {
		rescantoken();
		scanerror(T_NULL, "Symbol name expected");
		*buf = '\0';
		return FALSE;
	}
	strncpy(buf, tokensymbol(), SYMBOLSIZE);
	buf[SYMBOLSIZE] = '\0';
	return TRUE;
}


/*
 * Define a symbol name to be of the specified symbol type.  The scope
 * of a static variable with the same name is terminated if symtype is
 * global or if symtype is static and the old variable is at the same
 * level.  A scan error occurs if the name is already in use in an
 * incompatible manner.
 */
static void
definesymbol(char *name, int symtype)
{
	switch (symboltype(name)) {
		case SYM_STATIC:
			if (symtype == SYM_GLOBAL || symtype == SYM_STATIC)
				endscope(name, symtype == SYM_GLOBAL);
			/*FALLTHRU*/
		case SYM_UNDEFINED:
		case SYM_GLOBAL:
			if (symtype == SYM_LOCAL)
				(void) addlocal(name);
			else
				(void) addglobal(name, (symtype == SYM_STATIC));
			break;

		case SYM_LOCAL:
			if (symtype == SYM_LOCAL)
				return;
			/*FALLTHRU*/
		case SYM_PARAM:
			scanerror(T_COMMA, "Variable \"%s\" is already defined", name);
			return;
	}

}


/*
 * Check a symbol name to see if it is known and generate code to reference it.
 * The symbol can be either a parameter name, a local name, or a global name.
 * If autodef is true, we automatically define the name as a global symbol
 * if it is not yet known.
 *
 * given:
 *	name		symbol name to be checked
 *	autodef		TRUE => define is symbol is not known
 */
static void
usesymbol(char *name, BOOL autodef)
{
	switch (symboltype(name)) {
		case SYM_LOCAL:
			addopone(OP_LOCALADDR, (long) findlocal(name));
			return;
		case SYM_PARAM:
			addopone(OP_PARAMADDR, (long) findparam(name));
			return;
		case SYM_GLOBAL:
		case SYM_STATIC:
			addopptr(OP_GLOBALADDR, (char *) findglobal(name));
			return;
	}
	/*
	 * The symbol is not yet defined.
	 * If we are at the top level and we are allowed to, then define it.
	 */
	if ((curfunc->f_name[0] != '*') || !autodef) {
		scanerror(T_NULL, "\"%s\" is undefined", name);
		return;
	}
	(void) addglobal(name, FALSE);
	addopptr(OP_GLOBALADDR, (char *) findglobal(name));
}


/*
 * Get arguments for a function call.
 * The name and beginning parenthesis has already been seen.
 * callargs = [ [ '&' ] assignment  [',' [ '&' ] assignment] ] ')'.
 *
 * given:
 *	name		name of function
 */
static void
getcallargs(char *name)
{
	long index;		/* function index */
	long op;		/* opcode to add */
	int argcount;		/* number of arguments */
	BOOL addrflag;

	op = OP_CALL;
	index = getbuiltinfunc(name);
	if (index < 0) {
		op = OP_USERCALL;
		index = adduserfunc(name);
	}
	if (gettoken() == T_RIGHTPAREN) {
		if (op == OP_CALL)
			builtincheck(index, 0);
		addopfunction(op, index, 0);
		return;
	}
	rescantoken();
	argcount = 0;
	for (;;) {
		argcount++;
		if (gettoken() == T_RIGHTPAREN) {
			addop(OP_UNDEF);
			if (op == OP_CALL)
				builtincheck(index, argcount);
			addopfunction(op, index, argcount);
			return;
		}
		rescantoken();
		if (gettoken() == T_COMMA) {
			addop(OP_UNDEF);
			continue;
		}
		rescantoken();
		addrflag = (gettoken() == T_BACKQUOTE);
		if (!addrflag)
			rescantoken();
		(void) getopassignment();
		if (addrflag) {
			writeindexop();
		}
		if (!addrflag && (op != OP_CALL))
			addop(OP_GETVALUE);
		if (!strcmp(name, "quomod") && argcount > 2)
			writeindexop();
		switch (gettoken()) {
			case T_RIGHTPAREN:
				if (op == OP_CALL)
					builtincheck(index, argcount);
				addopfunction(op, index, argcount);
				return;
			case T_COMMA:
				break;
			default:
				scanerror(T_SEMICOLON, "Missing right parenthesis in function call");
				return;
		}
	}
}


/*
 * Change the current directory.  If no directory is given, assume home.
 */
static void
do_changedir(void)
{
	char *p;

	/* look at the next token */
	(void) tokenmode(TM_NEWLINES | TM_ALLSYMS);

	/* determine the new directory */
	switch (gettoken()) {
	case T_NULL:
	case T_NEWLINE:
	case T_SEMICOLON:
		p = home;
		break;
	default:
		p = tokensymbol(); /* This is not enough XXX */
		if (p == NULL) {
			p = home;
		}
		break;
	}
	if (p == NULL) {
		fprintf(stderr, "Cannot determine HOME directory\n");
	}

	/* change to that directory */
	if (chdir(p)) {
		perror(p);
	}
	return;
}


/* END CODE */
