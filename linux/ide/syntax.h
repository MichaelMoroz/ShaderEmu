/* syntax.h: what each character of a line of C, JavaScript, Python or HLSL is, for its colour (docs/apps/ide.md).
 * A lexer, not a compiler: it knows words, numbers, strings, comments and lines for the
 * preprocessor, and the one thing a line needs from the lines before it, the state it begins in:
 * inside a comment, a JavaScript template or a Python string of three quotes. */
#ifndef IDE_SYNTAX_H
#define IDE_SYNTAX_H
#include <ctype.h>
#include <string.h>

enum { SX_PLAIN, SX_KEYWORD, SX_TYPE, SX_DIRECTIVE, SX_STRING, SX_NUMBER, SX_COMMENT, SX_CALL, SX_KINDS };
enum { SX_C, SX_JS, SX_PY, SX_HLSL, SX_LANGS };
/* what a line begins inside: nothing, a comment, a template (`...`), a string of ''' or of """ */
enum { SX_IN_NONE, SX_IN_COMMENT, SX_IN_TEMPLATE, SX_IN_SINGLES, SX_IN_DOUBLES };

static int sx_lang;	/* the language of the text being lexed */

static const char *const sx_c_keywords[] = { "if", "else", "for", "while", "do", "return", "break", "continue", "switch", "case",
	"default", "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "extern", "const", "volatile", "inline",
	"register", "restrict", NULL };
static const char *const sx_c_types[] = { "int", "char", "float", "double", "void", "long", "short", "unsigned", "signed",
	"uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "size_t", "ssize_t", "uintptr_t",
	"bool", "FILE", "NULL", "GLfloat", "GLint", "GLuint", "GLubyte", "GLfixed", "GR_EVENT", "GR_WINDOW_ID", "GR_GC_ID", NULL };
static const char *const sx_js_keywords[] = { "break", "case", "catch", "class", "const", "continue", "debugger", "default",
	"delete", "do", "else", "export", "extends", "finally", "for", "function", "if", "import", "in", "instanceof", "let", "new",
	"of", "return", "super", "switch", "this", "throw", "try", "typeof", "var", "void", "while", "with", "yield", "async",
	"await", "static", "get", "set", "from", "as", NULL };
static const char *const sx_js_types[] = { "true", "false", "null", "undefined", "NaN", "Infinity", "Math", "JSON", "Object",
	"Array", "String", "Number", "Boolean", "BigInt", "Symbol", "Map", "Set", "Promise", "Date", "RegExp", "Error", "console",
	"print", "std", "os", "scriptArgs", "globalThis", "arguments", NULL };
static const char *const sx_py_keywords[] = { "False", "None", "True", "and", "as", "assert", "async", "await", "break", "class",
	"continue", "def", "del", "elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is",
	"lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try", "while", "with", "yield", NULL };
static const char *const sx_py_types[] = { "int", "float", "str", "bool", "list", "dict", "tuple", "set", "bytes", "bytearray",
	"object", "range", "len", "print", "type", "isinstance", "enumerate", "zip", "map", "filter", "sorted", "reversed", "min",
	"max", "sum", "abs", "open", "super", "self", "repr", "round", "input", "iter", "next", "any", "all", "hex", "ord", "chr",
	"divmod", "pow", "Exception", "ValueError", "TypeError", "KeyError", "IndexError", "StopIteration", NULL };
static const char *const sx_hlsl_keywords[] = { "if", "else", "for", "while", "do", "return", "break", "continue", "switch",
	"case", "default", "discard", "struct", "static", "const", "uniform", "in", "out", "inout", "cbuffer", "register",
	"packoffset", "typedef", "inline", "true", "false", NULL };
static const char *const sx_hlsl_types[] = { "void", "bool", "bool2", "bool3", "bool4", "int", "int2", "int3", "int4", "uint",
	"uint2", "uint3", "uint4", "float", "float2", "float3", "float4", "float2x2", "float3x3", "float4x4", "float2x3", "float3x2",
	"float3x4", "float4x3", "half", "half2", "half3", "half4", "matrix", "vector", "SV_Position", "SV_Target", NULL };
static const char *const *const sx_lists[SX_LANGS][2] = {
	{ sx_c_keywords, sx_c_types }, { sx_js_keywords, sx_js_types }, { sx_py_keywords, sx_py_types }, { sx_hlsl_keywords, sx_hlsl_types },
};

/* What a character may be part of: a name's letters, digits, the underscore (and in
 * JavaScript the dollar sign). */
enum { SX_ALPHA = 1, SX_DIGIT = 2, SX_UNDER = 4 };
static const unsigned char sx_class[256] = {
	['A' ... 'Z'] = SX_ALPHA, ['a' ... 'z'] = SX_ALPHA, ['0' ... '9'] = SX_DIGIT, ['_'] = SX_UNDER, ['$'] = SX_UNDER,
};

/* Each language's two lists by their words' first letters, made at its first word looked up. */
static struct { const char *word; unsigned char length, kind; } sx_words[SX_LANGS][100];	/* (Python has 84) */
static unsigned char sx_from[SX_LANGS][129];

static void
sx_index(int lang)
{
	const char *const *list;
	int c, n = 0, pass;

	for (c = 0; c < 128; c++) {
		sx_from[lang][c] = (unsigned char)n;
		for (pass = 0; pass < 2; pass++)
			for (list = sx_lists[lang][pass]; *list; list++)
				if ((*list)[0] == c && n < (int)(sizeof sx_words[0] / sizeof sx_words[0][0])) {
					sx_words[lang][n].word = *list;
					sx_words[lang][n].length = (unsigned char)strlen(*list);
					sx_words[lang][n++].kind = pass ? SX_TYPE : SX_KEYWORD;
				}
	}
	sx_from[lang][128] = (unsigned char)n;
}

/* A word's kind: a keyword, a type, or plain. */
static int
sx_word(const char *word, int length)
{
	int c = (unsigned char)word[0], i, k, lang = sx_lang;

	if (!sx_from[lang][128])
		sx_index(lang);
	if (c >= 128)
		return SX_PLAIN;
	for (i = sx_from[lang][c]; i < sx_from[lang][c + 1]; i++) {
		const char *is = sx_words[lang][i].word;

		if (sx_words[lang][i].length != length)
			continue;
		for (k = 1; k < length && is[k] == word[k]; k++)
			continue;
		if (k == length)
			return sx_words[lang][i].kind;
	}
	return SX_PLAIN;
}

/* A string's characters from i on as SX_STRING, up to its closing `end` (three of them when
 * `triple`); the index after it comes back, and *open says whether the line ended inside it. */
static int
sx_string(const char *text, int i, int n, int end, int triple, unsigned char *kinds, int *open)
{
	for (*open = 1; i < n; i++) {
		kinds[i] = SX_STRING;
		if (text[i] == '\\' && i + 1 < n) {
			kinds[++i] = SX_STRING;
		} else if (text[i] == end && (!triple || (text[i + 1] == end && text[i + 2] == end))) {
			if (triple)
				kinds[i + 1] = kinds[i + 2] = SX_STRING, i += 2;
			*open = 0;
			return i + 1;
		}
	}
	return n;
}

/* Fills `kinds` for a line; `state` says what it begins inside (SX_IN_...), and what comes
 * back is the state the next line begins in. */
static int
sx_line(const char *text, int state, unsigned char *kinds)
{
	int i = 0, n = (int)strlen(text), j, kind, directive = 0, open, py = sx_lang == SX_PY;

	while (i < n) {
		int ch = (unsigned char)text[i], is = sx_class[ch];

		if (state == SX_IN_COMMENT) {
			for (; i < n && !(text[i] == '*' && text[i + 1] == '/'); i++)
				kinds[i] = SX_COMMENT;
			state = i >= n ? SX_IN_COMMENT : SX_IN_NONE;
			if (!state) {
				kinds[i] = kinds[i + 1] = SX_COMMENT;
				i += 2;
			}
		} else if (state) {	/* the rest of a template or of a string of three quotes */
			i = sx_string(text, i, n, state == SX_IN_TEMPLATE ? '`' : state == SX_IN_SINGLES ? '\'' : '"', state != SX_IN_TEMPLATE, kinds, &open);
			state = open ? state : SX_IN_NONE;
		} else if (py && (ch == '\'' || ch == '"' || ((ch | 32) == 'r' || (ch | 32) == 'b' || (ch | 32) == 'f' || (ch | 32) == 'u')) &&
			   (ch == '\'' || ch == '"' || text[i + 1] == '\'' || text[i + 1] == '"' ||
			    (sx_class[(unsigned char)text[i + 1]] & SX_ALPHA && (text[i + 2] == '\'' || text[i + 2] == '"')))) {
			/* a Python string, after its prefix (r, b, f, u, rb...) */
			for (j = i; text[j] != '\'' && text[j] != '"'; j++)
				kinds[j] = SX_STRING;
			ch = (unsigned char)text[j];
			if (text[j + 1] == ch && text[j + 2] == ch) {
				kinds[j] = kinds[j + 1] = kinds[j + 2] = SX_STRING;
				i = sx_string(text, j + 3, n, ch, 1, kinds, &open);
				state = open ? (ch == '\'' ? SX_IN_SINGLES : SX_IN_DOUBLES) : SX_IN_NONE;
			} else {
				kinds[j] = SX_STRING;
				i = sx_string(text, j + 1, n, ch, 0, kinds, &open);
			}
		} else if (is & (SX_ALPHA | SX_UNDER)) {
			for (j = i + 1; sx_class[(unsigned char)text[j]]; j++)
				continue;
			kind = sx_word(text + i, j - i);
			if (kind == SX_PLAIN) {
				int after = j;

				while (text[after] == ' ')
					after++;
				if (text[after] == '(')
					kind = SX_CALL;
			}
			while (i < j)
				kinds[i++] = (unsigned char)kind;
		} else if (ch == ' ') {
			kinds[i++] = SX_PLAIN;
		} else if (!py && ch == '/' && text[i + 1] == '*') {
			kinds[i] = kinds[i + 1] = SX_COMMENT;
			i += 2;
			state = SX_IN_COMMENT;
		} else if ((!py && ch == '/' && text[i + 1] == '/') || (py && ch == '#')) {
			while (i < n)
				kinds[i++] = SX_COMMENT;
		} else if (sx_lang == SX_JS && ch == '`') {
			kinds[i] = SX_STRING;
			i = sx_string(text, i + 1, n, '`', 0, kinds, &open);
			state = open ? SX_IN_TEMPLATE : SX_IN_NONE;
		} else if (ch == '"' || ch == '\'' || (directive == 2 && ch == '<')) {
			int end = ch == '<' ? '>' : ch;

			for (j = i + 1; j < n && text[j] != end; j++)
				if (text[j] == '\\' && j + 1 < n)
					j++;
			j = j < n ? j + 1 : n;
			while (i < j)
				kinds[i++] = SX_STRING;
		} else if ((ch == '#' && !directive && (sx_lang == SX_C || sx_lang == SX_HLSL)) || (py && ch == '@' && !directive)) {
			/* the directive's own word (a decorator's name); what follows is code again (but for #include's name) */
			for (j = i + 1; j < n && text[j] == ' '; j++)
				continue;
			directive = !py && !strncmp(text + j, "include", 7) ? 2 : 1;
			while ((sx_class[(unsigned char)text[j]] & SX_ALPHA) || (py && (sx_class[(unsigned char)text[j]] || text[j] == '.')))
				j++;
			while (i < j)
				kinds[i++] = SX_DIRECTIVE;
		} else if ((is & SX_DIGIT) || (ch == '.' && (sx_class[(unsigned char)text[i + 1]] & SX_DIGIT))) {
			for (; (sx_class[(unsigned char)text[i]] & (SX_ALPHA | SX_DIGIT | (sx_lang == SX_JS || sx_lang == SX_PY ? SX_UNDER : 0))) || text[i] == '.'; i++)
				kinds[i] = SX_NUMBER;
		} else {
			directive = directive ? directive : -1;	/* a # further on is not a directive */
			kinds[i++] = SX_PLAIN;
		}
	}
	return state;
}
#endif
