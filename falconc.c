/*
 * falconc — Falcon language compiler  (v4)
 *
 * New in v4:
 *   -arch x86-32-linux  — original 32-bit target (default, unchanged)
 *   -arch x86-64-linux  — new 64-bit target (System V AMD64 ABI)
 *                         • syscall instead of int 0x80
 *                         • args in rdi,rsi,rdx,rcx,r8,r9
 *                         • long is a native 64-bit register (rax)
 *                         • all stack slots 8 bytes
 *                         • SSE2 for float/double (xmm0)
 *
 * New in v3:
 *   float         — 32-bit IEEE-754 via x87 FPU (flds/fstps/fadd etc.)
 *   double        — 64-bit IEEE-754 via x87 FPU (fldl/fstpl/faddp etc.)
 *   let           — mutable variable declaration (sugar: let x: int = 5)
 *   const         — immutable compile-time constant (folded, no stack slot)
 *                   const PI: float = 3.14   const MAX: int = 100
 *
 * New in v2:
 *   long          — 64-bit signed integer
 *   Type checker  — every expression has a computed type; mismatches are
 *                   errors at compile time, not silent wrong-code at runtime
 *   str_concat(a,b) -> str   — heap-allocate and concatenate two strings
 *   str_format(fmt, ...) -> str  — %-style formatting (%d %s %%) up to 8 args
 *   Dynamic heap  — _flr_alloc uses brk/sbrk (32-bit) or mmap (64-bit)
 *
 * Unchanged from v1:
 *   Lexer, parser structure, all operators, hardware intrinsics,
 *   freestanding mode, import system, arrays, structs, bool, break/continue
 *
 * Build:
 *   gcc -O2 -o falconc falconc.c
 *
 * Linking (32-bit hosted):
 *   as --32 out.s -o out.o
 *   ld -m elf_i386 flr.o out.o -o prog
 *
 * Linking (64-bit hosted):
 *   as out.s -o out.o
 *   ld out.o -o prog
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

/* ═══════════════════════════════════════════════════════════════════════
   UTILS
   ═══════════════════════════════════════════════════════════════════════ */

static void die(const char *fmt,...){
    va_list ap;va_start(ap,fmt);
    fprintf(stderr,"falconc: ");vfprintf(stderr,fmt,ap);
    fprintf(stderr,"\n");va_end(ap);exit(1);
}
static void warn(const char *fmt,...){
    va_list ap;va_start(ap,fmt);
    fprintf(stderr,"falconc: warning: ");vfprintf(stderr,fmt,ap);
    fprintf(stderr,"\n");va_end(ap);
}

static char *xstrdup(const char *s){return s?strdup(s):strdup("");}
static char *xstrndup(const char *s,int n){
    char *p=malloc(n+1);memcpy(p,s,n);p[n]=0;return p;
}
static char *path_dir(const char *path){
    char *tmp=xstrdup(path);
    char *sl=strrchr(tmp,'/');
    if(!sl){free(tmp);return xstrdup(".");}
    *sl='\0';return tmp;
}
static char *path_join(const char *dir,const char *name){
    size_t n=strlen(dir)+1+strlen(name)+1;
    char *p=malloc(n);snprintf(p,n,"%s/%s",dir,name);return p;
}

/* forward declaration — defined later in CODEGEN section */
static int has_std;

/* ═══════════════════════════════════════════════════════════════════════
   IMPORT DEDUP + SEARCH PATHS
   ═══════════════════════════════════════════════════════════════════════ */

#define MAX_IMPORTED 512
static char *imported_files[MAX_IMPORTED];
static int   nimported=0;
static int already_imported(const char *p){
    for(int i=0;i<nimported;i++)if(strcmp(imported_files[i],p)==0)return 1;
    return 0;
}
static void mark_imported(const char *p){
    if(nimported<MAX_IMPORTED)imported_files[nimported++]=xstrdup(p);
}

#define MAX_SEARCH 64
static char *search_paths[MAX_SEARCH];
static int   nsearch=0;
static void add_search(const char *p){
    if(nsearch<MAX_SEARCH)search_paths[nsearch++]=xstrdup(p);
}

static char *resolve_import(const char *name,const char *from_file){
    const char *exts[]={".fl",".fal",".flc",".flsrc","",NULL};
    char *from_dir=path_dir(from_file);
    for(int e=0;exts[e];e++){
        char nb[512];snprintf(nb,sizeof nb,"%s%s",name,exts[e]);
        char *p=path_join(from_dir,nb);
        FILE *f=fopen(p,"rb");if(f){fclose(f);free(from_dir);return p;}free(p);
    }
    free(from_dir);
    for(int si=0;si<nsearch;si++){
        for(int e=0;exts[e];e++){
            char nb[512];snprintf(nb,sizeof nb,"%s%s",name,exts[e]);
            char *p=path_join(search_paths[si],nb);
            FILE *f=fopen(p,"rb");if(f){fclose(f);return p;}free(p);
        }
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
   LEXER
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum{
    TT_INT_LIT,TT_LONG_LIT,TT_FLOAT_LIT,TT_DOUBLE_LIT,TT_STR_LIT,
    TT_VOID,TT_INT,TT_STR,TT_BOOL,TT_LONG,TT_FLOAT,TT_DOUBLE,
    TT_LET,TT_CONST,
    TT_FUNC,TT_RETURN,
    TT_IF,TT_ELIF,TT_ELSE,
    TT_WHILE,TT_FOR,
    TT_AND,TT_OR,TT_NOT,
    TT_TRUE,TT_FALSE,
    TT_BREAK,TT_CONTINUE,
    TT_STRUCT,TT_IMPORT,TT_TYPEDEF,
    TT_LBRACE,TT_RBRACE,
    TT_LPAREN,TT_RPAREN,
    TT_LBRACKET,TT_RBRACKET,
    TT_COLON,TT_COMMA,TT_ARROW,TT_DOT,
    TT_ASSIGN,
    TT_PLUS_ASSIGN,TT_MINUS_ASSIGN,TT_STAR_ASSIGN,TT_SLASH_ASSIGN,TT_MOD_ASSIGN,
    TT_SHL_ASSIGN,TT_SHR_ASSIGN,
    TT_AND_ASSIGN,TT_OR_ASSIGN,TT_XOR_ASSIGN,
    TT_EQ,TT_NEQ,TT_LT,TT_GT,TT_LTE,TT_GTE,
    TT_PLUS,TT_MINUS,TT_STAR,TT_SLASH,TT_MOD,
    TT_AMPERSAND,TT_PIPE,TT_CARET,TT_TILDE,
    TT_SHL,TT_SHR,
    TT_IDENT,TT_NEWLINE,TT_EOF,
    TT_STATIC,TT_EXTERN
}TT;

typedef struct{TT type;char *val;int line;const char *file;}Token;

#define MAX_TOKS 524288
static Token  gtoks[MAX_TOKS];
static int    gntoks=0;
static int    gpos=0;

typedef struct{const char *w;TT t;}KW;
static const KW kws[]={
    {"void",TT_VOID},{"int",TT_INT},{"str",TT_STR},{"bool",TT_BOOL},{"long",TT_LONG},
    {"float",TT_FLOAT},{"double",TT_DOUBLE},{"let",TT_LET},{"const",TT_CONST},
    {"func",TT_FUNC},{"return",TT_RETURN},
    {"if",TT_IF},{"elif",TT_ELIF},{"else",TT_ELSE},
    {"while",TT_WHILE},{"for",TT_FOR},
    {"and",TT_AND},{"or",TT_OR},{"not",TT_NOT},
    {"true",TT_TRUE},{"false",TT_FALSE},
    {"break",TT_BREAK},{"continue",TT_CONTINUE},
    {"struct",TT_STRUCT},{"import",TT_IMPORT},{"typedef",TT_TYPEDEF},
    {"static",TT_STATIC},{"extern",TT_EXTERN},
    {NULL,0}
};

static void emit_tok(TT t,const char *v,int line,const char *file){
    if(gntoks>=MAX_TOKS)die("too many tokens");
    gtoks[gntoks].type=t;gtoks[gntoks].val=xstrdup(v);
    gtoks[gntoks].line=line;gtoks[gntoks].file=file;
    gntoks++;
}

static void tokenize(const char *src,const char *filename){
    int i=0,n=(int)strlen(src),line=1,last_nl=1;
    const char *file=xstrdup(filename);
    while(i<n){
        unsigned char c=src[i];
        if(c==' '||c=='\t'||c=='\r'){i++;continue;}
        if(c=='#'){while(i<n&&src[i]!='\n')i++;continue;}
        if(c=='/'&&i+1<n&&src[i+1]=='*'){
            int cstart=line;
            i+=2;
            while(i+1<n&&!(src[i]=='*'&&src[i+1]=='/')){if(src[i]=='\n')line++;i++;}
            if(i+1>=n)die("%s:%d: unterminated /* comment",file,cstart);
            i+=2;
            continue;
        }
        if(c=='\n'){
            if(!last_nl)emit_tok(TT_NEWLINE,"\n",line,file);
            last_nl=1;line++;i++;continue;
        }
        last_nl=0;
        /* string */
        if(c=='"'){
            i++;
            char buf[16384];int bi=0;
            while(i<n&&src[i]!='"'){
                if(src[i]=='\\'&&i+1<n){
                    i++;
                    switch(src[i]){
                        case 'n':buf[bi++]='\n';break;case 't':buf[bi++]='\t';break;
                        case 'r':buf[bi++]='\r';break;case '0':buf[bi++]='\0';break;
                        case '"':buf[bi++]='"';break;case '\\':buf[bi++]='\\';break;
                        default:buf[bi++]='\\';buf[bi++]=src[i];break;
                    }
                }else{if(src[i]=='\n')line++;buf[bi++]=src[i];}
                i++;
            }
            if(i>=n)die("%s:%d: unterminated string",file,line);
            i++;buf[bi]=0;
            emit_tok(TT_STR_LIT,buf,line,file);continue;
        }
        /* hex literal */
        if(c=='0'&&i+1<n&&(src[i+1]=='x'||src[i+1]=='X')){
            int j=i+2;while(j<n&&isxdigit((unsigned char)src[j]))j++;
            int is_long=(j<n&&(src[j]=='L'||src[j]=='l'));
            char *s=xstrndup(src+i,j-i);
            emit_tok(is_long?TT_LONG_LIT:TT_INT_LIT,s,line,file);
            free(s);i=j+(is_long?1:0);continue;
        }
        /* binary literal */
        if(c=='0'&&i+1<n&&(src[i+1]=='b'||src[i+1]=='B')){
            int j=i+2;while(j<n&&(src[j]=='0'||src[j]=='1'))j++;
            if(j==i+2)die("%s:%d: invalid binary literal (no digits after 0b)",file,line);
            int is_long=(j<n&&(src[j]=='L'||src[j]=='l'));
            char *s=xstrndup(src+i,j-i);
            emit_tok(is_long?TT_LONG_LIT:TT_INT_LIT,s,line,file);
            free(s);i=j+(is_long?1:0);continue;
        }
        /* decimal / float / double */
        if(isdigit(c)||(c=='.'&&i+1<n&&isdigit((unsigned char)src[i+1]))){
            int j=i;
            while(j<n&&isdigit((unsigned char)src[j]))j++;
            int is_float=0;
            /* a literal is floating-point if it has a '.', or an exponent
               that is actually followed by digits (so `1e6` works but an
               identifier like `1else` is not swallowed) */
            int has_dot=(j<n&&src[j]=='.');
            int has_exp=0;
            if(!has_dot&&j<n&&(src[j]=='e'||src[j]=='E')){
                int k=j+1;
                if(k<n&&(src[k]=='+'||src[k]=='-'))k++;
                if(k<n&&isdigit((unsigned char)src[k]))has_exp=1;
            }
            if(has_dot||has_exp){
                if(has_dot){
                    j++;
                    while(j<n&&isdigit((unsigned char)src[j]))j++;
                }
                /* exponent (only if followed by digits) */
                if(j<n&&(src[j]=='e'||src[j]=='E')){
                    int k=j+1;
                    if(k<n&&(src[k]=='+'||src[k]=='-'))k++;
                    if(k<n&&isdigit((unsigned char)src[k])){
                        j=k;
                        while(j<n&&isdigit((unsigned char)src[j]))j++;
                    }
                }
                /* suffix: f/F = float, d/D or none = double */
                int suffix=0;
                if(j<n&&(src[j]=='f'||src[j]=='F')){is_float=1;suffix=1;}
                else if(j<n&&(src[j]=='d'||src[j]=='D')){suffix=1;}
                /* the suffix must end the token (don't eat the start of an identifier) */
                if(suffix&&j+1<n&&(isalnum((unsigned char)src[j+1])||src[j+1]=='_')){suffix=0;is_float=0;}
                char *s=xstrndup(src+i,j-i);
                emit_tok(is_float?TT_FLOAT_LIT:TT_DOUBLE_LIT,s,line,file);
                free(s);i=j+suffix;continue;
            }
            int is_long=(j<n&&(src[j]=='L'||src[j]=='l'));
            char *s=xstrndup(src+i,j-i);
            emit_tok(is_long?TT_LONG_LIT:TT_INT_LIT,s,line,file);
            free(s);i=j+(is_long?1:0);continue;
        }
        /* ident / keyword */
        if(c=='_'||isalpha(c)){
            int j=i;while(j<n&&(src[j]=='_'||isalnum((unsigned char)src[j])))j++;
            char *w=xstrndup(src+i,j-i);
            TT t2=TT_IDENT;
            for(int k=0;kws[k].w;k++)if(strcmp(w,kws[k].w)==0){t2=kws[k].t;break;}
            emit_tok(t2,w,line,file);free(w);i=j;continue;
        }
        /* 3-char ops */
        if(i+2<n){
            if(src[i]=='<'&&src[i+1]=='<'&&src[i+2]=='='){emit_tok(TT_SHL_ASSIGN,"<<=",line,file);i+=3;continue;}
            if(src[i]=='>'&&src[i+1]=='>'&&src[i+2]=='='){emit_tok(TT_SHR_ASSIGN,">>=",line,file);i+=3;continue;}
        }
        /* 2-char ops */
        if(i+1<n){
            char nc=src[i+1];
            if(c=='-'&&nc=='>'){emit_tok(TT_ARROW,"->",line,file);i+=2;continue;}
            if(c=='>'&&nc=='='){emit_tok(TT_GTE,">=",line,file);i+=2;continue;}
            if(c=='<'&&nc=='='){emit_tok(TT_LTE,"<=",line,file);i+=2;continue;}
            if(c=='!'&&nc=='='){emit_tok(TT_NEQ,"!=",line,file);i+=2;continue;}
            if(c=='='&&nc=='='){emit_tok(TT_EQ,"==",line,file);i+=2;continue;}
            if(c=='+'&&nc=='='){emit_tok(TT_PLUS_ASSIGN,"+=",line,file);i+=2;continue;}
            if(c=='-'&&nc=='='){emit_tok(TT_MINUS_ASSIGN,"-=",line,file);i+=2;continue;}
            if(c=='*'&&nc=='='){emit_tok(TT_STAR_ASSIGN,"*=",line,file);i+=2;continue;}
            if(c=='/'&&nc=='='){emit_tok(TT_SLASH_ASSIGN,"/=",line,file);i+=2;continue;}
            if(c=='%'&&nc=='='){emit_tok(TT_MOD_ASSIGN,"%=",line,file);i+=2;continue;}
            if(c=='&'&&nc=='='){emit_tok(TT_AND_ASSIGN,"&=",line,file);i+=2;continue;}
            if(c=='|'&&nc=='='){emit_tok(TT_OR_ASSIGN,"|=",line,file);i+=2;continue;}
            if(c=='^'&&nc=='='){emit_tok(TT_XOR_ASSIGN,"^=",line,file);i+=2;continue;}
            if(c=='<'&&nc=='<'){emit_tok(TT_SHL,"<<",line,file);i+=2;continue;}
            if(c=='>'&&nc=='>'){emit_tok(TT_SHR,">>",line,file);i+=2;continue;}
        }
        /* single-char */
        switch(c){
            case '{':emit_tok(TT_LBRACE,"{",line,file);break;
            case '}':emit_tok(TT_RBRACE,"}",line,file);break;
            case '(':emit_tok(TT_LPAREN,"(",line,file);break;
            case ')':emit_tok(TT_RPAREN,")",line,file);break;
            case '[':emit_tok(TT_LBRACKET,"[",line,file);break;
            case ']':emit_tok(TT_RBRACKET,"]",line,file);break;
            case ':':emit_tok(TT_COLON,":",line,file);break;
            case ',':emit_tok(TT_COMMA,",",line,file);break;
            case '.':emit_tok(TT_DOT,".",line,file);break;
            case '=':emit_tok(TT_ASSIGN,"=",line,file);break;
            case '+':emit_tok(TT_PLUS,"+",line,file);break;
            case '-':emit_tok(TT_MINUS,"-",line,file);break;
            case '*':emit_tok(TT_STAR,"*",line,file);break;
            case '/':emit_tok(TT_SLASH,"/",line,file);break;
            case '%':emit_tok(TT_MOD,"%",line,file);break;
            case '<':emit_tok(TT_LT,"<",line,file);break;
            case '>':emit_tok(TT_GT,">",line,file);break;
            case '&':emit_tok(TT_AMPERSAND,"&",line,file);break;
            case '|':emit_tok(TT_PIPE,"|",line,file);break;
            case '^':emit_tok(TT_CARET,"^",line,file);break;
            case '~':emit_tok(TT_TILDE,"~",line,file);break;
            default:die("%s:%d: unexpected character '%c' (0x%02x)",file,line,isprint(c)?c:'?',c);
        }
        i++;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   IMPORT EXPANDER
   ═══════════════════════════════════════════════════════════════════════ */

static char *read_file(const char *path){
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    fseek(f,0,SEEK_END);long sz=ftell(f);rewind(f);
    char *buf=malloc(sz+1);
    size_t got=fread(buf,1,sz,f);
    buf[got]=0;fclose(f);return buf;
}

static void expand_imports(int start,const char *from_file);

static void tokenize_file(const char *path){
    if(already_imported(path))return;
    mark_imported(path);
    char *src=read_file(path);
    if(!src)die("cannot open import '%s'",path);
    int tok_start=gntoks;
    tokenize(src,path);free(src);
    expand_imports(tok_start,path);
}

static void expand_imports(int start,const char *from_file){
    for(int i=start;i<gntoks-1;i++){
        if(gtoks[i].type==TT_IMPORT&&gtoks[i+1].type==TT_STR_LIT){
            const char *name=gtoks[i+1].val;
            char *resolved=resolve_import(name,from_file);
            if(!resolved){
                /* No real std.fl was found: use Falcon's virtual std runtime. */
                if(strcmp(name,"std")==0){has_std=1;continue;}
                die("%s:%d: cannot find import '%s'",gtoks[i].file,gtoks[i].line,name);
            }
            if(already_imported(resolved)){
                memmove(&gtoks[i],&gtoks[i+2],(gntoks-(i+2))*sizeof(Token));
                gntoks-=2;free(resolved);i--;continue;
            }
            /*
             * A real file named std.fl is just a normal source import.
             * This lets projects and libraries ship their own std.fl without
             * accidentally enabling Falcon's built-in runtime.
             */
            int splice_start=gntoks;
            tokenize_file(resolved);
            int splice_count=gntoks-splice_start;
            free(resolved);
            if(splice_count>0){
                Token *tmp=malloc(splice_count*sizeof(Token));
                memcpy(tmp,&gtoks[splice_start],splice_count*sizeof(Token));
                int tail=splice_start-(i+2);
                memmove(&gtoks[i+splice_count],&gtoks[i+2],tail*sizeof(Token));
                memcpy(&gtoks[i],tmp,splice_count*sizeof(Token));
                free(tmp);
                gntoks=i+splice_count+tail;
            }else{
                memmove(&gtoks[i],&gtoks[i+2],(gntoks-(i+2))*sizeof(Token));
                gntoks-=2;i--;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   TOKEN STREAM
   ═══════════════════════════════════════════════════════════════════════ */

static Token *peek(void)    {return &gtoks[gpos];}
static Token *advance(void) {Token *t=&gtoks[gpos];if(gpos<gntoks-1)gpos++;return t;}
static int    check(TT t)   {return peek()->type==t;}
static void   skip_nl(void) {while(check(TT_NEWLINE))advance();}

static Token *expect(TT t,const char *what){
    skip_nl();
    if(!check(t))die("%s:%d: expected %s, got '%s'",peek()->file,peek()->line,what,peek()->val);
    return advance();
}
static int match(TT t){skip_nl();if(check(t)){advance();return 1;}return 0;}

/* ═══════════════════════════════════════════════════════════════════════
   TYPE SYSTEM
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum{TY_VOID,TY_INT,TY_LONG,TY_FLOAT,TY_DOUBLE,TY_STR,TY_BOOL,TY_ARRAY,TY_STRUCT,TY_PTR}TypeKind;
typedef struct TypeRef TypeRef;
struct TypeRef{TypeKind kind;char *name;TypeRef *elem;};

static TypeRef *mktype(TypeKind k,const char *name,TypeRef *elem){
    TypeRef *t=calloc(1,sizeof*t);t->kind=k;
    t->name=name?xstrdup(name):NULL;t->elem=elem;return t;
}

static const char *type_name(TypeRef *t){
    if(!t)return "unknown";
    switch(t->kind){
        case TY_VOID:  return "void";
        case TY_INT:   return "int";
        case TY_LONG:  return "long";
        case TY_FLOAT: return "float";
        case TY_DOUBLE:return "double";
        case TY_STR:   return "str";
        case TY_BOOL:  return "bool";
        case TY_ARRAY: return "array";
        case TY_STRUCT:return t->name?t->name:"struct";
        case TY_PTR:   return "ptr";
    }
    return "unknown";
}

/* two types are compatible for assignment / comparison */
static int types_compat(TypeRef *a,TypeRef *b){
    if(!a||!b)return 1;
    if(a->kind==b->kind){
        if(a->kind==TY_STRUCT)
            return a->name&&b->name&&strcmp(a->name,b->name)==0;
        return 1;
    }
    /* int <-> long widening allowed */
    if((a->kind==TY_INT||a->kind==TY_LONG)&&(b->kind==TY_INT||b->kind==TY_LONG))return 1;
    /* float/double interchangeable; int/long can assign to float/double */
    if((a->kind==TY_FLOAT||a->kind==TY_DOUBLE)&&(b->kind==TY_FLOAT||b->kind==TY_DOUBLE))return 1;
    if((a->kind==TY_FLOAT||a->kind==TY_DOUBLE)&&(b->kind==TY_INT||b->kind==TY_LONG))return 1;
    /* bool <-> int allowed */
    if((a->kind==TY_INT||a->kind==TY_BOOL)&&(b->kind==TY_INT||b->kind==TY_BOOL))return 1;
    /* struct vars are heap pointers: allow int/long/ptr on rhs (e.g. _flr_alloc
       return, which is TY_LONG on x86-64 so it isn't truncated — see
       builtin_rettype) */
    if(a->kind==TY_STRUCT&&(b->kind==TY_INT||b->kind==TY_LONG||b->kind==TY_PTR))return 1;
    /* ptr <-> int/long interchangeable */
    if((a->kind==TY_PTR||a->kind==TY_INT||a->kind==TY_LONG)&&(b->kind==TY_PTR||b->kind==TY_INT||b->kind==TY_LONG))return 1;
    /* str <-> int/long/ptr: str is just a char pointer, allow for bare-metal */
    if((a->kind==TY_STR)&&(b->kind==TY_INT||b->kind==TY_LONG||b->kind==TY_PTR))return 1;
    if((b->kind==TY_STR)&&(a->kind==TY_INT||a->kind==TY_LONG||a->kind==TY_PTR))return 1;
    return 0;
}

static int type_is_numeric(TypeRef *t){
    return t&&(t->kind==TY_INT||t->kind==TY_LONG||t->kind==TY_BOOL||t->kind==TY_FLOAT||t->kind==TY_DOUBLE);
}

/* ═══════════════════════════════════════════════════════════════════════
   AST
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum{
    N_PROGRAM,N_FUNC,N_STRUCT,N_IMPORT,
    N_VARDECL,N_LETDECL,N_CONSTDECL,N_ASSIGN,N_RETURN,
    N_IF,N_ELIF,N_WHILE,N_FOR,N_BREAK,N_CONTINUE,N_EXPRSTMT,
    N_INTLIT,N_LONGLIT,N_FLOATLIT,N_DOUBLELIT,N_STRLIT,N_BOOLLIT,N_IDENT,N_BINOP,N_UNOP,
    N_CALL,N_INDEX,N_FIELD,N_ARRAYLIT
}NK;

typedef struct Node Node;
typedef struct{Node **d;int n,cap;}NList;
static void nl_push(NList *l,Node *n){
    if(l->n>=l->cap){l->cap=l->cap?l->cap*2:8;l->d=realloc(l->d,l->cap*sizeof*l->d);}
    l->d[l->n++]=n;
}
typedef struct{char *name;TypeRef *type;}Param;
typedef struct{Param *d;int n,cap;}PList;
static void pl_push(PList *l,Param p){
    if(l->n>=l->cap){l->cap=l->cap?l->cap*2:4;l->d=realloc(l->d,l->cap*sizeof*l->d);}
    l->d[l->n++]=p;
}
typedef struct{char *name;TypeRef *type;}Field;
typedef struct{Field *d;int n,cap;}FList;
static void fl_push(FList *l,Field f){
    if(l->n>=l->cap){l->cap=l->cap?l->cap*2:4;l->d=realloc(l->d,l->cap*sizeof*l->d);}
    l->d[l->n++]=f;
}
typedef struct{Node *cond;NList body;}ElifClause;
typedef struct{ElifClause *d;int n,cap;}ElifList;
static void el_push(ElifList *l,ElifClause e){
    if(l->n>=l->cap){l->cap=l->cap?l->cap*2:4;l->d=realloc(l->d,l->cap*sizeof*l->d);}
    l->d[l->n++]=e;
}

struct Node{
    NK kind;int line;const char *file;
    TypeRef *typeref;       /* declared type (vardecl) */
    TypeRef *etype;         /* computed expression type (filled by type-checker) */
    long long ival;double dval;char *sval;int bval;char *op;
    int is_const;  /* for N_CONSTDECL: enforce immutability */
    int is_static; /* N_FUNC/N_VARDECL(global): file-local linkage (.local, not .globl) */
    int is_extern; /* N_FUNC/N_VARDECL(global): declared here, defined in another object file */
    int is_gconst; /* N_VARDECL(global): top-level `const` (immutable, compile-time constant initializer) */
    Node *left,*right,*cond;
    char *fname;PList params;TypeRef *rettype;NList body;
    char *structname;FList fields;
    ElifList elifs;NList else_body;
    Node *for_init,*for_post;
    char *callee;NList args;
    NList elems;
    char *import_path;char *name;
};

static Node *mknode(NK k,int line){
    Node *n=calloc(1,sizeof*n);n->kind=k;n->line=line;n->file=peek()->file;return n;
}

/* ═══════════════════════════════════════════════════════════════════════
   PARSER
   ═══════════════════════════════════════════════════════════════════════ */

static Node *parse_expr(void);
static NList parse_block(void);
/* forward declarations for typedef registry (defined after struct registry) */
static void register_typedef(const char *alias,TypeRef *t);
static TypeRef *resolve_typedef(const char *name);

static TypeRef *parse_type(void){
    skip_nl();Token *t=peek();
    if(t->type==TT_STAR){advance();return mktype(TY_PTR,NULL,parse_type());}
    if(t->type==TT_LBRACKET){
        advance();TypeRef *elem=parse_type();expect(TT_RBRACKET,"]");
        return mktype(TY_ARRAY,NULL,elem);
    }
    advance();
    switch(t->type){
        case TT_INT:  return mktype(TY_INT,NULL,NULL);
        case TT_LONG:  return mktype(TY_LONG,NULL,NULL);
        case TT_FLOAT: return mktype(TY_FLOAT,NULL,NULL);
        case TT_DOUBLE:return mktype(TY_DOUBLE,NULL,NULL);
        case TT_STR:   return mktype(TY_STR,NULL,NULL);
        case TT_BOOL: return mktype(TY_BOOL,NULL,NULL);
        case TT_VOID: return mktype(TY_VOID,NULL,NULL);
        case TT_IDENT:{
            /* check typedef registry first, then fall back to struct name */
            TypeRef *aliased=resolve_typedef(t->val);
            if(aliased)return aliased;
            return mktype(TY_STRUCT,t->val,NULL);
        }
        default:die("%s:%d: expected type, got '%s'",t->file,t->line,t->val);
    }
    return NULL;
}

static int is_intrinsic(const char *name){
    static const char *ii[]={
        "__syscall","__inb","__outb","__cli","__sti","__hlt",
        "__rdtsc","__peek","__poke","__peekb","__pokeb",
        "__memset","__memcpy",NULL
    };
    for(int i=0;ii[i];i++)if(strcmp(name,ii[i])==0)return 1;
    return 0;
}

static Node *parse_primary(void){
    skip_nl();Token *t=peek();
    if(t->type==TT_LONG_LIT){
        advance();Node *n=mknode(N_LONGLIT,t->line);
        if(t->val[0]=='0'&&(t->val[1]=='x'||t->val[1]=='X'))
            n->ival=(long long)strtoll(t->val,NULL,16);
        else if(t->val[0]=='0'&&(t->val[1]=='b'||t->val[1]=='B'))
            n->ival=(long long)strtoll(t->val+2,NULL,2);
        else n->ival=atoll(t->val);
        return n;
    }
    if(t->type==TT_FLOAT_LIT){
        advance();Node *n=mknode(N_FLOATLIT,t->line);
        n->dval=strtod(t->val,NULL);return n;
    }
    if(t->type==TT_DOUBLE_LIT){
        advance();Node *n=mknode(N_DOUBLELIT,t->line);
        n->dval=strtod(t->val,NULL);return n;
    }
    if(t->type==TT_INT_LIT){
        advance();Node *n=mknode(N_INTLIT,t->line);
        int is_dec=1;
        if(t->val[0]=='0'&&(t->val[1]=='x'||t->val[1]=='X')){
            n->ival=(long long)strtoull(t->val,NULL,16);is_dec=0;
        }else if(t->val[0]=='0'&&(t->val[1]=='b'||t->val[1]=='B')){
            n->ival=(long long)strtoull(t->val+2,NULL,2);is_dec=0;
        }else n->ival=atoll(t->val);
        /* A literal that does not fit in 32 bits is a long (like C). Without
           this the 32-bit backend emitted `movl $4294967296,%eax`, which the
           assembler silently truncated to 0 (the 64-bit backend was fine).
           Hex/binary up to 0xFFFFFFFF stay int so 0x80000000-style constants
           keep working. */
        if((is_dec&&n->ival>2147483647LL)||(!is_dec&&(unsigned long long)n->ival>0xFFFFFFFFULL))
            n->kind=N_LONGLIT;
        return n;
    }
    if(t->type==TT_STR_LIT){
        advance();Node *n=mknode(N_STRLIT,t->line);n->sval=xstrdup(t->val);return n;
    }
    if(t->type==TT_TRUE||t->type==TT_FALSE){
        advance();Node *n=mknode(N_BOOLLIT,t->line);n->bval=(t->type==TT_TRUE);return n;
    }
    if(t->type==TT_LBRACKET){
        advance();Node *n=mknode(N_ARRAYLIT,t->line);
        skip_nl();
        if(!check(TT_RBRACKET)){
            nl_push(&n->elems,parse_expr());skip_nl();
            while(match(TT_COMMA)){skip_nl();if(check(TT_RBRACKET))break;nl_push(&n->elems,parse_expr());skip_nl();}
        }
        expect(TT_RBRACKET,"]");return n;
    }
    if(t->type==TT_LPAREN){
        advance();skip_nl();Node *n=parse_expr();skip_nl();expect(TT_RPAREN,")");return n;
    }
    if(t->type==TT_IDENT){
        advance();skip_nl();
        if(check(TT_LPAREN)){
            advance();skip_nl();
            Node *n=mknode(N_CALL,t->line);n->callee=xstrdup(t->val);
            if(!check(TT_RPAREN)){
                nl_push(&n->args,parse_expr());skip_nl();
                while(match(TT_COMMA)){skip_nl();if(check(TT_RPAREN))break;nl_push(&n->args,parse_expr());skip_nl();}
            }
            expect(TT_RPAREN,")");return n;
        }
        Node *n=mknode(N_IDENT,t->line);n->name=xstrdup(t->val);return n;
    }
    die("%s:%d: unexpected token '%s' in expression",t->file,t->line,t->val);
    return NULL;
}

static Node *parse_postfix(void){
    Node *n=parse_primary();
    for(;;){
        skip_nl();
        if(check(TT_LBRACKET)){
            int line=peek()->line;advance();skip_nl();
            Node *idx=mknode(N_INDEX,line);idx->left=n;idx->right=parse_expr();
            skip_nl();expect(TT_RBRACKET,"]");n=idx;
        }else if(check(TT_DOT)){
            int line=peek()->line;advance();skip_nl();
            Token *f=expect(TT_IDENT,"field name");
            Node *fe=mknode(N_FIELD,line);fe->left=n;fe->sval=xstrdup(f->val);n=fe;
        }else break;
    }
    return n;
}

static Node *parse_unary(void){
    skip_nl();
    if(check(TT_MINUS)||check(TT_NOT)||check(TT_TILDE)){
        Token *t=advance();Node *n=mknode(N_UNOP,t->line);
        n->op=xstrdup(t->val);n->left=parse_unary();return n;
    }
    return parse_postfix();
}

static int prec(TT t){
    switch(t){
        case TT_OR:         return 1;
        case TT_AND:        return 2;
        case TT_PIPE:       return 3;
        case TT_CARET:      return 4;
        case TT_AMPERSAND:  return 5;
        case TT_EQ:case TT_NEQ:return 6;
        case TT_LT:case TT_GT:case TT_LTE:case TT_GTE:return 7;
        case TT_SHL:case TT_SHR:return 8;
        case TT_PLUS:case TT_MINUS:return 9;
        case TT_STAR:case TT_SLASH:case TT_MOD:return 10;
        default:return 0;
    }
}

static Node *parse_binop(int min_prec){
    Node *lhs=parse_unary();
    for(;;){
        skip_nl();int p=prec(peek()->type);if(p<min_prec)break;
        Token *op=advance();skip_nl();Node *rhs=parse_binop(p+1);
        Node *n=mknode(N_BINOP,op->line);n->op=xstrdup(op->val);n->left=lhs;n->right=rhs;lhs=n;
    }
    return lhs;
}

static Node *parse_expr(void){return parse_binop(1);}
static int is_lvalue(Node *n){return n->kind==N_IDENT||n->kind==N_INDEX||n->kind==N_FIELD;}

static int is_compound_assign(TT t){
    return t==TT_ASSIGN||t==TT_PLUS_ASSIGN||t==TT_MINUS_ASSIGN||
           t==TT_STAR_ASSIGN||t==TT_SLASH_ASSIGN||t==TT_MOD_ASSIGN||
           t==TT_SHL_ASSIGN||t==TT_SHR_ASSIGN||
           t==TT_AND_ASSIGN||t==TT_OR_ASSIGN||t==TT_XOR_ASSIGN;
}

static Node *parse_stmt(void){
    skip_nl();Token *t=peek();
    if(t->type==TT_IMPORT){
        advance();Token *path=expect(TT_STR_LIT,"import path");
        Node *n=mknode(N_IMPORT,t->line);n->import_path=xstrdup(path->val);return n;
    }
    if(t->type==TT_RETURN){
        advance();Node *n=mknode(N_RETURN,t->line);skip_nl();
        if(!check(TT_NEWLINE)&&!check(TT_RBRACE)&&!check(TT_EOF))n->left=parse_expr();
        return n;
    }
    if(t->type==TT_BREAK)   {advance();return mknode(N_BREAK,t->line);}
    if(t->type==TT_CONTINUE){advance();return mknode(N_CONTINUE,t->line);}
    if(t->type==TT_IF){
        advance();Node *n=mknode(N_IF,t->line);
        expect(TT_LPAREN,"(");skip_nl();n->cond=parse_expr();skip_nl();expect(TT_RPAREN,")");
        skip_nl();expect(TT_LBRACE,"{");n->body=parse_block();expect(TT_RBRACE,"}");
        for(;;){
            skip_nl();if(!check(TT_ELIF))break;advance();
            ElifClause ec={0};
            expect(TT_LPAREN,"(");skip_nl();ec.cond=parse_expr();skip_nl();expect(TT_RPAREN,")");
            skip_nl();expect(TT_LBRACE,"{");ec.body=parse_block();expect(TT_RBRACE,"}");
            el_push(&n->elifs,ec);
        }
        skip_nl();
        if(check(TT_ELSE)){
            advance();skip_nl();expect(TT_LBRACE,"{");n->else_body=parse_block();expect(TT_RBRACE,"}");
        }
        return n;
    }
    if(t->type==TT_WHILE){
        advance();Node *n=mknode(N_WHILE,t->line);
        expect(TT_LPAREN,"(");skip_nl();n->cond=parse_expr();skip_nl();expect(TT_RPAREN,")");
        skip_nl();expect(TT_LBRACE,"{");n->body=parse_block();expect(TT_RBRACE,"}");return n;
    }
    if(t->type==TT_FOR){
        advance();Node *n=mknode(N_FOR,t->line);
        expect(TT_LPAREN,"(");skip_nl();
        n->for_init=parse_stmt();skip_nl();expect(TT_COMMA,",");skip_nl();
        n->cond=parse_expr();skip_nl();expect(TT_COMMA,",");skip_nl();
        n->for_post=parse_stmt();skip_nl();expect(TT_RPAREN,")");
        skip_nl();expect(TT_LBRACE,"{");n->body=parse_block();expect(TT_RBRACE,"}");return n;
    }
    /* let x: type = expr  (mutable, same as vardecl) */
    if(t->type==TT_LET){
        advance();skip_nl();
        Token *nm=expect(TT_IDENT,"variable name");skip_nl();
        Node *n=mknode(N_LETDECL,nm->line);n->name=xstrdup(nm->val);
        if(match(TT_COLON)){skip_nl();n->typeref=parse_type();skip_nl();}
        if(match(TT_ASSIGN)){skip_nl();n->left=parse_expr();}
        return n;
    }
    /* static x: type [= expr]  (function-local, persistent storage — a
       hidden global under the hood, initialized once at load time; the
       initializer must be a compile-time literal, same restriction as
       top-level globals). */
    if(t->type==TT_STATIC){
        advance();skip_nl();
        Token *nm=expect(TT_IDENT,"variable name");skip_nl();expect(TT_COLON,":");skip_nl();
        Node *n=mknode(N_VARDECL,nm->line);n->name=xstrdup(nm->val);
        n->typeref=parse_type();skip_nl();
        if(match(TT_ASSIGN)){skip_nl();n->left=parse_expr();}
        n->is_static=1;
        return n;
    }
    /* const NAME: type = expr  (immutable compile-time constant) */
    if(t->type==TT_CONST){
        advance();skip_nl();
        Token *nm=expect(TT_IDENT,"constant name");skip_nl();
        Node *n=mknode(N_CONSTDECL,nm->line);n->name=xstrdup(nm->val);n->is_const=1;
        if(match(TT_COLON)){skip_nl();n->typeref=parse_type();skip_nl();}
        expect(TT_ASSIGN,"=");skip_nl();n->left=parse_expr();
        return n;
    }
    if(t->type==TT_IDENT){
        int saved=gpos;advance();skip_nl();
        if(check(TT_COLON)){
            advance();skip_nl();
            Node *n=mknode(N_VARDECL,t->line);n->name=xstrdup(t->val);
            n->typeref=parse_type();skip_nl();
            if(match(TT_ASSIGN)){skip_nl();n->left=parse_expr();}
            return n;
        }
        gpos=saved;
    }
    Node *expr=parse_expr();skip_nl();
    TT at=peek()->type;
    if(is_compound_assign(at)){
        if(!is_lvalue(expr))die("%s:%d: left side not assignable",peek()->file,peek()->line);
        Token *op=advance();skip_nl();
        Node *n=mknode(N_ASSIGN,op->line);n->op=xstrdup(op->val);n->left=expr;n->right=parse_expr();return n;
    }
    Node *s=mknode(N_EXPRSTMT,expr->line);s->left=expr;return s;
}

static NList parse_block(void){
    NList list={0};
    for(;;){skip_nl();TT t=peek()->type;if(t==TT_RBRACE||t==TT_EOF)break;nl_push(&list,parse_stmt());}
    return list;
}

static Node *parse_func(int is_static,int is_extern){
    Token *ft=expect(TT_FUNC,"func");Node *n=mknode(N_FUNC,ft->line);
    n->is_static=is_static;n->is_extern=is_extern;
    skip_nl();Token *nm=expect(TT_IDENT,"function name");n->fname=xstrdup(nm->val);
    skip_nl();expect(TT_LPAREN,"(");skip_nl();
    if(!check(TT_RPAREN)){
        do{
            skip_nl();if(check(TT_RPAREN))break;
            Token *pn=expect(TT_IDENT,"param name");skip_nl();expect(TT_COLON,":");skip_nl();
            Param p;p.name=xstrdup(pn->val);p.type=parse_type();pl_push(&n->params,p);skip_nl();
        }while(match(TT_COMMA));
    }
    skip_nl();expect(TT_RPAREN,")");skip_nl();
    n->rettype=mktype(TY_VOID,NULL,NULL);
    if(match(TT_ARROW)){skip_nl();n->rettype=parse_type();skip_nl();}
    if(is_extern){
        /* extern func: signature only, defined in another object file.
           No body — just a declaration, terminated like any other statement. */
        if(check(TT_LBRACE))die("%s:%d: extern function '%s' cannot have a body",n->file,n->line,n->fname);
        return n;
    }
    expect(TT_LBRACE,"{");n->body=parse_block();expect(TT_RBRACE,"}");return n;
}

static Node *parse_struct(void){
    Token *st=expect(TT_STRUCT,"struct");Node *n=mknode(N_STRUCT,st->line);
    skip_nl();Token *nm=expect(TT_IDENT,"struct name");n->structname=xstrdup(nm->val);
    skip_nl();expect(TT_LBRACE,"{");skip_nl();
    while(!check(TT_RBRACE)&&!check(TT_EOF)){
        skip_nl();if(check(TT_RBRACE))break;
        Token *fn2=expect(TT_IDENT,"field name");skip_nl();expect(TT_COLON,":");skip_nl();
        TypeRef *ftype=parse_type();
        Field f;f.name=xstrdup(fn2->val);f.type=ftype;fl_push(&n->fields,f);
        skip_nl();match(TT_COMMA);skip_nl();
    }
    expect(TT_RBRACE,"}");return n;
}

/* ── parse_typedef ──────────────────────────────────────────────────────
   Three C-style forms supported:
     typedef int           myint           -- primitive / any type alias
     typedef ExistingName  NewName         -- struct or type alias
     typedef struct { f:t ... } Name       -- anonymous inline struct + alias
     typedef struct Tag { f:t ... } Name   -- tagged inline struct + alias
   ─────────────────────────────────────────────────────────────────────── */
static Node *parse_typedef(void){
    Token *tt=expect(TT_TYPEDEF,"typedef");
    int ln=tt->line;
    skip_nl();

    /* Form 3: typedef struct { ... } Name */
    if(check(TT_STRUCT)){
        advance();skip_nl();

        /* optional tag: typedef struct Tag { } Alias  OR  typedef struct { } Alias */
        char *tag=NULL;
        if(check(TT_IDENT)){
            /* look one token ahead to decide if this is a tag or something else */
            /* We save position, consume the ident, check for '{', then restore or keep */
            int saved=gpos;
            Token *maybe_tag=advance();
            skip_nl();
            if(check(TT_LBRACE)){
                tag=xstrdup(maybe_tag->val); /* it was a tag */
            } else {
                gpos=saved; /* not a tag, put it back */
            }
        }

        expect(TT_LBRACE,"{");skip_nl();

        /* Parse field list */
        Node *sn=mknode(N_STRUCT,ln);
        while(!check(TT_RBRACE)&&!check(TT_EOF)){
            skip_nl();if(check(TT_RBRACE))break;
            Token *fn2=expect(TT_IDENT,"field name");skip_nl();
            expect(TT_COLON,":");skip_nl();
            TypeRef *ftype=parse_type();
            Field f;f.name=xstrdup(fn2->val);f.type=ftype;fl_push(&sn->fields,f);
            skip_nl();match(TT_COMMA);skip_nl();
        }
        expect(TT_RBRACE,"}");skip_nl();

        /* alias name after the closing brace */
        Token *aname=expect(TT_IDENT,"typedef name");
        char *alias=xstrdup(aname->val);

        /* Name the struct with the alias; store tag in sval for typecheck */
        sn->structname=xstrdup(alias);
        if(tag) sn->sval=xstrdup(tag);

        /* Register the typedef alias -> TY_STRUCT(alias) at parse time
           so that any subsequent parse_type() calls can resolve it.
           The StructInfo is registered in typecheck's first pass. */
        TypeRef *tr=mktype(TY_STRUCT,alias,NULL);
        register_typedef(alias,tr);
        if(tag){
            TypeRef *tr2=mktype(TY_STRUCT,alias,NULL);
            register_typedef(tag,tr2);
        }
        return sn;
    }

    /* Forms 1 & 2: typedef <type> <newname> */
    TypeRef *base=parse_type();
    skip_nl();
    Token *aname=expect(TT_IDENT,"typedef name");
    register_typedef(aname->val,base);

    /* Return a dummy N_IMPORT node (no code generated, just keeps AST clean) */
    Node *dummy=mknode(N_IMPORT,ln);
    dummy->import_path=NULL;
    dummy->name=xstrdup(aname->val);
    return dummy;
}

static Node *parse_program(void){
    Node *prog=mknode(N_PROGRAM,1);
    for(;;){
        skip_nl();TT t=peek()->type;if(t==TT_EOF)break;
        /* optional 'static' / 'extern' modifier ahead of func or a global var.
           At most one of the two may be given; order is fixed (modifier first). */
        int mod_static=0,mod_extern=0;
        if(t==TT_STATIC){advance();skip_nl();mod_static=1;t=peek()->type;}
        else if(t==TT_EXTERN){advance();skip_nl();mod_extern=1;t=peek()->type;}
        if(t==TT_CONST&&mod_extern)
            die("%s:%d: 'extern const' is not supported",peek()->file,peek()->line);
        if(t==TT_CONST){
            /* const NAME [: type] = constant-expression   (optionally `static const`) */
            advance();skip_nl();
            Token *nm=expect(TT_IDENT,"constant name");skip_nl();
            Node *n=mknode(N_VARDECL,nm->line);n->name=xstrdup(nm->val);
            if(match(TT_COLON)){skip_nl();n->typeref=parse_type();skip_nl();}
            expect(TT_ASSIGN,"=");skip_nl();n->left=parse_expr();
            n->is_const=2;n->is_gconst=1;n->is_static=mod_static;
            nl_push(&prog->body,n);
        }
        else if(t==TT_FUNC)     nl_push(&prog->body,parse_func(mod_static,mod_extern));
        else if(mod_static||mod_extern){
            /* only a global var declaration may follow a modifier now */
            if(t!=TT_IDENT)die("%s:%d: expected function or variable declaration after '%s'",
                peek()->file,peek()->line,mod_static?"static":"extern");
            Token *nm=expect(TT_IDENT,"variable name");skip_nl();expect(TT_COLON,":");skip_nl();
            Node *n=mknode(N_VARDECL,nm->line);n->name=xstrdup(nm->val);
            n->typeref=parse_type();skip_nl();
            if(mod_extern&&check(TT_ASSIGN))
                die("%s:%d: extern variable '%s' cannot have an initializer",n->file,n->line,n->name);
            if(match(TT_ASSIGN)){skip_nl();n->left=parse_expr();}
            n->is_const=2; /* sentinel: global */
            n->is_static=mod_static;n->is_extern=mod_extern;
            nl_push(&prog->body,n);
        }
        else if(t==TT_STRUCT)   nl_push(&prog->body,parse_struct());
        else if(t==TT_TYPEDEF)  nl_push(&prog->body,parse_typedef());
        else if(t==TT_IMPORT){
            advance();Token *path=expect(TT_STR_LIT,"import path");
            Node *im=mknode(N_IMPORT,path->line);im->import_path=xstrdup(path->val);
            nl_push(&prog->body,im);
        }
        /* top-level variable: name: type [= expr] */
        else if(t==TT_IDENT){
            int saved=gpos;advance();skip_nl();
            if(check(TT_COLON)){
                advance();skip_nl();
                Node *n=mknode(N_VARDECL,peek()->line);n->name=xstrdup(gtoks[saved].val);
                n->typeref=parse_type();skip_nl();
                if(match(TT_ASSIGN)){skip_nl();n->left=parse_expr();}
                n->is_const=2; /* sentinel: global */
                nl_push(&prog->body,n);
            } else {
                gpos=saved;
                die("%s:%d: unexpected top-level token '%s'",peek()->file,peek()->line,peek()->val);
            }
        }
        else die("%s:%d: unexpected top-level token '%s'",peek()->file,peek()->line,peek()->val);
    }
    return prog;
}

/* ═══════════════════════════════════════════════════════════════════════
   TYPE CHECKER
   Walks the AST after parsing. Annotates every expression node with
   its etype. Reports type errors with file+line and bails via die().
   ═══════════════════════════════════════════════════════════════════════ */

/* function registry for return-type lookup */
typedef struct{char *name;TypeRef *rettype;PList params;}FuncSig;
static FuncSig fsigs[1024];
static int     nfsigs=0;

static FuncSig *find_func(const char *name){
    for(int i=0;i<nfsigs;i++)if(strcmp(fsigs[i].name,name)==0)return &fsigs[i];
    return NULL;
}

/* struct registry (shared with codegen) */
typedef struct{char *name;Field *fields;int nfields;}StructInfo;
static StructInfo structs[512];
static int nstructs=0;

static StructInfo *find_struct(const char *name){
    /* direct lookup first */
    for(int i=0;i<nstructs;i++)if(strcmp(structs[i].name,name)==0)return &structs[i];
    /* then try to resolve through the typedef registry */
    TypeRef *aliased=resolve_typedef(name);
    if(aliased&&aliased->kind==TY_STRUCT&&aliased->name&&strcmp(aliased->name,name)!=0)
        return find_struct(aliased->name);
    return NULL;
}

/* Resolves the byte offset of `field_name` within the struct type of
   `base` (using base->etype, already computed by typecheck() since
   codegen runs on the same annotated tree). This MUST key off the
   specific struct type, not just the field name — two different
   structs are free to share a field name (e.g. both have a "kind" or
   "sval" field) at different offsets, and a name-only search across
   every registered struct would silently resolve to whichever struct
   happened to be registered first, corrupting memory. */
/* Bytes one field occupies. elem_size==8 is the x86-64 target (every field is
   a full 8-byte slot). On x86-32 fields are 4 bytes, except `long` and
   `double`, which need 8 — with a flat fi*4 layout a double field overlapped
   its neighbour and a long field lost its high word. */
static int field_size(TypeRef *t,int elem_size){
    if(elem_size==8)return 8;
    return (t&&(t->kind==TY_LONG||t->kind==TY_DOUBLE))?8:4;
}
static int field_offset_in(StructInfo *si,const char *field_name,int elem_size){
    int off=0;
    for(int fi=0;fi<si->nfields;fi++){
        if(strcmp(si->fields[fi].name,field_name)==0)return off;
        off+=field_size(si->fields[fi].type,elem_size);
    }
    return -1;
}
static int field_offset(Node *base,const char *field_name,int elem_size){
    TypeRef *bt=base->etype;
    if(bt&&bt->kind==TY_STRUCT&&bt->name){
        StructInfo *si=find_struct(bt->name);
        if(si){
            int o=field_offset_in(si,field_name,elem_size);
            if(o>=0)return o;
            die("%s:%d: struct '%s' has no field '%s'",base->file,base->line,bt->name,field_name);
        }
    }
    /* fallback for raw pointer/int-typed bases where etype couldn't be
       resolved to a specific struct: preserve old best-effort behaviour
       rather than hard-failing. */
    for(int si=0;si<nstructs;si++){
        int o=field_offset_in(&structs[si],field_name,elem_size);
        if(o>=0)return o;
    }
    die("%s:%d: unknown field '%s'",base->file,base->line,field_name);
    return -1;
}

/* ── typedef registry ───────────────────────────────────────────────── */
typedef struct{char *alias;TypeRef *type;}TypeAlias;
static TypeAlias typedefs[1024];
static int ntypedefs=0;

static void register_typedef(const char *alias,TypeRef *t){
    for(int i=0;i<ntypedefs;i++)
        if(strcmp(typedefs[i].alias,alias)==0)
            die("typedef: '%s' already defined",alias);
    if(ntypedefs>=1024)die("too many typedefs");
    typedefs[ntypedefs].alias=xstrdup(alias);
    typedefs[ntypedefs].type=t;
    ntypedefs++;
}

/* Resolve alias chain up to 64 hops */
static TypeRef *resolve_typedef(const char *name){
    const char *cur=name;
    for(int depth=0;depth<64;depth++){
        int found=0;
        for(int i=0;i<ntypedefs;i++){
            if(strcmp(typedefs[i].alias,cur)==0){
                TypeRef *t=typedefs[i].type;
                if(t->kind==TY_STRUCT&&t->name){
                    int is_alias=0;
                    for(int j=0;j<ntypedefs;j++)
                        if(strcmp(typedefs[j].alias,t->name)==0){is_alias=1;break;}
                    if(is_alias){cur=t->name;found=1;break;}
                }
                return t;
            }
        }
        if(!found)break;
    }
    return NULL;
}

/* variable type stack (same logical layout as codegen vars) */
typedef struct{char *name;TypeRef *type;int is_const;}TVar;
static TVar  tvars[4096];
static int   ntvars=0;
static int   scope_base=0;   /* first tvars[] index belonging to the innermost scope */

static TypeRef *find_tvar(const char *name){
    for(int i=ntvars-1;i>=0;i--)if(strcmp(tvars[i].name,name)==0)return tvars[i].type;
    return NULL;
}

/* ── global variables (declared here so typecheck can register them) ── */
typedef struct{char *name;TypeRef *type;long long ival;double dval;int has_init;int is_static;int is_extern;}GVar;
static GVar gvars[4096];
static int  ngvars=0;
static int  nslocals=0;         /* counter for unique static-local symbol names */
static const char *cur_fn_name=NULL; /* function currently being type-checked, for static-local mangling */
/* same name twice in one scope is an error (shadowing an outer scope is fine) */
static void check_redecl(const char *name,Node *at);
static void push_tvar(const char *name,TypeRef *t){
    if(ntvars>=4096)die("too many variables");
    tvars[ntvars].name=xstrdup(name);tvars[ntvars].type=t;tvars[ntvars].is_const=0;ntvars++;
}
static void push_tvar_const(const char *name,TypeRef *t){
    if(ntvars>=4096)die("too many variables");
    tvars[ntvars].name=xstrdup(name);tvars[ntvars].type=t;tvars[ntvars].is_const=1;ntvars++;
}
static void check_redecl(const char *name,Node *at){
    for(int i=scope_base;i<ntvars;i++)
        if(strcmp(tvars[i].name,name)==0)
            die("%s:%d: '%s' is already declared in this scope",at->file,at->line,name);
}
/* a string literal is a pointer, not a number: `x: int = "abc"` is a mistake */
static void check_strlit_dest(Node *e,TypeRef *dest,const char *what,Node *at){
    if(e&&e->kind==N_STRLIT&&dest&&(dest->kind==TY_INT||dest->kind==TY_BOOL))
        die("%s:%d: cannot assign str to %s of type %s",at->file,at->line,what,type_name(dest));
}

static TypeRef *tc_expr(Node *n,TypeRef *expected_ret);

/* ── architecture selection ──────────────────────────────────────── */
#define ARCH_X86_32 32
#define ARCH_X86_64 64
static int arch=ARCH_X86_32;   /* default: 32-bit; set from argv before typecheck() runs */

/* ── KNOWN, INTENTIONAL x86-32 vs x86-64 SEMANTIC DIFFERENCE ────────
   `int` arithmetic does NOT wrap/truncate at 32 bits on the x86-64
   target the way it naturally does on x86-32 (where results simply
   live in eax). On x86-64 every value — regardless of whether its
   Falcon type is `int` or `long` — is computed and stored in a full
   64-bit register/slot with no truncation, so e.g. `1 << 31` as an
   `int` yields 2147483648 on x86-64 but the C-correct -2147483648
   (INT32_MIN) on x86-32.

   This is NOT safely fixable as a narrow codegen patch: this language
   has no separate pointer type, so `int` is also the type used for
   every heap address (_flr_alloc's return, and by extension every
   struct field, array element, and __peek/__poke offset computation
   done through an `int`-typed base pointer). On x86-64 those
   addresses are real 48-bit-ish values from mmap(). Truncating every
   `int`-typed arithmetic result to 32 bits to fix the overflow
   semantics would silently corrupt that pointer arithmetic instead —
   trading a rare int-overflow edge case for pervasive breakage across
   every pointer-using program. Fixing this properly means introducing
   a real pointer type distinct from `int` (a language/type-system
   change, not a codegen bug fix). Left as documented, known behavior
   per an explicit decision — do not "fix" this with blanket
   truncation in gen_expr64's N_BINOP without that larger change. */

/* return type of a builtin/runtime call; NULL means "not a known builtin" */
static TypeRef *builtin_rettype(const char *name){
    if(has_std&&strcmp(name,"print")==0)     return mktype(TY_VOID,NULL,NULL);
    if(strcmp(name,"len")==0)                return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"str_concat")==0)         return mktype(TY_STR,NULL,NULL);
    if(strcmp(name,"str_format")==0)         return mktype(TY_STR,NULL,NULL);
    if(strcmp(name,"_flr_str_len")==0||
       strcmp(name,"_flr_strlen")==0)        return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"_flr_str_eq")==0)        return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"_flr_int_to_str")==0)    return mktype(TY_STR,NULL,NULL);
    if(strcmp(name,"_flr_str_to_int")==0)    return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"_flr_abs")==0)           return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"_flr_min")==0||
       strcmp(name,"_flr_max")==0)           return mktype(TY_INT,NULL,NULL);
    /* _flr_alloc returns a heap pointer straight out of mmap()/sbrk(); it must be
       typed at native pointer width for the target, or a 64-bit heap address
       gets truncated the moment it's assigned to a var (see emit_data's global
       sizing, which allocates .long for TY_INT and .quad for TY_LONG). */
    if(strcmp(name,"_flr_alloc")==0)         return mktype(arch==ARCH_X86_64?TY_LONG:TY_INT,NULL,NULL);
    if(strcmp(name,"_flr_free")==0)          return mktype(TY_VOID,NULL,NULL);
    if(strcmp(name,"_flr_exit")==0)          return mktype(TY_VOID,NULL,NULL);
    if(strcmp(name,"_flr_assert")==0)        return mktype(TY_VOID,NULL,NULL);
    /* intrinsics */
    if(strcmp(name,"__syscall")==0)          return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"__rdtsc")==0)            return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"__peek")==0||
       strcmp(name,"__peekb")==0||
       strcmp(name,"__inb")==0)              return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"__poke")==0||
       strcmp(name,"__pokeb")==0||
       strcmp(name,"__outb")==0||
       strcmp(name,"__cli")==0||
       strcmp(name,"__sti")==0||
       strcmp(name,"__hlt")==0||
       strcmp(name,"__memset")==0||
       strcmp(name,"__memcpy")==0)           return mktype(TY_VOID,NULL,NULL);
    return NULL;
}

static int is_fp_type(TypeRef *t){return t&&(t->kind==TY_FLOAT||t->kind==TY_DOUBLE);}

/* wrap a float/double expression as `(e != 0.0)` so it can be used as a
   condition / logical operand (the integer code paths just test eax/rax,
   which for a float is its bit pattern or — on x86-64 — a stale register) */
static Node *fp_to_bool(Node *e){
    if(!e||!is_fp_type(e->etype))return e;
    Node *z=calloc(1,sizeof*z);z->kind=N_DOUBLELIT;z->line=e->line;z->file=e->file;
    z->dval=0.0;z->etype=mktype(TY_DOUBLE,NULL,NULL);
    Node *b=calloc(1,sizeof*b);b->kind=N_BINOP;b->line=e->line;b->file=e->file;
    b->op=xstrdup("!=");b->left=e;b->right=z;b->etype=mktype(TY_BOOL,NULL,NULL);
    return b;
}

/* result type + operand validation for every binary operator once both
   operand types are known. Shared by N_BINOP and desugared `x op= y`. */
static void tc_binop_finish(Node *n,TypeRef *lt,TypeRef *rt){
    const char *op=n->op;
    int cmp=(strcmp(op,"==")==0||strcmp(op,"!=")==0||strcmp(op,"<")==0||
             strcmp(op,">")==0||strcmp(op,"<=")==0||strcmp(op,">=")==0);
    if(cmp){
        /* numeric widening is fine in either direction (`i > f`, `f < i`) */
        if(!types_compat(lt,rt)&&!types_compat(rt,lt))
            die("%s:%d: cannot compare %s with %s",n->file,n->line,type_name(lt),type_name(rt));
        n->etype=mktype(TY_BOOL,NULL,NULL);
    }else if(strcmp(op,"and")==0||strcmp(op,"or")==0){
        n->left=fp_to_bool(n->left);n->right=fp_to_bool(n->right);
        n->etype=mktype(TY_BOOL,NULL,NULL);
    }else if(strcmp(op,"+")==0&&(lt->kind==TY_STR||rt->kind==TY_STR)){
        /* str + str only allowed if both are str; otherwise error */
        if(lt->kind!=TY_STR||rt->kind!=TY_STR)
            die("%s:%d: cannot add str and %s — use str_concat()",n->file,n->line,
                lt->kind!=TY_STR?type_name(lt):type_name(rt));
        n->etype=mktype(TY_STR,NULL,NULL);
    }else{
        if(!type_is_numeric(lt))
            die("%s:%d: operator '%s' requires numeric left operand, got %s",
                n->file,n->line,op,type_name(lt));
        if(!type_is_numeric(rt))
            die("%s:%d: operator '%s' requires numeric right operand, got %s",
                n->file,n->line,op,type_name(rt));
        int fp=is_fp_type(lt)||is_fp_type(rt);
        if(fp&&(strcmp(op,"+")!=0&&strcmp(op,"-")!=0&&strcmp(op,"*")!=0&&strcmp(op,"/")!=0&&strcmp(op,"%")!=0))
            die("%s:%d: operator '%s' is not defined for float/double operands",n->file,n->line,op);
        /* double > float > long > int  (a long mixed with a double is a double) */
        if(lt->kind==TY_DOUBLE||rt->kind==TY_DOUBLE)
            n->etype=mktype(TY_DOUBLE,NULL,NULL);
        else if(lt->kind==TY_FLOAT||rt->kind==TY_FLOAT)
            n->etype=mktype(TY_FLOAT,NULL,NULL);
        else if(lt->kind==TY_LONG||rt->kind==TY_LONG)
            n->etype=mktype(TY_LONG,NULL,NULL);
        else
            n->etype=mktype(TY_INT,NULL,NULL);
    }
}

static TypeRef *tc_expr(Node *n,TypeRef *ret){
    if(!n)return mktype(TY_VOID,NULL,NULL);
    switch(n->kind){
    case N_INTLIT:  n->etype=mktype(TY_INT,NULL,NULL); break;
    case N_LONGLIT: n->etype=mktype(TY_LONG,NULL,NULL);break;
    case N_FLOATLIT: n->etype=mktype(TY_FLOAT,NULL,NULL); break;
    case N_DOUBLELIT:n->etype=mktype(TY_DOUBLE,NULL,NULL); break;
    case N_STRLIT:  n->etype=mktype(TY_STR,NULL,NULL); break;
    case N_BOOLLIT: n->etype=mktype(TY_BOOL,NULL,NULL);break;
    case N_IDENT:{
        TypeRef *vt=find_tvar(n->name);
        if(!vt)die("%s:%d: undefined variable '%s'",n->file,n->line,n->name);
        n->etype=vt;break;
    }
    case N_UNOP:{
        TypeRef *et=tc_expr(n->left,ret);
        if(strcmp(n->op,"not")==0){
            if(is_fp_type(et)){
                /* not x  ==>  x == 0.0 */
                Node *z=calloc(1,sizeof*z);z->kind=N_DOUBLELIT;z->line=n->line;z->file=n->file;
                z->dval=0.0;z->etype=mktype(TY_DOUBLE,NULL,NULL);
                n->kind=N_BINOP;n->op=xstrdup("==");n->right=z;
            }
            n->etype=mktype(TY_BOOL,NULL,NULL);
        }else if(strcmp(n->op,"-")==0||strcmp(n->op,"~")==0){
            if(!type_is_numeric(et))
                die("%s:%d: unary '%s' requires numeric type, got %s",n->file,n->line,n->op,type_name(et));
            if(strcmp(n->op,"~")==0&&is_fp_type(et))
                die("%s:%d: unary '~' is not defined for float/double",n->file,n->line);
            n->etype=et;
        }
        break;
    }
    case N_BINOP:{
        TypeRef *lt=tc_expr(n->left,ret);
        TypeRef *rt=tc_expr(n->right,ret);
        tc_binop_finish(n,lt,rt);
        break;
    }
    case N_CALL:{
        /* check each argument */
        for(int i=0;i<n->args.n;i++)tc_expr(n->args.d[i],ret);
        /* virtual std special: print takes any single value */
        if(has_std&&strcmp(n->callee,"print")==0){
            if(n->args.n!=1)die("%s:%d: print takes 1 argument",n->file,n->line);
            n->etype=mktype(TY_VOID,NULL,NULL);break;
        }
        /* str_concat(a:str, b:str) -> str */
        if(strcmp(n->callee,"str_concat")==0){
            if(n->args.n!=2)die("%s:%d: str_concat takes 2 arguments",n->file,n->line);
            TypeRef *a=n->args.d[0]->etype,*b=n->args.d[1]->etype;
            if(a->kind!=TY_STR)die("%s:%d: str_concat arg1 must be str, got %s",n->file,n->line,type_name(a));
            if(b->kind!=TY_STR)die("%s:%d: str_concat arg2 must be str, got %s",n->file,n->line,type_name(b));
            n->etype=mktype(TY_STR,NULL,NULL);break;
        }
        /* str_format(fmt:str, ...) -> str — fmt must be str literal or str var */
        if(strcmp(n->callee,"str_format")==0){
            if(n->args.n<1)die("%s:%d: str_format needs at least a format string",n->file,n->line);
            TypeRef *fmt=n->args.d[0]->etype;
            if(fmt->kind!=TY_STR)die("%s:%d: str_format first arg must be str, got %s",n->file,n->line,type_name(fmt));
            if(n->args.n>9)die("%s:%d: str_format supports at most 8 format arguments",n->file,n->line);
            n->etype=mktype(TY_STR,NULL,NULL);break;
        }
        /* known builtins */
        TypeRef *brt=builtin_rettype(n->callee);
        if(brt){n->etype=brt;break;}
        /* user-defined functions */
        FuncSig *sig=find_func(n->callee);
        if(sig){
            /* check arg count */
            if(n->args.n!=sig->params.n)
                die("%s:%d: function '%s' expects %d args, got %d",
                    n->file,n->line,n->callee,sig->params.n,n->args.n);
            /* check arg types */
            for(int i=0;i<n->args.n;i++){
                TypeRef *at=n->args.d[i]->etype;
                TypeRef *pt=sig->params.d[i].type;
                if(!types_compat(at,pt)&&!types_compat(pt,at))
                    die("%s:%d: arg %d of '%s': expected %s, got %s",
                        n->file,n->line,i+1,n->callee,type_name(pt),type_name(at));
                /* a str literal passed as an `int` parameter is allowed on purpose:
                   bare-metal code takes pointers as ints (examples/EXAMPLE-OS). Only
                   bool, which can't hold a pointer, is rejected. */
                if(n->args.d[i]->kind==N_STRLIT&&pt&&pt->kind==TY_BOOL)
                    die("%s:%d: arg %d of '%s': expected bool, got str",
                        n->file,n->line,i+1,n->callee);
            }
            n->etype=sig->rettype;break;
        }
        /* unknown. Runtime-internal helpers (_flr_*) that have no entry in
           builtin_rettype() are still allowed as untyped void calls; anything
           else must be declared (`extern func f(...) -> T`) or defined. */
        if(strncmp(n->callee,"_flr_",5)==0){
            warn("%s:%d: unknown runtime function '%s' — assuming void",n->file,n->line,n->callee);
            n->etype=mktype(TY_VOID,NULL,NULL);
            break;
        }
        if(strcmp(n->callee,"print")==0)
            die("%s:%d: unknown function 'print' (missing `import \"std\"`?)",n->file,n->line);
        die("%s:%d: unknown function '%s' (declare it with `extern func %s(...) -> type`)",
            n->file,n->line,n->callee,n->callee);
        break;
    }
    case N_INDEX:{
        TypeRef *at=tc_expr(n->left,ret);
        TypeRef *it=tc_expr(n->right,ret);
        if(at->kind!=TY_ARRAY)
            die("%s:%d: index operator on non-array type %s",n->file,n->line,type_name(at));
        if(!type_is_numeric(it))
            die("%s:%d: array index must be numeric, got %s",n->file,n->line,type_name(it));
        n->etype=at->elem?at->elem:mktype(TY_INT,NULL,NULL);
        break;
    }
    case N_FIELD:{
        TypeRef *st=tc_expr(n->left,ret);
        if(st->kind!=TY_STRUCT&&st->kind!=TY_INT&&st->kind!=TY_PTR){
            /* tolerate int/ptr (raw alloc returns int) */
        }
        /* look up field type */
        if(st->kind==TY_STRUCT&&st->name){
            StructInfo *si=find_struct(st->name);
            if(si){
                for(int i=0;i<si->nfields;i++){
                    if(strcmp(si->fields[i].name,n->sval)==0){
                        n->etype=si->fields[i].type;goto field_done;
                    }
                }
                die("%s:%d: struct '%s' has no field '%s'",n->file,n->line,st->name,n->sval);
            }
        }
        /* raw pointer / unknown struct: assume int field */
        n->etype=mktype(TY_INT,NULL,NULL);
        field_done:;
        break;
    }
    case N_ARRAYLIT:{
        TypeRef *elem=NULL;
        for(int i=0;i<n->elems.n;i++){
            TypeRef *et=tc_expr(n->elems.d[i],ret);
            if(!elem)elem=et;
            else if(!types_compat(elem,et))
                die("%s:%d: array literal has mixed types (%s vs %s)",
                    n->file,n->line,type_name(elem),type_name(et));
        }
        if(!elem)elem=mktype(TY_INT,NULL,NULL);
        n->etype=mktype(TY_ARRAY,NULL,elem);
        break;
    }
    default:
        n->etype=mktype(TY_VOID,NULL,NULL);break;
    }
    return n->etype;
}

/* Fold a global / static-local initializer to a constant. Handles integer,
   long, bool and float/double literals, unary minus / bitwise-not on them
   (`g: double = -1.5`), and converts to the variable's declared type (so
   `g: double = 9` is 9.0, not 0.0). Returns 1 on success. */
static int global_tvars_end=0x7fffffff;   /* tvars[0..global_tvars_end) are globals; anything above is a local in scope */
static Node *fold_prog=NULL;   /* program being compiled: lets constant folding see global `const`s */
static int fold_depth=0;
static int fold_const(Node *e,int *is_fp,long long *iv,double *dv);
static Node *find_gconst(const char *name){
    if(!fold_prog)return NULL;
    for(int i=0;i<fold_prog->body.n;i++){
        Node *g=fold_prog->body.d[i];
        if(g->kind==N_VARDECL&&g->is_gconst&&g->name&&!strcmp(g->name,name))return g;
    }
    return NULL;
}
static int fold_const(Node *e,int *is_fp,long long *iv,double *dv){
    if(!e)return 0;
    switch(e->kind){
    case N_IDENT:{
        Node *g=find_gconst(e->name);
        if(!g||fold_depth>32)return 0;
        /* a local variable of the same name shadows the global const; a local
           isn't a compile-time constant, so don't silently use the global's value */
        for(int i=ntvars-1;i>=global_tvars_end;i--)
            if(strcmp(tvars[i].name,e->name)==0)return 0;
        fold_depth++;int ok=fold_const(g->left,is_fp,iv,dv);fold_depth--;
        if(!ok)return 0;
        if(g->typeref&&is_fp_type(g->typeref)&&!*is_fp){*dv=(double)*iv;*is_fp=1;}
        else if(g->typeref&&!is_fp_type(g->typeref)&&*is_fp)return 0;   /* ill-typed; reported when g itself is checked */
        return 1;
    }
    case N_BINOP:{
        int lf=0,rf=0;long long li=0,ri=0;double ld=0,rd=0;
        if(!fold_const(e->left,&lf,&li,&ld)||!fold_const(e->right,&rf,&ri,&rd))return 0;
        const char *op=e->op;
        if(lf||rf){
            double a=lf?ld:(double)li,b=rf?rd:(double)ri;
            if     (!strcmp(op,"+"))*dv=a+b;
            else if(!strcmp(op,"-"))*dv=a-b;
            else if(!strcmp(op,"*"))*dv=a*b;
            else if(!strcmp(op,"/")){if(b==0.0)return 0;*dv=a/b;}
            else return 0;
            *is_fp=1;return 1;
        }
        long long r;
        if     (!strcmp(op,"+"))r=(long long)((unsigned long long)li+(unsigned long long)ri);
        else if(!strcmp(op,"-"))r=(long long)((unsigned long long)li-(unsigned long long)ri);
        else if(!strcmp(op,"*"))r=(long long)((unsigned long long)li*(unsigned long long)ri);
        else if(!strcmp(op,"/")){if(ri==0||(ri==-1&&li==(-9223372036854775807LL-1)))return 0;r=li/ri;}
        else if(!strcmp(op,"%")){if(ri==0||ri==-1)return ri==-1?(*iv=0,*is_fp=0,1):0;r=li%ri;}
        else if(!strcmp(op,"&"))r=li&ri;
        else if(!strcmp(op,"|"))r=li|ri;
        else if(!strcmp(op,"^"))r=li^ri;
        else if(!strcmp(op,"<<")){if(ri<0||ri>62)return 0;r=(long long)((unsigned long long)li<<ri);}
        else if(!strcmp(op,">>")){if(ri<0||ri>63)return 0;r=li>>ri;}
        else return 0;
        *iv=r;*is_fp=0;return 1;
    }
    case N_INTLIT:case N_LONGLIT:case N_BOOLLIT:
        *is_fp=0;*iv=(e->kind==N_BOOLLIT)?(e->bval?1:0):e->ival;return 1;
    case N_FLOATLIT:case N_DOUBLELIT:
        *is_fp=1;*dv=e->dval;return 1;
    case N_UNOP:{
        if(!fold_const(e->left,is_fp,iv,dv))return 0;
        if(!strcmp(e->op,"-")){if(*is_fp)*dv=-*dv;else *iv=-*iv;return 1;}
        if(!strcmp(e->op,"~")&&!*is_fp){*iv=~*iv;return 1;}
        if(!strcmp(e->op,"not")){*iv=*is_fp?(*dv==0.0):(*iv==0);*is_fp=0;return 1;}
        return 0;
    }
    default:return 0;
    }
}
static void fold_global_init(Node *init,TypeRef *vt,long long *iv,double *dv,const char *name,Node *at){
    *iv=0;*dv=0.0;
    if(!init)return;
    int fp=0;long long i=0;double d=0.0;
    if(!fold_const(init,&fp,&i,&d)){
        warn("%s:%d: initializer of global '%s' is not a compile-time constant; it will start at 0",
             at->file,at->line,name);
        return;
    }
    if(vt&&(vt->kind==TY_FLOAT||vt->kind==TY_DOUBLE)){*dv=fp?d:(double)i;}
    else                                              {*iv=fp?(long long)d:i;}
}

static void tc_stmts(NList *stmts,TypeRef *ret_type,int in_loop);

static void tc_stmt(Node *n,TypeRef *ret_type,int in_loop){
    if(!n)return;
    switch(n->kind){
    case N_IMPORT:break;
    case N_VARDECL:
    case N_LETDECL:{
        TypeRef *decl=n->typeref;
        if(n->left){
            TypeRef *et=tc_expr(n->left,ret_type);
            if(decl&&!types_compat(decl,et))
                die("%s:%d: cannot assign %s to variable '%s' of type %s",
                    n->file,n->line,type_name(et),n->name,type_name(decl));
            check_strlit_dest(n->left,decl,"variable",n);
            if(decl&&decl->kind==TY_ARRAY&&decl->elem&&n->left->kind==N_ARRAYLIT)
                n->left->etype=decl;   /* elements are converted to the declared element type */
        }
        TypeRef *vt=decl?decl:(n->left?n->left->etype:mktype(TY_INT,NULL,NULL));
        if(n->is_static){
            /* function-local static: give it a unique hidden-global symbol
               name and register it in gvars for codegen, same as a
               top-level global. n->sval carries the mangled symbol name
               through to statement codegen. */
            char mangled[256];
            snprintf(mangled,sizeof mangled,"_slocal_%s_%s_%d",cur_fn_name?cur_fn_name:"fn",n->name,nslocals++);
            n->sval=xstrdup(mangled);
            if(ngvars<4096){
                gvars[ngvars].name=xstrdup(mangled);
                gvars[ngvars].type=vt;
                gvars[ngvars].has_init=(n->left!=NULL);
                gvars[ngvars].is_static=1;   /* always file-local — never exported */
                gvars[ngvars].is_extern=0;
                long long iv=0;double dv=0.0;
                fold_global_init(n->left,vt,&iv,&dv,n->name,n);
                gvars[ngvars].ival=iv;
                gvars[ngvars].dval=dv;
                n->ival=ngvars; /* stash gvars index for statement codegen */
                ngvars++;
            }
        }
        check_redecl(n->name,n);
        push_tvar(n->name,vt);
        break;
    }
    case N_CONSTDECL:{
        TypeRef *decl=n->typeref;
        if(!n->left)die("%s:%d: const '%s' requires an initializer",n->file,n->line,n->name);
        TypeRef *et=tc_expr(n->left,ret_type);
        if(decl&&!types_compat(decl,et))
            die("%s:%d: cannot assign %s to const '%s' of type %s",
                n->file,n->line,type_name(et),n->name,type_name(decl));
        check_strlit_dest(n->left,decl,"const",n);
        check_redecl(n->name,n);
        push_tvar_const(n->name,decl?decl:et);
        break;
    }
    case N_ASSIGN:{
        TypeRef *rhs=tc_expr(n->right,ret_type);
        TypeRef *lhs=tc_expr(n->left,ret_type);
        /* check const immutability */
        if(n->left->kind==N_IDENT){
            for(int ci=ntvars-1;ci>=0;ci--){
                if(strcmp(tvars[ci].name,n->left->name)==0){
                    if(tvars[ci].is_const)
                        die("%s:%d: cannot assign to const '%s'",n->file,n->line,n->left->name);
                    break;
                }
            }
        }
        if(strcmp(n->op,"=")==0){
            if(!types_compat(lhs,rhs))
                die("%s:%d: cannot assign %s to %s",n->file,n->line,type_name(rhs),type_name(lhs));
            check_strlit_dest(n->right,lhs,"a variable",n);
            if(lhs->kind==TY_ARRAY&&lhs->elem&&n->right->kind==N_ARRAYLIT)
                n->right->etype=lhs;
        }else{
            /* compound assign: both sides must be numeric (or str for +=) */
            if(!type_is_numeric(lhs)&&!(strcmp(n->op,"+=")==0&&lhs->kind==TY_STR))
                die("%s:%d: compound assignment requires numeric left operand, got %s",
                    n->file,n->line,type_name(lhs));
            /* Desugar `x op= y` into `x = x op y`. The old per-backend
               compound code only knew 32/64-bit integer ops, so `+=` on a
               float, double or (x86-32) long silently did integer math on
               the raw bits. A real binop gets the correct typing and
               codegen for free. */
            size_t ol=strlen(n->op);
            char *bop=xstrndup(n->op,ol-1);
            Node *b=calloc(1,sizeof*b);
            b->kind=N_BINOP;b->line=n->line;b->file=n->file;
            b->op=bop;b->left=n->left;b->right=n->right;
            tc_binop_finish(b,lhs,rhs);
            n->op=xstrdup("=");
            n->right=b;
        }
        break;
    }
    case N_RETURN:{
        TypeRef *et=n->left?tc_expr(n->left,ret_type):mktype(TY_VOID,NULL,NULL);
        if(ret_type&&ret_type->kind!=TY_VOID&&!types_compat(ret_type,et))
            die("%s:%d: return type mismatch: function returns %s, got %s",
                n->file,n->line,type_name(ret_type),type_name(et));
        if(ret_type&&ret_type->kind==TY_VOID&&n->left&&et->kind!=TY_VOID)
            die("%s:%d: return type mismatch: function returns void, got %s",
                n->file,n->line,type_name(et));
        check_strlit_dest(n->left,ret_type,"a return value",n);
        break;
    }
    case N_EXPRSTMT:tc_expr(n->left,ret_type);break;
    case N_IF:{
        TypeRef *ct=tc_expr(n->cond,ret_type);
        (void)ct;/* any type allowed in condition */
        n->cond=fp_to_bool(n->cond);
        tc_stmts(&n->body,ret_type,in_loop);
        for(int i=0;i<n->elifs.n;i++){
            tc_expr(n->elifs.d[i].cond,ret_type);
            n->elifs.d[i].cond=fp_to_bool(n->elifs.d[i].cond);
            tc_stmts(&n->elifs.d[i].body,ret_type,in_loop);
        }
        tc_stmts(&n->else_body,ret_type,in_loop);
        break;
    }
    case N_WHILE:{
        tc_expr(n->cond,ret_type);
        n->cond=fp_to_bool(n->cond);
        tc_stmts(&n->body,ret_type,1);
        break;
    }
    case N_FOR:{
        int sv=ntvars,sb=scope_base;scope_base=ntvars;   /* init var lives only in the loop */
        tc_stmt(n->for_init,ret_type,0);
        tc_expr(n->cond,ret_type);
        n->cond=fp_to_bool(n->cond);
        tc_stmt(n->for_post,ret_type,0);
        tc_stmts(&n->body,ret_type,1);
        ntvars=sv;scope_base=sb;
        break;
    }
    case N_BREAK:case N_CONTINUE:
        if(!in_loop)die("%s:%d: break/continue outside loop",n->file,n->line);
        break;
    default:break;
    }
}

static void tc_stmts(NList *stmts,TypeRef *ret_type,int in_loop){
    /* every block is its own scope, matching the code generators, so names
       declared inside an if/while/for body don't leak out */
    int sv=ntvars,sb=scope_base;scope_base=ntvars;
    for(int i=0;i<stmts->n;i++)tc_stmt(stmts->d[i],ret_type,in_loop);
    ntvars=sv;scope_base=sb;
}

/* Globals whose initializer is not a compile-time constant (array literals,
   string literals, calls, ...) used to start at 0 with only a warning, so an
   array global was a null pointer. Move each such initializer into an
   assignment at the top of main(), in declaration order. */
static void hoist_global_inits(Node *prog){
    Node *mainfn=NULL;
    for(int i=0;i<prog->body.n;i++){
        Node *n=prog->body.d[i];
        if(n->kind==N_FUNC&&n->fname&&!strcmp(n->fname,"main")&&!n->is_extern){mainfn=n;break;}
    }
    if(!mainfn)return;
    NList pre={0};
    for(int i=0;i<prog->body.n;i++){
        Node *n=prog->body.d[i];
        if(n->kind!=N_VARDECL||n->is_const!=2||n->is_extern||!n->left||!n->typeref)continue;
        int fp=0;long long iv=0;double dv=0.0;
        if(fold_const(n->left,&fp,&iv,&dv))continue;
        Node *id=mknode(N_IDENT,n->line);id->name=xstrdup(n->name);id->file=n->file;
        Node *as=mknode(N_ASSIGN,n->line);as->op=xstrdup("=");as->file=n->file;
        as->left=id;as->right=n->left;
        n->left=NULL;
        nl_push(&pre,as);
    }
    if(!pre.n)return;
    for(int i=0;i<mainfn->body.n;i++)nl_push(&pre,mainfn->body.d[i]);
    free(mainfn->body.d);
    mainfn->body=pre;
}

/* conservative "does every path through this block end in a return?" */
static int block_returns(NList *b);
static int stmt_returns(Node *n){
    if(!n)return 0;
    if(n->kind==N_RETURN)return 1;
    if(n->kind==N_WHILE)   /* while (true) { ... } with no break never falls out */
        return n->cond&&n->cond->kind==N_BOOLLIT&&n->cond->bval;
    if(n->kind==N_IF){
        if(!n->else_body.n)return 0;
        if(!block_returns(&n->body))return 0;
        for(int i=0;i<n->elifs.n;i++)if(!block_returns(&n->elifs.d[i].body))return 0;
        return block_returns(&n->else_body);
    }
    return 0;
}
static int block_returns(NList *b){
    for(int i=0;i<b->n;i++)if(stmt_returns(b->d[i]))return 1;
    return 0;
}

/* two top-level definitions with the same name: an `extern` declaration may
   coexist with the real definition, anything else is a redefinition */
static void check_toplevel_dups(Node *prog){
    int has_main=0;
    for(int i=0;i<prog->body.n;i++){
        Node *n=prog->body.d[i];
        int isf=(n->kind==N_FUNC);
        int isg=(n->kind==N_VARDECL&&n->is_const==2);
        if(!isf&&!isg)continue;
        const char *nm=isf?n->fname:n->name;
        if(isf&&nm&&!strcmp(nm,"main")&&!n->is_extern)has_main=1;
        for(int j=0;j<i;j++){
            Node *m=prog->body.d[j];
            int mf=(m->kind==N_FUNC),mg=(m->kind==N_VARDECL&&m->is_const==2);
            if(!(isf?mf:mg))continue;
            const char *mn=mf?m->fname:m->name;
            if(!nm||!mn||strcmp(nm,mn))continue;
            if(n->is_extern||m->is_extern)continue;
            die("%s:%d: %s '%s' is already defined (first defined at %s:%d)",
                n->file,n->line,isf?"function":"global",nm,m->file,m->line);
        }
    }
    if(!has_main)
        warn("no 'main' function defined (fine for a library object; a linked program needs one)");
}

static void typecheck(Node *prog){
    check_toplevel_dups(prog);
    fold_prog=prog;
    /* a global `const` must have a compile-time-constant initializer: it is
       never hoisted into main() */
    for(int i=0;i<prog->body.n;i++){
        Node *n=prog->body.d[i];
        if(n->kind!=N_VARDECL||!n->is_gconst)continue;
        int fp=0;long long iv=0;double dv=0.0;
        if(!fold_const(n->left,&fp,&iv,&dv))
            die("%s:%d: initializer of const '%s' is not a compile-time constant",n->file,n->line,n->name);
    }
    hoist_global_inits(prog);
    /* first pass: register all structs and function signatures */
    for(int i=0;i<prog->body.n;i++){
        Node *n=prog->body.d[i];
        if(n->kind==N_STRUCT){
            /* avoid double-registering (typedef struct {} Name already called
               register_typedef at parse time; we still need the StructInfo) */
            if(!find_struct(n->structname)){
                StructInfo *si=&structs[nstructs++];
                si->name=xstrdup(n->structname);
                si->fields=n->fields.d;si->nfields=n->fields.n;
            }
            /* if this was 'typedef struct Tag { } Alias', also register Tag */
            if(n->sval&&!find_struct(n->sval)){
                StructInfo *si2=&structs[nstructs++];
                si2->name=xstrdup(n->sval);
                si2->fields=n->fields.d;si2->nfields=n->fields.n;
            }
        }
        if(n->kind==N_FUNC){
            if(nfsigs>=1024)die("too many functions");
            fsigs[nfsigs].name=xstrdup(n->fname);
            fsigs[nfsigs].rettype=n->rettype;
            fsigs[nfsigs].params=n->params;
            nfsigs++;
        }
    }
    /* register top-level var declarations as globals in tvars so funcs can see
       them (after all function signatures, so initializers may call any function) */
    for(int i=0;i<prog->body.n;i++){
        Node *n=prog->body.d[i];
        if(n->kind==N_VARDECL&&n->is_const==2){
            TypeRef *vt=n->typeref;
            if(n->left){
                TypeRef *et=tc_expr(n->left,NULL);
                if(!vt)vt=et;
                else{
                    if(!types_compat(vt,et))
                        die("%s:%d: cannot assign %s to global '%s' of type %s",
                            n->file,n->line,type_name(et),n->name,type_name(vt));
                    check_strlit_dest(n->left,vt,"global",n);
                    if(vt->kind==TY_ARRAY&&vt->elem&&n->left->kind==N_ARRAYLIT)n->left->etype=vt;
                }
            }
            if(n->is_gconst)push_tvar_const(n->name,vt?vt:mktype(TY_INT,NULL,NULL));
            else            push_tvar(n->name,vt?vt:mktype(TY_INT,NULL,NULL));
            /* also register into gvars for codegen */
            if(ngvars<4096){
                gvars[ngvars].name=xstrdup(n->name);
                gvars[ngvars].type=vt;
                gvars[ngvars].has_init=(n->left!=NULL);
                gvars[ngvars].is_static=n->is_static;
                gvars[ngvars].is_extern=n->is_extern;
                /* extract literal value if it's a simple int/bool/long/float/
                   double literal (only compile-time-constant initializers are
                   supported; anything else stays zero and must be assigned
                   at runtime, as with e.g. a heap pointer from _flr_alloc) */
                long long iv=0;double dv=0.0;
                fold_global_init(n->left,vt,&iv,&dv,n->name,n);
                gvars[ngvars].ival=iv;
                gvars[ngvars].dval=dv;
                ngvars++;
            }
        }
    }
    global_tvars_end=ntvars;
    /* second pass: type-check each function body */
    for(int i=0;i<prog->body.n;i++){
        Node *n=prog->body.d[i];
        if(n->kind!=N_FUNC)continue;
        int saved_ntvars=ntvars;
        scope_base=ntvars;   /* params form the outermost local scope */
        /* push params into scope */
        for(int j=0;j<n->params.n;j++){
            check_redecl(n->params.d[j].name,n);
            push_tvar(n->params.d[j].name,n->params.d[j].type);
        }
        cur_fn_name=n->fname;
        tc_stmts(&n->body,n->rettype,0);
        if(n->rettype&&n->rettype->kind!=TY_VOID&&!n->is_extern&&!block_returns(&n->body))
            warn("%s:%d: control reaches end of non-void function '%s'",n->file,n->line,n->fname);
        cur_fn_name=NULL;
        ntvars=saved_ntvars;/* pop function scope */
        scope_base=0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   CODE GENERATOR — x86-32 GAS AT&T, cdecl, int 0x80
   ═══════════════════════════════════════════════════════════════════════ */

static char  *out_buf=NULL;
static size_t out_len=0,out_cap=0;

static void out(const char *fmt,...){
    char tmp[4096];va_list ap;va_start(ap,fmt);int n=vsnprintf(tmp,sizeof tmp,fmt,ap);va_end(ap);
    if(out_len+n+1>out_cap){
        out_cap=out_cap?out_cap*2:65536;
        while(out_cap<out_len+n+1)out_cap*=2;
        out_buf=realloc(out_buf,out_cap);
    }
    memcpy(out_buf+out_len,tmp,n);out_len+=n;out_buf[out_len]=0;
}

static char *str_lits[8192];
static int   nstr_lits=0;
static int add_strlit(const char *s){
    for(int i=0;i<nstr_lits;i++)if(strcmp(str_lits[i],s)==0)return i;
    str_lits[nstr_lits]=xstrdup(s);return nstr_lits++;
}
typedef struct{double val;int is_double;}FLit;
static FLit flits[1024];
static int  nflits=0;
static int add_flit(double v,int is_double){
    for(int i=0;i<nflits;i++)if(flits[i].val==v&&flits[i].is_double==is_double)return i;
    flits[nflits].val=v;flits[nflits].is_double=is_double;return nflits++;
}

typedef struct{char *name;int offset;TypeRef *type;}Var;
static Var  vars[4096];
static int  nvars=0,frame_sz=0,lbl_cnt=0;


static char break_lbl[64],cont_lbl[64];
static int  has_std=0,freestanding=0;

/* x86-64 System V AMD64 ABI integer argument registers (in order) */
static const char *argregs64[6]={"rdi","rsi","rdx","rcx","r8","r9"};

static int   new_label(void){return lbl_cnt++;}
static Var gvar_proxy; /* scratch slot to return a fake Var for globals */
static Var  *find_var(const char *name){
    for(int i=nvars-1;i>=0;i--)if(strcmp(vars[i].name,name)==0)return &vars[i];
    /* fall back to globals — use sentinel offset -999999 */
    for(int i=0;i<ngvars;i++)if(strcmp(gvars[i].name,name)==0){
        gvar_proxy.name=gvars[i].name;gvar_proxy.offset=-999999-i;gvar_proxy.type=gvars[i].type;
        return &gvar_proxy;
    }
    return NULL;
}
static int alloc_var(const char *name,TypeRef *type){
    int sz;
    if(arch==ARCH_X86_64){
        /* on 64-bit every slot is 8 bytes (simplest, always aligned) */
        sz=8;
    } else {
        /* long/double occupy 8 bytes; float 4 bytes; everything else 4 bytes */
        sz=(type&&(type->kind==TY_LONG||type->kind==TY_DOUBLE))?8:4;
    }
    frame_sz+=sz;
    vars[nvars].name=xstrdup(name);
    vars[nvars].offset=-frame_sz;
    vars[nvars].type=type;
    nvars++;
    return -frame_sz;
}

static void gen_expr(Node *n);
static void gen_stmt(Node *n);

/* ── long (64-bit) helpers emitted once into the output ──────────── */
static int long_helpers_emitted=0;
static void need_long_helpers(void){
    if(long_helpers_emitted)return;
    long_helpers_emitted=1;
    /* These helpers may be emitted mid-function (wherever the first long
       operation is first compiled), so jump over their bodies — otherwise
       straight-line code falls through into _flr_ladd and hits its `ret`
       with no call on the stack. */
    out("    jmp .Lflr_long_helpers_end\n");
    /* _flr_ladd(alo,ahi,blo,bhi) -> edx:eax */
    out("_flr_ladd:\n");
    out("    movl 4(%%esp),%%eax\n    movl 8(%%esp),%%edx\n");
    out("    addl 12(%%esp),%%eax\n    adcl 16(%%esp),%%edx\n    ret\n\n");
    /* _flr_lsub */
    out("_flr_lsub:\n");
    out("    movl 4(%%esp),%%eax\n    movl 8(%%esp),%%edx\n");
    out("    subl 12(%%esp),%%eax\n    sbbl 16(%%esp),%%edx\n    ret\n\n");
    /* _flr_lmul (lo word only via imul trick) */
    out("_flr_lmul:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    movl 8(%%ebp),%%eax\n    movl 12(%%ebp),%%ecx\n"); /* alo, ahi */
    out("    movl 16(%%ebp),%%edx\n    movl 20(%%ebp),%%esi\n"); /* blo, bhi */
    /* result_hi = alo*bhi + ahi*blo + (alo*blo)>>32 */
    out("    pushl %%esi\n    pushl %%ecx\n    pushl %%edx\n    pushl %%eax\n");
    out("    imull %%edx,%%ecx\n"); /* ahi*blo -> ecx */
    out("    imull %%eax,%%esi\n"); /* alo*bhi -> esi */
    out("    mull %%edx\n");        /* alo*blo -> edx:eax */
    out("    addl %%ecx,%%edx\n    addl %%esi,%%edx\n");
    out("    addl $16,%%esp\n    leave\n    ret\n\n");
    /* _flr_lshl(alo,ahi,cntlo,cnthi) -> edx:eax — 64-bit left shift by a
       variable 0-63 count, using the standard shld+shl double-shift
       technique (count_hi is ignored: only the low 6 bits of the count
       matter, same as any x86 shift). */
    out("_flr_lshl:\n");
    out("    movl 4(%%esp),%%eax\n    movl 8(%%esp),%%edx\n    movl 12(%%esp),%%ecx\n");
    out("    andl $63,%%ecx\n");
    out("    shldl %%cl,%%eax,%%edx\n    shll %%cl,%%eax\n");
    out("    testb $32,%%cl\n    je .Llshl_done\n");
    out("    movl %%eax,%%edx\n    xorl %%eax,%%eax\n");
    out(".Llshl_done:\n    ret\n\n");
    /* _flr_lshr(alo,ahi,cntlo,cnthi) -> edx:eax — 64-bit ARITHMETIC right
       shift (Falcon's `long` is signed), same variable-count technique. */
    out("_flr_lshr:\n");
    out("    movl 4(%%esp),%%eax\n    movl 8(%%esp),%%edx\n    movl 12(%%esp),%%ecx\n");
    out("    andl $63,%%ecx\n");
    out("    shrdl %%cl,%%edx,%%eax\n    sarl %%cl,%%edx\n");
    out("    testb $32,%%cl\n    je .Llshr_done\n");
    out("    movl %%edx,%%eax\n    sarl $31,%%edx\n");
    out(".Llshr_done:\n    ret\n\n");
    /* _flr_udivmod64: edx:eax / ecx:ebx (both unsigned) -> quotient edx:eax,
       remainder esi:edi.  Plain 64-step shift-subtract; clobbers ecx/ebx
       only by reading them. */
    out("_flr_udivmod64:\n");
    out("    xorl %%esi,%%esi\n    xorl %%edi,%%edi\n    pushl $64\n");
    out(".Ludm_l:\n    shll $1,%%eax\n    rcll $1,%%edx\n    rcll $1,%%edi\n    rcll $1,%%esi\n");
    out("    cmpl %%ecx,%%esi\n    jb .Ludm_s\n    ja .Ludm_sub\n    cmpl %%ebx,%%edi\n    jb .Ludm_s\n");
    out(".Ludm_sub:\n    subl %%ebx,%%edi\n    sbbl %%ecx,%%esi\n    orl $1,%%eax\n");
    out(".Ludm_s:\n    decl (%%esp)\n    jne .Ludm_l\n    addl $4,%%esp\n    ret\n\n");
    /* _flr_ldiv / _flr_lmod (alo,ahi,blo,bhi): signed, truncating toward zero;
       remainder takes the dividend's sign (same as C). */
    for(int rem=0;rem<2;rem++){
        const char *nm=rem?"lmod":"ldiv";
        out("_flr_%s:\n",nm);
        out("    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%ebx\n    pushl %%esi\n    pushl %%edi\n    subl $8,%%esp\n");
        out("    movl 8(%%ebp),%%eax\n    movl 12(%%ebp),%%edx\n    movl 16(%%ebp),%%ebx\n    movl 20(%%ebp),%%ecx\n");
        out("    movl %%edx,-16(%%ebp)\n    movl %%ecx,-20(%%ebp)\n");
        /* divisor == 0: raise #DE (SIGFPE), matching the 64-bit path */
        out("    movl %%ebx,%%esi\n    orl %%ecx,%%esi\n    jnz .L%s_nz\n    xorl %%esi,%%esi\n    divl %%esi\n.L%s_nz:\n",nm,nm);
        out("    testl %%edx,%%edx\n    jns .L%s_a\n    negl %%eax\n    adcl $0,%%edx\n    negl %%edx\n.L%s_a:\n",nm,nm);
        out("    testl %%ecx,%%ecx\n    jns .L%s_b\n    negl %%ebx\n    adcl $0,%%ecx\n    negl %%ecx\n.L%s_b:\n",nm,nm);
        out("    call _flr_udivmod64\n");
        if(rem){
            out("    movl %%edi,%%eax\n    movl %%esi,%%edx\n    cmpl $0,-16(%%ebp)\n    jge .L%s_d\n",nm);
        }else{
            out("    movl -16(%%ebp),%%ecx\n    xorl -20(%%ebp),%%ecx\n    jns .L%s_d\n",nm);
        }
        out("    negl %%eax\n    adcl $0,%%edx\n    negl %%edx\n.L%s_d:\n",nm);
        out("    addl $8,%%esp\n    popl %%edi\n    popl %%esi\n    popl %%ebx\n    leave\n    ret\n\n");
    }
    /* _flr_lprint(lo,hi) — print signed 64-bit integer */
    out("_flr_lprint:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    out("    movl 8(%%ebp),%%eax\n    movl 12(%%ebp),%%edx\n"); /* lo, hi */
    /* negative? */
    out("    testl %%edx,%%edx\n    jge .Llpr_pos\n");
    out("    negl %%eax\n    adcl $0,%%edx\n    negl %%edx\n"); /* negate 64-bit */
    out("    movl $1,%%esi\n    jmp .Llpr_go\n");
    out(".Llpr_pos:\n    xorl %%esi,%%esi\n");
    out(".Llpr_go:\n");
    /* convert to decimal using 64-bit division by 10 */
    out("    leal .Lflr_ibuf+23,%%edi\n    movb $10,(%%edi)\n    decl %%edi\n");
    out(".Llpr_l:\n");
    out("    pushl %%edx\n    pushl %%eax\n"); /* save hi:lo */
    out("    movl $10,%%ecx\n");
    /* divide 64-bit by 10: use double division */
    out("    xorl %%edx,%%edx\n    movl 4(%%esp),%%eax\n    divl %%ecx\n"); /* hi/10 */
    out("    movl %%eax,%%ebx\n");             /* quotient hi */
    out("    movl (%%esp),%%eax\n    divl %%ecx\n"); /* lo/10, edx=rem */
    out("    popl %%ecx\n    popl %%ecx\n");   /* discard saved */
    out("    addb $48,%%dl\n    movb %%dl,(%%edi)\n    decl %%edi\n");
    /* Loop-termination check must be non-destructive: `orl %ebx,%eax` looks
       like a harmless zero-test but it's a real OR — it overwrites eax with
       the OR result, corrupting the quotient_lo value that must carry into
       the next iteration as the new "lo". This was invisible for any long
       value small enough that the high word (and thus ebx) stayed zero for
       the whole computation — ORing with 0 is a no-op — which is every
       value the existing test suite happened to use. It corrupts any value
       that actually spans into the high 32 bits. edx is safe to use as a
       throwaway copy here: its digit value was already written to memory
       above, and it's unconditionally overwritten by `movl %ebx,%edx` right
       below if the loop continues, or left unused if it doesn't. */
    out("    movl %%eax,%%edx\n    orl %%ebx,%%edx\n    je .Llpr_done\n    movl %%ebx,%%edx\n");
    out("    jmp .Llpr_l\n");
    out(".Llpr_done:\n");
    out("    testl %%esi,%%esi\n    je .Llpr_nomin\n");
    out("    movb $45,(%%edi)\n    decl %%edi\n");
    out(".Llpr_nomin:\n    incl %%edi\n");
    out("    leal .Lflr_ibuf+24,%%edx\n    subl %%edi,%%edx\n    movl %%edi,%%ecx\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    int $0x80\n");
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");
    out(".Lflr_long_helpers_end:\n");
}

/* push a long variable: pushes hi then lo (so lo is at lower address on stack) */

/* ── hardware intrinsics ──────────────────────────────────────────── */
/* ── x86-32 value representation ────────────────────────────────────
   int/bool/ptr/str/struct/float(bit pattern) : eax
   long / double(bit pattern)                 : edx:eax
   The helpers below convert between those representations and the x87. */
static TypeRef *cur_ret_type=NULL;   /* return type of the function being generated */

enum{K_I=0,K_L=1,K_F=2,K_D=3};
static int kcls(TypeRef *t){
    if(!t)return K_I;
    if(t->kind==TY_LONG)  return K_L;
    if(t->kind==TY_FLOAT) return K_F;
    if(t->kind==TY_DOUBLE)return K_D;
    return K_I;
}
static int slot32(TypeRef *t){int k=kcls(t);return (k==K_L||k==K_D)?8:4;}

/* push the value currently in eax / edx:eax onto the CPU stack in its native size */
static void spill32(TypeRef *t){
    if(slot32(t)==8)out("    pushl %%edx\n    pushl %%eax\n");
    else            out("    pushl %%eax\n");
}
/* x87 load of a native-typed value that is sitting at (%esp) */
static void fld_mem32(TypeRef *t){
    switch(kcls(t)){
    case K_L:out("    fildll (%%esp)\n");break;
    case K_F:out("    flds (%%esp)\n");break;
    case K_D:out("    fldl (%%esp)\n");break;
    default: out("    fildl (%%esp)\n");break;   /* NB: `filds` is the 16-bit load */
    }
}
/* x87 load of the native-typed value currently in the registers */
static void fld_reg32(TypeRef *t){
    spill32(t);fld_mem32(t);out("    addl $%d,%%esp\n",slot32(t));
}
/* pop st(0) into the registers as a float / double bit pattern */
static void fstp_reg32(int to_double){
    if(to_double)out("    subl $8,%%esp\n    fstpl (%%esp)\n    popl %%eax\n    popl %%edx\n");
    else         out("    subl $4,%%esp\n    fstps (%%esp)\n    popl %%eax\n");
}
/* pop st(0) into the registers as an int (to_long=0) or long (to_long=1),
   truncating toward zero like a C cast */
static void fistp_trunc_reg32(int to_long){
    out("    subl $16,%%esp\n    fnstcw 8(%%esp)\n    movzwl 8(%%esp),%%eax\n");
    out("    orl $0x0c00,%%eax\n    movw %%ax,10(%%esp)\n    fldcw 10(%%esp)\n");
    out(to_long?"    fistpll (%%esp)\n":"    fistpl (%%esp)\n");
    out("    fldcw 8(%%esp)\n    movl (%%esp),%%eax\n");
    if(to_long)out("    movl 4(%%esp),%%edx\n");
    out("    addl $16,%%esp\n");
}
/* convert the value in the registers from type `from` to type `to` */
static void gen_convert32(TypeRef *from,TypeRef *to){
    int f=kcls(from),t=kcls(to);
    if(f==t)return;
    if(f==K_I&&t==K_L){out("    cdq\n");return;}
    if(f==K_L&&t==K_I)return;                       /* low word is already in eax */
    if(t==K_F||t==K_D){fld_reg32(from);fstp_reg32(t==K_D);return;}
    /* float/double -> int/long */
    fld_reg32(from);fistp_trunc_reg32(t==K_L);
}

static void gen_intrinsic(Node *n){
    const char *nm=n->callee;
    if(strcmp(nm,"__syscall")==0){
        int argc=n->args.n;
        if(argc<1)die("__syscall: need at least syscall number");
        out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
        for(int i=argc-1;i>=0;i--){gen_expr(n->args.d[i]);out("    pushl %%eax\n");}
        out("    popl %%eax\n");
        if(argc>1)out("    popl %%ebx\n");else out("    xorl %%ebx,%%ebx\n");
        if(argc>2)out("    popl %%ecx\n");else out("    xorl %%ecx,%%ecx\n");
        if(argc>3)out("    popl %%edx\n");else out("    xorl %%edx,%%edx\n");
        if(argc>4)out("    popl %%esi\n");else out("    xorl %%esi,%%esi\n");
        if(argc>5)out("    popl %%edi\n");else out("    xorl %%edi,%%edi\n");
        out("    int $0x80\n");
        out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n");
        return;
    }
    if(strcmp(nm,"__inb")==0){
        if(n->args.n!=1)die("__inb takes 1 argument");
        gen_expr(n->args.d[0]);
        out("    movw %%ax,%%dx\n    xorl %%eax,%%eax\n    inb %%dx,%%al\n    movzbl %%al,%%eax\n");
        return;
    }
    if(strcmp(nm,"__outb")==0){
        if(n->args.n!=2)die("__outb takes 2 arguments");
        gen_expr(n->args.d[1]);out("    pushl %%eax\n");
        gen_expr(n->args.d[0]);out("    movw %%ax,%%dx\n");
        out("    popl %%eax\n    outb %%al,%%dx\n    xorl %%eax,%%eax\n");
        return;
    }
    if(strcmp(nm,"__cli")==0){out("    cli\n    xorl %%eax,%%eax\n");return;}
    if(strcmp(nm,"__sti")==0){out("    sti\n    xorl %%eax,%%eax\n");return;}
    if(strcmp(nm,"__hlt")==0){out("    hlt\n    xorl %%eax,%%eax\n");return;}
    if(strcmp(nm,"__rdtsc")==0){out("    rdtsc\n");return;}
    if(strcmp(nm,"__peek")==0){
        if(n->args.n!=1)die("__peek takes 1 argument");
        gen_expr(n->args.d[0]);out("    movl (%%eax),%%eax\n");return;
    }
    if(strcmp(nm,"__poke")==0){
        if(n->args.n!=2)die("__poke takes 2 arguments");
        gen_expr(n->args.d[1]);out("    pushl %%eax\n");
        gen_expr(n->args.d[0]);out("    popl %%ecx\n    movl %%ecx,(%%eax)\n    xorl %%eax,%%eax\n");return;
    }
    if(strcmp(nm,"__peekb")==0){
        if(n->args.n!=1)die("__peekb takes 1 argument");
        gen_expr(n->args.d[0]);out("    movzbl (%%eax),%%eax\n");return;
    }
    if(strcmp(nm,"__pokeb")==0){
        if(n->args.n!=2)die("__pokeb takes 2 arguments");
        gen_expr(n->args.d[1]);out("    pushl %%eax\n");
        gen_expr(n->args.d[0]);out("    popl %%ecx\n    movb %%cl,(%%eax)\n    xorl %%eax,%%eax\n");return;
    }
    if(strcmp(nm,"__memset")==0){
        if(n->args.n!=3)die("__memset takes 3 arguments");
        gen_expr(n->args.d[2]);out("    pushl %%eax\n");
        gen_expr(n->args.d[1]);out("    pushl %%eax\n");
        gen_expr(n->args.d[0]);out("    pushl %%eax\n");
        out("    call _flr_memset\n    addl $12,%%esp\n");return;
    }
    if(strcmp(nm,"__memcpy")==0){
        if(n->args.n!=3)die("__memcpy takes 3 arguments");
        gen_expr(n->args.d[2]);out("    pushl %%eax\n");
        gen_expr(n->args.d[1]);out("    pushl %%eax\n");
        gen_expr(n->args.d[0]);out("    pushl %%eax\n");
        out("    call _flr_memcpy\n    addl $12,%%esp\n");return;
    }
    die("unknown intrinsic '%s'",nm);
}

/* ── expression codegen ─────────────────────────────────────────── */
static void gen_expr(Node *n){
    if(!n){out("    xorl %%eax,%%eax\n");return;}
    switch(n->kind){
    case N_INTLIT:
        out("    movl $%lld,%%eax\n",n->ival);break;
    case N_LONGLIT:
        /* result in edx:eax */
        out("    movl $%lld,%%eax\n",(int)(n->ival&0xFFFFFFFFLL));
        out("    movl $%lld,%%edx\n",(int)(n->ival>>32));
        break;
    case N_BOOLLIT:
        out("    movl $%d,%%eax\n",n->bval?1:0);break;
    case N_FLOATLIT:{
        /* the IEEE-754 bit pattern is just an immediate in eax */
        union{float f;unsigned u;}cv;cv.f=(float)n->dval;
        out("    movl $%u,%%eax\n",cv.u);break;
    }
    case N_DOUBLELIT:{
        union{double d;unsigned u[2];}cv;cv.d=n->dval;
        out("    movl $%u,%%eax\n    movl $%u,%%edx\n",cv.u[0],cv.u[1]);break;
    }
    case N_STRLIT:
        out("    leal .Lstr%d,%%eax\n",add_strlit(n->sval));break;
    case N_IDENT:{
        Var *v=find_var(n->name);
        if(!v)die("%s:%d: undefined variable '%s'",n->file,n->line,n->name);
        int gidx=(v->offset<=-999999)?(-999999-v->offset):-1;
        if(gidx>=0){
            /* global variable */
            out("    movl _gv_%s,%%eax\n",gvars[gidx].name);
            if(slot32(v->type)==8)
                out("    movl _gv_%s+4,%%edx\n",gvars[gidx].name);
        } else {
            out("    movl %d(%%ebp),%%eax\n",v->offset);
            if(slot32(v->type)==8)
                out("    movl %d(%%ebp),%%edx\n",v->offset+4);
        }
        break;
    }
    case N_BINOP:{
        const char *op=n->op;
        /* `and` / `or` short-circuit: the right operand is only evaluated
           when the left one doesn't already decide the result. */
        if(strcmp(op,"and")==0||strcmp(op,"or")==0){
            int is_and=(op[0]=='a');
            int Lsc=new_label();
            gen_expr(n->left);
            if(n->left->etype&&n->left->etype->kind==TY_LONG)out("    orl %%edx,%%eax\n");
            else out("    testl %%eax,%%eax\n");
            out(is_and?"    jz .Lscs%d\n":"    jnz .Lscs%d\n",Lsc);
            gen_expr(n->right);
            if(n->right->etype&&n->right->etype->kind==TY_LONG)out("    orl %%edx,%%eax\n");
            else out("    testl %%eax,%%eax\n");
            out(is_and?"    jz .Lscs%d\n":"    jnz .Lscs%d\n",Lsc);
            out("    movl $%d,%%eax\n    jmp .Lsce%d\n",is_and?1:0,Lsc);
            out(".Lscs%d:\n    movl $%d,%%eax\n.Lsce%d:\n",Lsc,is_and?0:1,Lsc);
            break;
        }
        int is_long=(n->etype&&n->etype->kind==TY_LONG);
        int llong=(n->left&&n->left->etype&&n->left->etype->kind==TY_LONG);
        int rlong=(n->right&&n->right->etype&&n->right->etype->kind==TY_LONG);

        /* str + str → str_concat */
        if(strcmp(op,"+")==0&&n->left->etype&&n->left->etype->kind==TY_STR){
            gen_expr(n->left);out("    pushl %%eax\n");
            gen_expr(n->right);out("    popl %%ecx\n    pushl %%eax\n    pushl %%ecx\n");
            out("    call _flr_str_concat\n    addl $8,%%esp\n");
            break;
        }
        /* float/double arithmetic via x87 */
        int is_fp=(n->etype&&(n->etype->kind==TY_FLOAT||n->etype->kind==TY_DOUBLE));
        int lf=(n->left&&n->left->etype&&(n->left->etype->kind==TY_FLOAT||n->left->etype->kind==TY_DOUBLE));
        int rf=(n->right&&n->right->etype&&(n->right->etype->kind==TY_FLOAT||n->right->etype->kind==TY_DOUBLE));
        if(is_fp||lf||rf){
            /* The right operand is spilled to the CPU stack in its native
               form while the left is evaluated, so the 8-entry x87 stack
               never holds more than two values no matter how deeply the
               expression nests. Left ends up in st(0) and right in st(1)
               (or the reverse for < and <=, see below). */
            TypeRef *lt=n->left->etype,*rt=n->right->etype;
            int is_cmp=(!strcmp(op,"==")||!strcmp(op,"!=")||!strcmp(op,"<")||!strcmp(op,">")||!strcmp(op,"<=")||!strcmp(op,">="));
            int swap=(!strcmp(op,"<")||!strcmp(op,"<="));
            gen_expr(n->left);spill32(lt);
            gen_expr(n->right);
            if(!swap){
                fld_reg32(rt);                                          /* st0=right */
                fld_mem32(lt);out("    addl $%d,%%esp\n",slot32(lt));  /* st0=left, st1=right */
            }else{
                fld_mem32(lt);out("    addl $%d,%%esp\n",slot32(lt));  /* st0=left */
                fld_reg32(rt);                                          /* st0=right, st1=left */
            }
            if(is_cmp){
                /* fucomip sets CF/ZF/PF like an unsigned compare of st0 with
                   st1; an unordered result (NaN) sets all three. `<` and `<=`
                   are evaluated as the swapped `>` / `>=` so that NaN makes
                   them false (CF=1 fails seta/setae); == and != also look at PF. */
                out("    fucomip %%st(1),%%st\n    fstp %%st(0)\n");
                if     (!strcmp(op,"==")) out("    sete %%al\n    setnp %%cl\n    andb %%cl,%%al\n");
                else if(!strcmp(op,"!=")) out("    setne %%al\n    setp %%cl\n    orb %%cl,%%al\n");
                else if(!strcmp(op,"<")||!strcmp(op,">")) out("    seta %%al\n");
                else                                      out("    setae %%al\n");
                out("    movzbl %%al,%%eax\n");
                break;
            }
            /* st0=left, st1=right. AT&T `fsubp`/`fdivp` (no operands) compute st0-st1 and st0/st1. */
            if     (!strcmp(op,"+")) out("    faddp\n");
            else if(!strcmp(op,"-")) out("    fsubp\n");
            else if(!strcmp(op,"*")) out("    fmulp\n");
            else if(!strcmp(op,"/")) out("    fdivp\n");
            else if(!strcmp(op,"%")){
                /* C fmod: fprem leaves a truncated remainder (sign of the dividend);
                   repeat while C2 says the reduction is incomplete. */
                int L=new_label();
                out(".Lfmod%d:\n    fprem\n    fnstsw %%ax\n    testb $4,%%ah\n    jnz .Lfmod%d\n    fstp %%st(1)\n",L,L);
            }
            else die("%s:%d: operator '%s' is not defined for float/double",n->file,n->line,op);
            fstp_reg32(n->etype&&n->etype->kind==TY_DOUBLE);
            break;
        }

        if(is_long||llong||rlong){
            need_long_helpers();
            /* evaluate lhs first (left-to-right), park it on the stack, then
               evaluate rhs and re-push both so the helpers see lhs on top */
            gen_expr(n->left);
            if(llong){out("    pushl %%edx\n    pushl %%eax\n");}
            else     {out("    cdq\n    pushl %%edx\n    pushl %%eax\n");}
            gen_expr(n->right);
            if(!rlong)out("    cdq\n");
            out("    popl %%ecx\n    popl %%esi\n");          /* lhs lo,hi */
            out("    pushl %%edx\n    pushl %%eax\n");        /* rhs */
            out("    pushl %%esi\n    pushl %%ecx\n");        /* lhs on top */

            if(strcmp(op,"+")==0){
                out("    call _flr_ladd\n    addl $16,%%esp\n");
            }else if(strcmp(op,"-")==0){
                out("    call _flr_lsub\n    addl $16,%%esp\n");
            }else if(strcmp(op,"*")==0){
                out("    call _flr_lmul\n    addl $16,%%esp\n");
            }else if(strcmp(op,"<<")==0){
                out("    call _flr_lshl\n    addl $16,%%esp\n");
            }else if(strcmp(op,">>")==0){
                out("    call _flr_lshr\n    addl $16,%%esp\n");
            }else if(strcmp(op,"/")==0){
                out("    call _flr_ldiv\n    addl $16,%%esp\n");
            }else if(strcmp(op,"%")==0){
                out("    call _flr_lmod\n    addl $16,%%esp\n");
            }else if(!strcmp(op,"&")||!strcmp(op,"|")||!strcmp(op,"^")||!strcmp(op,"and")||!strcmp(op,"or")){
                out("    popl %%eax\n    popl %%edx\n"); /* lhs lo,hi */
                out("    popl %%ecx\n    popl %%esi\n"); /* rhs lo,hi */
                if(!strcmp(op,"&"))      out("    andl %%ecx,%%eax\n    andl %%esi,%%edx\n");
                else if(!strcmp(op,"|")) out("    orl %%ecx,%%eax\n    orl %%esi,%%edx\n");
                else if(!strcmp(op,"^")) out("    xorl %%ecx,%%eax\n    xorl %%esi,%%edx\n");
                else if(!strcmp(op,"and")){
                    out("    orl %%edx,%%eax\n    setne %%al\n    orl %%esi,%%ecx\n    setne %%cl\n");
                    out("    andb %%cl,%%al\n    movzbl %%al,%%eax\n    xorl %%edx,%%edx\n");
                }else{
                    out("    orl %%edx,%%eax\n    orl %%esi,%%ecx\n    orl %%ecx,%%eax\n    setne %%al\n");
                    out("    movzbl %%al,%%eax\n    xorl %%edx,%%edx\n");
                }
            }else{
                /* comparison: compare hi then lo */
                out("    popl %%eax\n    popl %%edx\n"); /* lhs lo,hi */
                out("    popl %%ecx\n    popl %%esi\n"); /* rhs lo,hi */
                out("    cmpl %%esi,%%edx\n"); /* compare hi */
                int l=new_label();
                if(strcmp(op,"==")==0){
                    out("    jne .Llcmp%d\n",l);
                    out("    cmpl %%ecx,%%eax\n");
                    out(".Llcmp%d:\n    sete %%al\n    movzbl %%al,%%eax\n",l);
                }else if(strcmp(op,"!=")==0){
                    out("    jne .Llcmp%d\n",l);
                    out("    cmpl %%ecx,%%eax\n");
                    out(".Llcmp%d:\n    setne %%al\n    movzbl %%al,%%eax\n",l);
                }else if(strcmp(op,"<")==0){
                    int d=new_label(),z=new_label();
                    out("    jl .Llcmp%d\n    jg .Llcmp%d\n",l,z);
                    out("    cmpl %%ecx,%%eax\n    setb %%al\n    movzbl %%al,%%eax\n    jmp .Llcmp%d\n",d);
                    out(".Llcmp%d:\n    movl $1,%%eax\n    jmp .Llcmp%d\n.Llcmp%d:\n    xorl %%eax,%%eax\n.Llcmp%d:\n",l,d,z,d);
                }else if(strcmp(op,">")==0){
                    int d=new_label(),z=new_label();
                    out("    jg .Llcmp%d\n    jl .Llcmp%d\n",l,z);
                    out("    cmpl %%ecx,%%eax\n    seta %%al\n    movzbl %%al,%%eax\n    jmp .Llcmp%d\n",d);
                    out(".Llcmp%d:\n    movl $1,%%eax\n    jmp .Llcmp%d\n.Llcmp%d:\n    xorl %%eax,%%eax\n.Llcmp%d:\n",l,d,z,d);
                }else if(strcmp(op,"<=")==0){
                    int d=new_label(),z=new_label();
                    out("    jl .Llcmp%d\n    jg .Llcmp%d\n",l,z);
                    out("    cmpl %%ecx,%%eax\n    setbe %%al\n    movzbl %%al,%%eax\n    jmp .Llcmp%d\n",d);
                    out(".Llcmp%d:\n    movl $1,%%eax\n    jmp .Llcmp%d\n.Llcmp%d:\n    xorl %%eax,%%eax\n.Llcmp%d:\n",l,d,z,d);
                }else if(strcmp(op,">=")==0){
                    int d=new_label(),z=new_label();
                    out("    jg .Llcmp%d\n    jl .Llcmp%d\n",l,z);
                    out("    cmpl %%ecx,%%eax\n    setae %%al\n    movzbl %%al,%%eax\n    jmp .Llcmp%d\n",d);
                    out(".Llcmp%d:\n    movl $1,%%eax\n    jmp .Llcmp%d\n.Llcmp%d:\n    xorl %%eax,%%eax\n.Llcmp%d:\n",l,d,z,d);
                }else{
                    die("unsupported long operator '%s'",op);
                }
            }
            break;
        }

        /* normal 32-bit */
        gen_expr(n->left);out("    pushl %%eax\n");
        gen_expr(n->right);out("    movl %%eax,%%ecx\n    popl %%eax\n");
        if     (strcmp(op,"+")==0) out("    addl %%ecx,%%eax\n");
        else if(strcmp(op,"-")==0) out("    subl %%ecx,%%eax\n");
        else if(strcmp(op,"*")==0) out("    imull %%ecx,%%eax\n");
        else if(strcmp(op,"/")==0){out("    cdq\n");out("    idivl %%ecx\n");}
        else if(strcmp(op,"%")==0){out("    cdq\n");out("    idivl %%ecx\n");out("    movl %%edx,%%eax\n");}
        else if(strcmp(op,"&")==0) out("    andl %%ecx,%%eax\n");
        else if(strcmp(op,"|")==0) out("    orl  %%ecx,%%eax\n");
        else if(strcmp(op,"^")==0) out("    xorl %%ecx,%%eax\n");
        else if(strcmp(op,"<<")==0)out("    shll %%cl,%%eax\n");
        else if(strcmp(op,">>")==0)out("    sarl %%cl,%%eax\n");
        else if(strcmp(op,"==")==0){out("    cmpl %%ecx,%%eax\n");out("    sete %%al\n");out("    movzbl %%al,%%eax\n");}
        else if(strcmp(op,"!=")==0){out("    cmpl %%ecx,%%eax\n");out("    setne %%al\n");out("    movzbl %%al,%%eax\n");}
        else if(strcmp(op,"<")==0) {out("    cmpl %%ecx,%%eax\n");out("    setl %%al\n");out("    movzbl %%al,%%eax\n");}
        else if(strcmp(op,">")==0) {out("    cmpl %%ecx,%%eax\n");out("    setg %%al\n");out("    movzbl %%al,%%eax\n");}
        else if(strcmp(op,"<=")==0){out("    cmpl %%ecx,%%eax\n");out("    setle %%al\n");out("    movzbl %%al,%%eax\n");}
        else if(strcmp(op,">=")==0){out("    cmpl %%ecx,%%eax\n");out("    setge %%al\n");out("    movzbl %%al,%%eax\n");}
        else if(strcmp(op,"and")==0){
            out("    testl %%eax,%%eax\n");out("    setne %%al\n");
            out("    testl %%ecx,%%ecx\n");out("    setne %%cl\n");
            out("    andb %%cl,%%al\n");out("    movzbl %%al,%%eax\n");
        }
        else if(strcmp(op,"or")==0){
            out("    orl %%ecx,%%eax\n");out("    setne %%al\n");out("    movzbl %%al,%%eax\n");
        }
        else die("unknown binop '%s'",op);
        break;
    }
    case N_UNOP:
        gen_expr(n->left);
        if(strcmp(n->op,"-")==0&&n->etype&&n->etype->kind==TY_LONG){
            out("    negl %%eax\n    adcl $0,%%edx\n    negl %%edx\n");
        }
        else if(strcmp(n->op,"-")==0&&n->etype&&n->etype->kind==TY_FLOAT){
            /* eax holds the raw 32-bit IEEE-754 bit pattern; flip the sign bit */
            out("    xorl $0x80000000,%%eax\n");
        }
        else if(strcmp(n->op,"-")==0&&n->etype&&n->etype->kind==TY_DOUBLE){
            /* edx:eax holds the raw 64-bit IEEE-754 bit pattern (edx = high dword);
               the sign bit lives in edx's top bit, eax is untouched */
            out("    xorl $0x80000000,%%edx\n");
        }
        else if     (strcmp(n->op,"-"  )==0)out("    negl %%eax\n");
        else if(strcmp(n->op,"~"  )==0){
            out("    notl %%eax\n");
            if(n->left->etype&&n->left->etype->kind==TY_LONG)out("    notl %%edx\n");
        }
        else if(strcmp(n->op,"not")==0){
            if(n->left->etype&&n->left->etype->kind==TY_LONG)out("    orl %%edx,%%eax\n");
            else out("    testl %%eax,%%eax\n");
            out("    sete %%al\n    movzbl %%al,%%eax\n");
        }
        break;
    case N_CALL:{
        if(is_intrinsic(n->callee)){gen_intrinsic(n);break;}
        if(has_std){
            /* print: dispatch on etype */
            if(strcmp(n->callee,"print")==0&&n->args.n==1){
                Node *arg=n->args.d[0];
                int is_long_arg=(arg->etype&&arg->etype->kind==TY_LONG);
                if(is_long_arg){
                    need_long_helpers();
                    gen_expr(arg);
                    out("    pushl %%edx\n    pushl %%eax\n");
                    out("    call _flr_lprint\n    addl $8,%%esp\n");
                    break;
                }
                int is_str2=(arg->etype&&arg->etype->kind==TY_STR);
                int is_fa=(arg->etype&&arg->etype->kind==TY_FLOAT);
                int is_da=(arg->etype&&arg->etype->kind==TY_DOUBLE);
                if(!is_fa&&!is_da&&arg->kind==N_IDENT){
                    Var *v=find_var(arg->name);
                    if(v&&v->type&&v->type->kind==TY_FLOAT)is_fa=1;
                    if(v&&v->type&&v->type->kind==TY_DOUBLE)is_da=1;
                }
                gen_expr(arg);
                if(is_fa){out("    pushl %%eax\n    call _flr_print_float\n    addl $4,%%esp\n");break;}
                if(is_da){out("    pushl %%edx\n    pushl %%eax\n    call _flr_print_double\n    addl $8,%%esp\n");break;}
                out("    pushl %%eax\n");
                out("    call %s\n    addl $4,%%esp\n",
                    is_str2?"_flr_print_str":"_flr_print_int");
                break;
            }
            if(strcmp(n->callee,"len")==0&&n->args.n==1){
                gen_expr(n->args.d[0]);out("    movl (%%eax),%%eax\n");break;
            }
            /* str_concat(a, b) -> str */
            if(strcmp(n->callee,"str_concat")==0&&n->args.n==2){
                gen_expr(n->args.d[1]);out("    pushl %%eax\n");
                gen_expr(n->args.d[0]);out("    pushl %%eax\n");
                out("    call _flr_str_concat\n    addl $8,%%esp\n");
                break;
            }
            /* str_format(fmt, ...) -> str */
            if(strcmp(n->callee,"str_format")==0&&n->args.n>=1){
                /* push args right-to-left then fmt last */
                for(int i=n->args.n-1;i>=1;i--){
                    gen_expr(n->args.d[i]);out("    pushl %%eax\n");
                }
                out("    pushl $%d\n",n->args.n-1); /* nargs */
                gen_expr(n->args.d[0]);out("    pushl %%eax\n");
                out("    call _flr_str_format\n    addl $%d,%%esp\n",(n->args.n+1)*4);
                break;
            }
        }
        {
            /* Arguments are converted to the callee's declared parameter
               types (int->double etc.) and long/double take 8 bytes. */
            FuncSig *sig=find_func(n->callee);
            int total=0;
            for(int i=n->args.n-1;i>=0;i--){
                Node *a=n->args.d[i];
                TypeRef *pt=(sig&&i<sig->params.n)?sig->params.d[i].type:a->etype;
                gen_expr(a);
                gen_convert32(a->etype,pt);
                spill32(pt);total+=slot32(pt);
            }
            out("    call %s\n",n->callee);
            if(total>0)out("    addl $%d,%%esp\n",total);
        }
        break;
    }
    case N_INDEX:{
        int esz=slot32(n->etype);   /* 8 for long/double elements, else 4 */
        gen_expr(n->right);out("    pushl %%eax\n");
        gen_expr(n->left); out("    popl %%ecx\n");
        out("    leal 8(%%eax,%%ecx,%d),%%ecx\n",esz);
        if(esz==8)out("    movl 4(%%ecx),%%edx\n");
        out("    movl (%%ecx),%%eax\n");break;
    }
    case N_FIELD:{
        gen_expr(n->left);
        int found_off=field_offset(n->left,n->sval,4);
        if(slot32(n->etype)==8)out("    movl %d(%%eax),%%edx\n",found_off+4);
        out("    movl %d(%%eax),%%eax\n",found_off);break;
    }
    case N_ARRAYLIT:{
        TypeRef *elt=(n->etype&&n->etype->kind==TY_ARRAY&&n->etype->elem)?n->etype->elem:NULL;
        int esz=slot32(elt);
        int cnt=n->elems.n,alloc_sz=8+cnt*esz;
        /* use sbrk(alloc_sz) via sys_brk trick: just call _flr_alloc */
        out("    pushl $%d\n",alloc_sz);
        out("    call _flr_alloc\n    addl $4,%%esp\n");
        out("    pushl %%eax\n");
        out("    movl $%d,(%%eax)\n    movl $%d,4(%%eax)\n",cnt,cnt);
        for(int i=0;i<cnt;i++){
            gen_expr(n->elems.d[i]);
            gen_convert32(n->elems.d[i]->etype,elt);
            /* reload the array base AFTER evaluating the element (a nested
               literal or call would have clobbered a cached register) */
            out("    movl (%%esp),%%ecx\n");
            out("    movl %%eax,%d(%%ecx)\n",8+i*esz);
            if(esz==8)out("    movl %%edx,%d(%%ecx)\n",8+i*esz+4);
        }
        out("    popl %%eax\n");break;
    }
    default:die("gen_expr: unhandled kind %d",n->kind);
    }
}

static void gen_store(Node *lv){
    /* the value to store is in eax (edx:eax for long/double), already
       converted to the lvalue's type */
    int w8=(slot32(lv->etype)==8);
    switch(lv->kind){
    case N_IDENT:{
        Var *v=find_var(lv->name);
        if(!v)die("%s:%d: undefined variable '%s'",lv->file,lv->line,lv->name);
        int gidx=(v->offset<=-999999)?(-999999-v->offset):-1;
        w8=(slot32(v->type)==8);
        if(gidx>=0){
            out("    movl %%eax,_gv_%s\n",gvars[gidx].name);
            if(w8)out("    movl %%edx,_gv_%s+4\n",gvars[gidx].name);
        } else {
            out("    movl %%eax,%d(%%ebp)\n",v->offset);
            if(w8)out("    movl %%edx,%d(%%ebp)\n",v->offset+4);
        }
        break;
    }
    case N_INDEX:
        if(w8)out("    pushl %%edx\n");
        out("    pushl %%eax\n");
        gen_expr(lv->right);out("    pushl %%eax\n");
        gen_expr(lv->left); out("    popl %%ecx\n");
        out("    leal 8(%%eax,%%ecx,%d),%%ecx\n",w8?8:4);
        out("    popl %%eax\n");
        if(w8)out("    popl %%edx\n    movl %%edx,4(%%ecx)\n");
        out("    movl %%eax,(%%ecx)\n");break;
    case N_FIELD:{
        if(w8)out("    pushl %%edx\n");
        out("    pushl %%eax\n");gen_expr(lv->left);
        int found_off=field_offset(lv->left,lv->sval,4);
        out("    popl %%ecx\n");
        if(w8)out("    popl %%edx\n    movl %%edx,%d(%%eax)\n",found_off+4);
        out("    movl %%ecx,%d(%%eax)\n",found_off);break;
    }
    default:die("gen_store: not an lvalue");
    }
}

static void gen_stmt(Node *n){
    if(!n)return;
    switch(n->kind){
    case N_IMPORT:if(strcmp(n->import_path,"std")==0)has_std=1;break;
    case N_VARDECL:
    case N_LETDECL:
    case N_CONSTDECL:{
        if(n->is_static){
            /* function-local static: backed by the hidden global slot
               registered during type-check (n->sval / n->ival index).
               No init code — a literal initializer is already baked
               into .data by emit_data; anything else starts at 0. */
            if(nvars>=4096)die("too many variables");
            vars[nvars].name=xstrdup(n->name);
            vars[nvars].offset=-999999-(int)n->ival;
            vars[nvars].type=n->typeref?n->typeref:(n->left&&n->left->etype?n->left->etype:mktype(TY_INT,NULL,NULL));
            nvars++;
            break;
        }
        TypeRef *vtype=n->typeref;
        /* infer type from initializer if not declared */
        if(!vtype&&n->left&&n->left->etype)vtype=n->left->etype;
        int off=alloc_var(n->name,vtype);
        if(n->left){
            gen_expr(n->left);
            /* int->float, float->double, double->float, ... into the declared type */
            gen_convert32(n->left->etype,vtype);
            out("    movl %%eax,%d(%%ebp)\n",off);
            if(slot32(vtype)==8)out("    movl %%edx,%d(%%ebp)\n",off+4);
        }else{
            out("    movl $0,%d(%%ebp)\n",off);
            if(slot32(vtype)==8)
                out("    movl $0,%d(%%ebp)\n",off+4);
        }
        break;
    }
    case N_ASSIGN:{
        /* `x op= y` was rewritten to `x = x op y` by the type checker */
        gen_expr(n->right);
        gen_convert32(n->right->etype,n->left->etype);
        gen_store(n->left);
        break;
    }
    case N_RETURN:
        if(n->left){gen_expr(n->left);gen_convert32(n->left->etype,cur_ret_type);}
        else out("    xorl %%eax,%%eax\n");
        out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n");
        out("    leave\n    ret\n");break;
    case N_EXPRSTMT:{
        Node *e=n->left;
        if(has_std&&e->kind==N_CALL&&strcmp(e->callee,"print")==0){
            if(e->args.n!=1)die("print takes 1 argument");
            Node *arg=e->args.d[0];
            int is_long_arg=(arg->etype&&arg->etype->kind==TY_LONG);
            if(is_long_arg){
                need_long_helpers();
                gen_expr(arg);
                out("    pushl %%edx\n    pushl %%eax\n");
                out("    call _flr_lprint\n    addl $8,%%esp\n");
                break;
            }
            int is_str=(arg->etype&&arg->etype->kind==TY_STR);
            if(!is_str&&arg->kind==N_IDENT){Var *v=find_var(arg->name);if(v&&v->type&&v->type->kind==TY_STR)is_str=1;}
            int is_float_arg=(arg->etype&&arg->etype->kind==TY_FLOAT);
            int is_double_arg=(arg->etype&&arg->etype->kind==TY_DOUBLE);
            if(!is_float_arg&&!is_double_arg&&arg->kind==N_IDENT){
                Var *v=find_var(arg->name);
                if(v&&v->type&&v->type->kind==TY_FLOAT)is_float_arg=1;
                if(v&&v->type&&v->type->kind==TY_DOUBLE)is_double_arg=1;
            }
            if(is_float_arg||is_double_arg){
                gen_expr(arg);
                if(is_double_arg){
                    out("    pushl %%edx\n    pushl %%eax\n");
                    out("    call _flr_print_double\n    addl $8,%%esp\n");
                } else {
                    out("    pushl %%eax\n");
                    out("    call _flr_print_float\n    addl $4,%%esp\n");
                }
                break;
            }
            gen_expr(arg);out("    pushl %%eax\n");
            out("    call %s\n    addl $4,%%esp\n",is_str?"_flr_print_str":"_flr_print_int");
            break;
        }
        gen_expr(e);break;
    }
    case N_IF:{
        int lend=new_label(),lnext=new_label();
        gen_expr(n->cond);out("    testl %%eax,%%eax\n    je .Lif%d\n",lnext);
        {int sv=nvars;for(int i=0;i<n->body.n;i++)gen_stmt(n->body.d[i]);nvars=sv;}
        out("    jmp .Lif%d\n.Lif%d:\n",lend,lnext);
        for(int ei=0;ei<n->elifs.n;ei++){
            ElifClause *ec=&n->elifs.d[ei];int ln2=new_label();
            gen_expr(ec->cond);out("    testl %%eax,%%eax\n    je .Lif%d\n",ln2);
            {int sv=nvars;for(int i=0;i<ec->body.n;i++)gen_stmt(ec->body.d[i]);nvars=sv;}
            out("    jmp .Lif%d\n.Lif%d:\n",lend,ln2);
        }
        {int sv=nvars;for(int i=0;i<n->else_body.n;i++)gen_stmt(n->else_body.d[i]);nvars=sv;}
        out(".Lif%d:\n",lend);break;
    }
    case N_WHILE:{
        int ls=new_label(),le=new_label();
        char ob[64],oc[64];strcpy(ob,break_lbl);strcpy(oc,cont_lbl);
        snprintf(break_lbl,64,".Lwh%d",le);snprintf(cont_lbl,64,".Lwh%d",ls);
        out(".Lwh%d:\n",ls);gen_expr(n->cond);out("    testl %%eax,%%eax\n    je .Lwh%d\n",le);
        {int sv=nvars;for(int i=0;i<n->body.n;i++)gen_stmt(n->body.d[i]);nvars=sv;}
        out("    jmp .Lwh%d\n.Lwh%d:\n",ls,le);
        strcpy(break_lbl,ob);strcpy(cont_lbl,oc);break;
    }
    case N_FOR:{
        int ls=new_label(),le=new_label(),lp=new_label();
        char ob[64],oc[64];strcpy(ob,break_lbl);strcpy(oc,cont_lbl);
        snprintf(break_lbl,64,".Lfor%d",le);snprintf(cont_lbl,64,".Lfor%d",lp);
        {int sv=nvars;gen_stmt(n->for_init);
        out(".Lfor%d:\n",ls);gen_expr(n->cond);out("    testl %%eax,%%eax\n    je .Lfor%d\n",le);
        for(int i=0;i<n->body.n;i++)gen_stmt(n->body.d[i]);
        out(".Lfor%d:\n",lp);gen_stmt(n->for_post);
        out("    jmp .Lfor%d\n.Lfor%d:\n",ls,le);nvars=sv;}
        strcpy(break_lbl,ob);strcpy(cont_lbl,oc);break;
    }
    case N_BREAK:
        if(!break_lbl[0])die("%s:%d: break outside loop",n->file,n->line);
        out("    jmp %s\n",break_lbl);break;
    case N_CONTINUE:
        if(!cont_lbl[0])die("%s:%d: continue outside loop",n->file,n->line);
        out("    jmp %s\n",cont_lbl);break;
    default:die("gen_stmt: unhandled kind %d",n->kind);
    }
}

static void gen_func(Node *fn){
    nvars=0;frame_sz=0;/* lbl_cnt is global, not reset per function */
    memset(break_lbl,0,sizeof break_lbl);memset(cont_lbl,0,sizeof cont_lbl);
    cur_ret_type=fn->rettype;
    {
        int poff=8;
        for(int i=0;i<fn->params.n;i++){
            Var *v=&vars[nvars++];
            v->name=xstrdup(fn->params.d[i].name);
            v->offset=poff;
            v->type=fn->params.d[i].type;
            poff+=slot32(v->type);   /* long/double arguments occupy 8 bytes */
        }
    }
    size_t body_start=out_len;
    for(int i=0;i<fn->body.n;i++)gen_stmt(fn->body.d[i]);
    size_t body_len=out_len-body_start;
    char *body_asm=malloc(body_len+1);
    memcpy(body_asm,out_buf+body_start,body_len);body_asm[body_len]=0;
    out_len=body_start;out_buf[out_len]=0;

    int fsz=(frame_sz+15)&~15;
    out("%s %s\n%s:\n",fn->is_static?".local":".globl",fn->fname,fn->fname);
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    if(fsz>0)out("    subl $%d,%%esp\n",fsz);
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");

    if(out_len+body_len+1>out_cap){out_cap=out_cap*2+body_len+64;out_buf=realloc(out_buf,out_cap);}
    memcpy(out_buf+out_len,body_asm,body_len);out_len+=body_len;out_buf[out_len]=0;free(body_asm);

    if(strcmp(fn->fname,"main")==0){
        if(!freestanding){
            out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n");
            out("    xorl %%ebx,%%ebx\n    movl $1,%%eax\n    int $0x80\n");
        }else{
            out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n");
            out("    cli\n.Lhlt_loop:\n    hlt\n    jmp .Lhlt_loop\n");
        }
    }else{
        out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n");
        out("    leave\n    ret\n");
    }
    out("\n");
}

/* ── runtime (inlined when hosted + has_std) ────────────────────── */
static void emit_runtime(void){
    /* The ELF entry point must exist for any hosted (non-freestanding)
       program regardless of whether it uses Falcon's virtual std runtime —
       has_std only controls whether the runtime *helper* functions below
       (print_int, memcpy, etc.) get emitted. A project supplying its own
       std.fl (has_std==0, freestanding==0) still needs _start, or the
       linker has no entry point and the binary can't run at all.
       Freestanding programs supply their own _start via an external boot
       stub (see examples/EXAMPLE-OS/boot.s), so this must stay silent then. */
    if(!freestanding){
        out(".globl _start\n_start:\n");
        out("    xorl %%ebp,%%ebp\n");
        if(has_std){
            /* _flr_alloc only exists when the virtual std runtime is
               emitted below (has_std). Build a Falcon-shaped argv array
               (8-byte header + 4-byte elements) from the kernel's raw
               argc/argv and pass it to main. Without has_std there is no
               allocator available yet, so fall back to the plain call. */
            out("    movl (%%esp),%%esi\n");        /* esi = argc */
            out("    leal 4(%%esp),%%edi\n");        /* edi = &argv[0] */
            out("    movl %%esi,%%eax\n");
            out("    imull $4,%%eax,%%eax\n");
            out("    addl $8,%%eax\n");
            out("    pushl %%eax\n");
            out("    call _flr_alloc\n");
            out("    addl $4,%%esp\n");
            out("    movl %%eax,%%ebx\n");           /* ebx = argv array ptr */
            out("    movl %%esi,(%%ebx)\n");
            out("    movl %%esi,4(%%ebx)\n");
            out("    xorl %%ecx,%%ecx\n");
            out(".Lflr_argv_copy:\n");
            out("    cmpl %%esi,%%ecx\n");
            out("    jge .Lflr_argv_done\n");
            out("    movl (%%edi,%%ecx,4),%%edx\n");
            out("    movl %%edx,8(%%ebx,%%ecx,4)\n");
            out("    incl %%ecx\n");
            out("    jmp .Lflr_argv_copy\n");
            out(".Lflr_argv_done:\n");
            out("    pushl %%ebx\n");
            out("    pushl %%esi\n");
            out("    call main\n");
            out("    addl $8,%%esp\n");
        } else {
            out("    call main\n");
        }
        out("    movl %%eax,%%ebx\n");
        out("    movl $1,%%eax\n");
        out("    int $0x80\n\n");
    }
    if(!has_std||freestanding)return;
    out("# ── Falcon Runtime (inline) ───────────────────────────\n");
    /* memset / memcpy */
    out("_flr_memset:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%edi\n");
    out("    movl 8(%%ebp),%%edi\n    movl 12(%%ebp),%%eax\n    movl 16(%%ebp),%%ecx\n");
    out("    rep stosb\n    popl %%edi\n    leave\n    ret\n\n");
    out("_flr_memcpy:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%esi\n    pushl %%edi\n");
    out("    movl 8(%%ebp),%%edi\n    movl 12(%%ebp),%%esi\n    movl 16(%%ebp),%%ecx\n");
    out("    rep movsb\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* print_str */
    out("_flr_print_str:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%esi\n");
    out("    movl 8(%%ebp),%%esi\n    movl %%esi,%%ecx\n");
    out(".Lfps_l:\n    cmpb $0,(%%ecx)\n    je .Lfps_d\n    incl %%ecx\n    jmp .Lfps_l\n");
    out(".Lfps_d:\n    subl %%esi,%%ecx\n    movl %%ecx,%%edx\n    movl %%esi,%%ecx\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    int $0x80\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    leal .Lflr_nl,%%ecx\n    movl $1,%%edx\n    int $0x80\n");
    out("    popl %%esi\n    leave\n    ret\n\n");

    /* print_int */
    out("_flr_print_int:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%esi\n    pushl %%edi\n");
    out("    movl 8(%%ebp),%%eax\n");
    out("    leal .Lflr_ibuf+11,%%edi\n    movb $10,(%%edi)\n    decl %%edi\n");
    out("    testl %%eax,%%eax\n    jge .Lfpi_pos\n    negl %%eax\n    movl $1,%%esi\n    jmp .Lfpi_l\n");
    out(".Lfpi_pos:\n    xorl %%esi,%%esi\n");
    out(".Lfpi_l:\n    movl $10,%%ecx\n    xorl %%edx,%%edx\n    divl %%ecx\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%edi)\n    decl %%edi\n");
    out("    testl %%eax,%%eax\n    jne .Lfpi_l\n");
    out("    testl %%esi,%%esi\n    je .Lfpi_nom\n    movb $45,(%%edi)\n    decl %%edi\n");
    out(".Lfpi_nom:\n    incl %%edi\n");
    out("    leal .Lflr_ibuf+12,%%edx\n    subl %%edi,%%edx\n    movl %%edi,%%ecx\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    int $0x80\n");
    out("    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* exit */
    out("_flr_exit:\n    movl 4(%%esp),%%ebx\n    movl $1,%%eax\n    int $0x80\n\n");

    /* _flr_print_float(bits:int) — print a 32-bit float */
    out("_flr_print_float:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    /* load float from stack arg, convert to double for printing */
    out("    flds 8(%%ebp)\n");
    out("    subl $8,%%esp\n    fstpl (%%esp)\n");
    out("    pushl 4(%%esp)\n    pushl 4(%%esp)\n"); /* push hi,lo of double */
    out("    call _flr_print_double\n    addl $8,%%esp\n");
    out("    addl $8,%%esp\n");
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* _flr_print_double(lo:int, hi:int) — print a 64-bit double with 6 decimals.
       Rounds v*1e6 to a 64-bit integer FIRST and then splits it into
       integer / fraction parts, so 2.9999999 prints "3.000000" (the old
       code printed "2.000000": the fraction rounded up to 1000000 but the
       carry never reached the integer part) and values >= 2^31 work (the old
       code used a 32-bit fistpl). It also fixes the 16-bit `filds` that was
       used to reload the integer part (anything >= 32768 printed garbage).
       Values >= 9e12 skip the v*1e6 scaling (it would overflow int64): the
       integer part and the fraction (via fprem) are converted separately; NaN / +-inf print as nan / inf. */
    out("_flr_print_double:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    out("    movl 12(%%ebp),%%eax\n    movl %%eax,%%ecx\n");
    out("    shrl $20,%%ecx\n    andl $0x7ff,%%ecx\n    cmpl $0x7ff,%%ecx\n    je .Lfpd_special\n");
    out("    testl %%eax,%%eax\n    jns .Lfpd_pos\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    leal .Lflr_minus,%%ecx\n    movl $1,%%edx\n    int $0x80\n");
    out(".Lfpd_pos:\n");
    out("    fldl 8(%%ebp)\n    fabs\n");
    out("    fldl .Lflr_big\n    fucomip %%st(1),%%st\n    jbe .Lfpd_big\n");
    out("    fldl .Lflr_1e6\n    fmulp\n");
    out("    subl $8,%%esp\n    fistpll (%%esp)\n    popl %%eax\n    popl %%edx\n");
    out("    movl %%eax,%%ebx\n");
    out("    movl %%edx,%%eax\n    xorl %%edx,%%edx\n    movl $1000000,%%ecx\n    divl %%ecx\n");
    out("    movl %%eax,%%esi\n");
    out("    movl %%ebx,%%eax\n    divl %%ecx\n");
    out("    movl %%edx,%%edi\n    movl %%esi,%%edx\n    jmp .Lfpd_emit\n");
    out(".Lfpd_big:\n");
    /* fraction first: frac = fprem(|v|, 1.0), scaled by 1e6 and rounded -> edi */
    out("    fld1\n    fldl 8(%%ebp)\n    fabs\n");
    out(".Lfpd_bfp:\n    fprem\n    fnstsw %%ax\n    testb $4,%%ah\n    jnz .Lfpd_bfp\n    fstp %%st(1)\n");
    out("    fldl .Lflr_1e6\n    fmulp\n    subl $4,%%esp\n    fistpl (%%esp)\n    popl %%edi\n");
    out("    subl $16,%%esp\n    fnstcw 8(%%esp)\n    movzwl 8(%%esp),%%eax\n    orl $0x0c00,%%eax\n");
    out("    movw %%ax,10(%%esp)\n    fldcw 10(%%esp)\n    fistpll (%%esp)\n    fldcw 8(%%esp)\n");
    out("    movl (%%esp),%%eax\n    movl 4(%%esp),%%edx\n    addl $16,%%esp\n");
    out("    testl %%edx,%%edx\n    jns .Lfpd_bok\n    movl $0xffffffff,%%eax\n    movl $0x7fffffff,%%edx\n    xorl %%edi,%%edi\n    jmp .Lfpd_emit\n");
    /* fraction rounded up to 1.000000: carry into the integer part */
    out(".Lfpd_bok:\n    cmpl $1000000,%%edi\n    jb .Lfpd_emit\n    xorl %%edi,%%edi\n    addl $1,%%eax\n    adcl $0,%%edx\n");
    out(".Lfpd_emit:\n");
    out("    call _flr_u64_nonl\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    leal .Lflr_dot,%%ecx\n    movl $1,%%edx\n    int $0x80\n");
    out("    movl %%edi,%%eax\n    leal .Lflr_ibuf+11,%%edi\n    movl $6,%%ecx\n");
    out(".Lfpd_fl:\n    movl $10,%%esi\n    xorl %%edx,%%edx\n    divl %%esi\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%edi)\n    decl %%edi\n    loop .Lfpd_fl\n");
    out("    incl %%edi\n    movl $6,%%edx\n    movl %%edi,%%ecx\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    int $0x80\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    leal .Lflr_nl,%%ecx\n    movl $1,%%edx\n    int $0x80\n");
    out("    jmp .Lfpd_ret\n");
    out(".Lfpd_special:\n");
    out("    movl 12(%%ebp),%%eax\n    andl $0x000fffff,%%eax\n    orl 8(%%ebp),%%eax\n    jne .Lfpd_nan\n");
    out("    cmpl $0,12(%%ebp)\n    jl .Lfpd_ninf\n");
    out("    leal .Lflr_inf,%%ecx\n    movl $4,%%edx\n    jmp .Lfpd_sp_w\n");
    out(".Lfpd_ninf:\n    leal .Lflr_ninf,%%ecx\n    movl $5,%%edx\n    jmp .Lfpd_sp_w\n");
    out(".Lfpd_nan:\n    leal .Lflr_nan,%%ecx\n    movl $4,%%edx\n");
    out(".Lfpd_sp_w:\n    movl $4,%%eax\n    movl $1,%%ebx\n    int $0x80\n");
    out(".Lfpd_ret:\n");
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* _flr_u64_nonl: print edx:eax as an unsigned decimal, no newline.
       Preserves esi/edi/ebx. */
    out("_flr_u64_nonl:\n");
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    out("    movl %%edx,%%ebx\n    movl %%eax,%%esi\n    leal .Lflr_ibuf+31,%%edi\n");
    out(".Lu64_l:\n    movl $10,%%ecx\n");
    out("    movl %%ebx,%%eax\n    xorl %%edx,%%edx\n    divl %%ecx\n    movl %%eax,%%ebx\n");
    out("    movl %%esi,%%eax\n    divl %%ecx\n    movl %%eax,%%esi\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%edi)\n    decl %%edi\n");
    out("    movl %%ebx,%%eax\n    orl %%esi,%%eax\n    jne .Lu64_l\n");
    out("    leal .Lflr_ibuf+31,%%edx\n    subl %%edi,%%edx\n    incl %%edi\n    movl %%edi,%%ecx\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    int $0x80\n");
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    ret\n\n");

    /* strlen / str_len */
    out("_flr_strlen:\n    movl 4(%%esp),%%ecx\n    movl %%ecx,%%eax\n");
    out(".Lflrsl:\n    cmpb $0,(%%ecx)\n    je .Lflrsld\n    incl %%ecx\n    jmp .Lflrsl\n");
    out(".Lflrsld:\n    subl 4(%%esp),%%ecx\n    movl %%ecx,%%eax\n    ret\n\n");
    out("_flr_str_len:\n    jmp _flr_strlen\n\n");

    /* str_eq */
    out("_flr_str_eq:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%esi\n    pushl %%edi\n");
    out("    movl 8(%%ebp),%%esi\n    movl 12(%%ebp),%%edi\n");
    out(".Lfseq:\n    movzbl (%%esi),%%eax\n    movzbl (%%edi),%%ecx\n");
    out("    cmpl %%ecx,%%eax\n    jne .Lfseq_no\n    testl %%eax,%%eax\n    je .Lfseq_yes\n");
    out("    incl %%esi\n    incl %%edi\n    jmp .Lfseq\n");
    out(".Lfseq_yes:\n    movl $1,%%eax\n    jmp .Lfseq_ret\n");
    out(".Lfseq_no:\n    xorl %%eax,%%eax\n");
    out(".Lfseq_ret:\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* int_to_str */
    out("_flr_int_to_str:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%esi\n    pushl %%edi\n");
    out("    movl 8(%%ebp),%%eax\n    leal .Lflr_ibuf+11,%%edi\n    movb $0,(%%edi)\n    decl %%edi\n");
    out("    testl %%eax,%%eax\n    jge .Lfits_p\n    negl %%eax\n    movl $1,%%esi\n    jmp .Lfits_l\n");
    out(".Lfits_p:\n    xorl %%esi,%%esi\n");
    out(".Lfits_l:\n    movl $10,%%ecx\n    xorl %%edx,%%edx\n    divl %%ecx\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%edi)\n    decl %%edi\n");
    out("    testl %%eax,%%eax\n    jne .Lfits_l\n");
    out("    testl %%esi,%%esi\n    je .Lfits_n\n    movb $45,(%%edi)\n    decl %%edi\n");
    out(".Lfits_n:\n    incl %%edi\n    movl %%edi,%%eax\n");
    out("    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* str_to_int */
    out("_flr_str_to_int:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%esi\n");
    out("    movl 8(%%ebp),%%esi\n    xorl %%eax,%%eax\n    xorl %%ecx,%%ecx\n");
    out("    cmpb $45,(%%esi)\n    jne .Lfsti_l\n    movl $1,%%ecx\n    incl %%esi\n");
    out(".Lfsti_l:\n    movzbl (%%esi),%%edx\n    cmpl $48,%%edx\n    jl .Lfsti_d\n");
    out("    cmpl $57,%%edx\n    jg .Lfsti_d\n    imull $10,%%eax\n    subl $48,%%edx\n    addl %%edx,%%eax\n");
    out("    incl %%esi\n    jmp .Lfsti_l\n");
    out(".Lfsti_d:\n    testl %%ecx,%%ecx\n    je .Lfsti_r\n    negl %%eax\n");
    out(".Lfsti_r:\n    popl %%esi\n    leave\n    ret\n\n");

    /* abs / min / max */
    out("_flr_abs:\n    movl 4(%%esp),%%eax\n    testl %%eax,%%eax\n    jge .Labs_ok\n    negl %%eax\n.Labs_ok:\n    ret\n\n");
    out("_flr_min:\n    movl 4(%%esp),%%eax\n    movl 8(%%esp),%%ecx\n    cmpl %%ecx,%%eax\n    jle .Lmin_ok\n    movl %%ecx,%%eax\n.Lmin_ok:\n    ret\n\n");
    out("_flr_max:\n    movl 4(%%esp),%%eax\n    movl 8(%%esp),%%ecx\n    cmpl %%ecx,%%eax\n    jge .Lmax_ok\n    movl %%ecx,%%eax\n.Lmax_ok:\n    ret\n\n");

    /* assert */
    out("_flr_assert:\n    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    movl 8(%%ebp),%%eax\n    testl %%eax,%%eax\n    jne .Lassert_ok\n");
    out("    movl $4,%%eax\n    movl $2,%%ebx\n    leal .Lflr_assert_msg,%%ecx\n    movl $19,%%edx\n    int $0x80\n");
    out("    movl 12(%%ebp),%%esi\n    movl %%esi,%%ecx\n");
    out(".Lasssl:\n    cmpb $0,(%%ecx)\n    je .Lasssd\n    incl %%ecx\n    jmp .Lasssl\n");
    out(".Lasssd:\n    subl %%esi,%%ecx\n    movl %%ecx,%%edx\n    movl %%esi,%%ecx\n");
    out("    movl $4,%%eax\n    movl $2,%%ebx\n    int $0x80\n");
    out("    movl $4,%%eax\n    movl $2,%%ebx\n    leal .Lflr_nl,%%ecx\n    movl $1,%%edx\n    int $0x80\n");
    out("    movl $1,%%ebx\n    movl $1,%%eax\n    int $0x80\n");
    out(".Lassert_ok:\n    leave\n    ret\n\n");

    /* ── dynamic heap via sbrk (sys_brk = 45) ────────────────────
       _flr_alloc(size) -> ptr
       Calls brk(0) to get current break, aligns size to 4,
       then calls brk(break+size) to grow. Simple, no hard cap.     */
    out("_flr_alloc:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%ebx\n");
    /* get current break */
    out("    movl $45,%%eax\n    xorl %%ebx,%%ebx\n    int $0x80\n");
    out("    movl %%eax,%%ecx\n"); /* ecx = current break = base of new block */
    /* align requested size to 4 */
    out("    movl 8(%%ebp),%%edx\n    addl $3,%%edx\n    andl $-4,%%edx\n");
    /* new break = old + size */
    out("    movl $45,%%eax\n    leal (%%ecx,%%edx),%%ebx\n    int $0x80\n");
    out("    movl %%ecx,%%eax\n"); /* return base of allocated block */
    out("    popl %%ebx\n    leave\n    ret\n\n");
    out("_flr_free:\n    ret\n\n");

    /* ── str_concat(a:str, b:str) -> str ─────────────────────────
       Allocates len(a)+len(b)+1 bytes, copies both strings in.     */
    out("_flr_str_concat:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    out("    movl 8(%%ebp),%%esi\n");   /* a */
    out("    movl 12(%%ebp),%%edi\n");  /* b */
    /* len(a) */
    out("    pushl %%esi\n    call _flr_strlen\n    addl $4,%%esp\n    movl %%eax,%%ebx\n");
    /* len(b) */
    out("    pushl %%edi\n    call _flr_strlen\n    addl $4,%%esp\n");
    out("    addl %%ebx,%%eax\n    incl %%eax\n"); /* total = lena+lenb+1 */
    /* alloc */
    out("    pushl %%eax\n    call _flr_alloc\n    addl $4,%%esp\n");
    out("    pushl %%eax\n"); /* save result ptr */
    /* copy a into result */
    out("    movl %%eax,%%edi\n");
    out(".Lsca_l:\n    movzbl (%%esi),%%ecx\n    testl %%ecx,%%ecx\n    je .Lsca_d\n");
    out("    movb %%cl,(%%edi)\n    incl %%esi\n    incl %%edi\n    jmp .Lsca_l\n");
    out(".Lsca_d:\n");
    /* copy b */
    out("    movl 12(%%ebp),%%esi\n");
    out(".Lscb_l:\n    movzbl (%%esi),%%ecx\n    movb %%cl,(%%edi)\n    testl %%ecx,%%ecx\n    je .Lscb_d\n");
    out("    incl %%esi\n    incl %%edi\n    jmp .Lscb_l\n");
    out(".Lscb_d:\n");
    out("    popl %%eax\n"); /* return result */
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* ── str_format(fmt:str, nargs:int, arg0..argN) -> str ───────
       Supports %d (int), %s (str), %% (literal %).
       Output buffer: alloc 512 bytes (simple, sufficient for typical use).  */
    out("_flr_str_format:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    /* alloc 512 byte output buffer */
    out("    pushl $512\n    call _flr_alloc\n    addl $4,%%esp\n");
    out("    movl %%eax,%%edi\n"); /* edi = output ptr */
    out("    pushl %%edi\n");      /* save result */
    out("    movl 8(%%ebp),%%esi\n");    /* fmt */
    out("    movl 12(%%ebp),%%ecx\n");   /* nargs */
    out("    leal 16(%%ebp),%%ebx\n");   /* &arg0 */
    out(".Lsfmt_l:\n");
    out("    movzbl (%%esi),%%eax\n    testl %%eax,%%eax\n    je .Lsfmt_end\n");
    out("    cmpl $37,%%eax\n    jne .Lsfmt_copy\n"); /* '%' */
    out("    incl %%esi\n    movzbl (%%esi),%%eax\n");
    out("    cmpl $37,%%eax\n    je .Lsfmt_copy\n");  /* %% -> % */
    out("    cmpl $100,%%eax\n    je .Lsfmt_d\n");    /* %d */
    out("    cmpl $115,%%eax\n    je .Lsfmt_s\n");    /* %s */
    /* unknown spec: just copy literal */
    out("    jmp .Lsfmt_copy\n");
    /* %d */
    out(".Lsfmt_d:\n");
    out("    incl %%esi\n");
    out("    testl %%ecx,%%ecx\n    je .Lsfmt_l\n");
    /* Only esi (repurposed below as a scratch walk pointer) needs saving —
       ebx/ecx/edi must advance PERMANENTLY (next arg, remaining count,
       output write position) rather than being rewound after the
       substitution, which was the original bug: it silently discarded
       every substituted value and never advanced past the first arg. ecx
       specifically must be saved/restored around the call because
       _flr_int_to_str clobbers it internally (it's caller-saved by
       convention); ebx and edi are left alone since int_to_str doesn't
       touch them. */
    out("    pushl %%esi\n");
    out("    movl (%%ebx),%%eax\n    addl $4,%%ebx\n    decl %%ecx\n");
    out("    pushl %%ecx\n");
    out("    pushl %%eax\n    call _flr_int_to_str\n    addl $4,%%esp\n");
    out("    popl %%ecx\n");
    /* copy int string into output */
    out("    movl %%eax,%%esi\n");
    out(".Lsfmt_dc:\n    movzbl (%%esi),%%eax\n    testl %%eax,%%eax\n    je .Lsfmt_dr\n");
    out("    movb %%al,(%%edi)\n    incl %%esi\n    incl %%edi\n    jmp .Lsfmt_dc\n");
    out(".Lsfmt_dr:\n");
    out("    popl %%esi\n");
    out("    jmp .Lsfmt_l\n");
    /* %s */
    out(".Lsfmt_s:\n");
    out("    incl %%esi\n");
    out("    testl %%ecx,%%ecx\n    je .Lsfmt_l\n");
    out("    pushl %%esi\n");
    out("    movl (%%ebx),%%esi\n    addl $4,%%ebx\n    decl %%ecx\n");
    out(".Lsfmt_sc:\n    movzbl (%%esi),%%eax\n    testl %%eax,%%eax\n    je .Lsfmt_sr\n");
    out("    movb %%al,(%%edi)\n    incl %%esi\n    incl %%edi\n    jmp .Lsfmt_sc\n");
    out(".Lsfmt_sr:\n");
    out("    popl %%esi\n");
    out("    jmp .Lsfmt_l\n");
    /* plain copy */
    out(".Lsfmt_copy:\n    movb %%al,(%%edi)\n    incl %%esi\n    incl %%edi\n    jmp .Lsfmt_l\n");
    out(".Lsfmt_end:\n    movb $0,(%%edi)\n");
    out("    popl %%eax\n"); /* return result ptr */
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════
   CODE GENERATOR — x86-64 GAS AT&T, System V AMD64 ABI, syscall
   Result of integer/pointer/bool/long expressions: rax
   Result of float expressions:  xmm0 (32-bit lane)
   Result of double expressions: xmm0 (64-bit lane)
   ═══════════════════════════════════════════════════════════════════════ */

static void gen_expr64(Node *n);
static void gen_stmt64(Node *n);

/* ── x86-64 value representation ─────────────────────────────────────
   every int/long/bool/ptr/str/struct value : rax
   float / double                           : xmm0 (single / double precision) */
static TypeRef *ty_dbl64(void){static TypeRef *t;if(!t)t=mktype(TY_DOUBLE,NULL,NULL);return t;}
static TypeRef *ty_flt64(void){static TypeRef *t;if(!t)t=mktype(TY_FLOAT,NULL,NULL);return t;}
static int kcls64(TypeRef *t){int k=kcls(t);return k==K_L?K_I:k;}

static void gen_convert64(TypeRef *from,TypeRef *to){
    int f=kcls64(from),t=kcls64(to);
    if(f==t)return;
    if(f==K_I){out(t==K_F?"    cvtsi2ssq %%rax,%%xmm0\n":"    cvtsi2sdq %%rax,%%xmm0\n");return;}
    if(t==K_I){out(f==K_F?"    cvttss2si %%xmm0,%%rax\n":"    cvttsd2si %%xmm0,%%rax\n");return;}
    if(f==K_F)out("    cvtss2sd %%xmm0,%%xmm0\n");   /* float -> double */
    else      out("    cvtsd2ss %%xmm0,%%xmm0\n");   /* double -> float */
}
/* push the current value (rax or xmm0) as one 8-byte stack slot */
static void spill64(TypeRef *t){
    int k=kcls64(t);
    if(k==K_F)      out("    subq $8,%%rsp\n    movss %%xmm0,(%%rsp)\n");
    else if(k==K_D) out("    subq $8,%%rsp\n    movsd %%xmm0,(%%rsp)\n");
    else            out("    pushq %%rax\n");
}
/* load a value of type t from memory operand `mem` into rax / xmm0 */
static void load64(TypeRef *t,const char *mem){
    int k=kcls64(t);
    if(k==K_F)      out("    movss %s,%%xmm0\n",mem);
    else if(k==K_D) out("    movsd %s,%%xmm0\n",mem);
    else            out("    movq %s,%%rax\n",mem);
}
/* store rax / xmm0 (already of type t) to memory operand `mem` */
static void store64(TypeRef *t,const char *mem){
    int k=kcls64(t);
    if(k==K_F)      out("    movss %%xmm0,%s\n",mem);
    else if(k==K_D) out("    movsd %%xmm0,%s\n",mem);
    else            out("    movq %%rax,%s\n",mem);
}

static void gen_store64(Node *lv){
    /* value to store is in rax / xmm0, already converted to the lvalue's type */
    char mem[96];
    switch(lv->kind){
    case N_IDENT:{
        Var *v=find_var(lv->name);
        if(!v)die("%s:%d: undefined variable '%s'",lv->file,lv->line,lv->name);
        int gidx=(v->offset<=-999999)?(-999999-v->offset):-1;
        if(gidx>=0)snprintf(mem,sizeof mem,"_gv_%s(%%rip)",gvars[gidx].name);
        else       snprintf(mem,sizeof mem,"%d(%%rbp)",v->offset);
        store64(v->type,mem);
        break;
    }
    case N_INDEX:
        spill64(lv->etype);
        gen_expr64(lv->right); out("    pushq %%rax\n");
        gen_expr64(lv->left);  out("    popq %%rcx\n");
        /* array layout: [count:8][count:8][elem0][elem1]...  elements at base+16 */
        out("    leaq 16(%%rax,%%rcx,8),%%rdx\n");
        if(kcls64(lv->etype)==K_I)out("    movq (%%rsp),%%rax\n");
        else load64(lv->etype,"(%rsp)");
        out("    addq $8,%%rsp\n");
        store64(lv->etype,"(%rdx)");
        break;
    case N_FIELD:{
        spill64(lv->etype);
        gen_expr64(lv->left);
        int found_off=field_offset(lv->left,lv->sval,8);
        out("    movq %%rax,%%rdx\n");
        load64(lv->etype,"(%rsp)");
        out("    addq $8,%%rsp\n");
        snprintf(mem,sizeof mem,"%d(%%rdx)",found_off);
        store64(lv->etype,mem);
        break;
    }
    default:die("gen_store64: not an lvalue");
    }
}

static void gen_intrinsic64(Node *n){
    const char *nm=n->callee;
    if(strcmp(nm,"__syscall")==0){
        int argc=n->args.n;
        if(argc<1)die("__syscall: need at least syscall number");
        /* evaluate args right-to-left, push, then load registers */
        for(int i=argc-1;i>=0;i--){gen_expr64(n->args.d[i]);out("    pushq %%rax\n");}
        out("    popq %%rax\n"); /* syscall number */
        const char *sc[6]={"rdi","rsi","rdx","r10","r8","r9"};
        for(int i=1;i<argc&&i<=6;i++) out("    popq %%%s\n",sc[i-1]);
        out("    syscall\n");
        return;
    }
    if(strcmp(nm,"__inb")==0){
        gen_expr64(n->args.d[0]);
        out("    movw %%ax,%%dx\n    xorl %%eax,%%eax\n    inb %%dx,%%al\n    movzbl %%al,%%eax\n");
        return;
    }
    if(strcmp(nm,"__outb")==0){
        gen_expr64(n->args.d[1]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[0]);out("    movw %%ax,%%dx\n");
        out("    popq %%rax\n    outb %%al,%%dx\n    xorl %%eax,%%eax\n");
        return;
    }
    if(strcmp(nm,"__cli")==0){out("    cli\n    xorl %%eax,%%eax\n");return;}
    if(strcmp(nm,"__sti")==0){out("    sti\n    xorl %%eax,%%eax\n");return;}
    if(strcmp(nm,"__hlt")==0){out("    hlt\n    xorl %%eax,%%eax\n");return;}
    if(strcmp(nm,"__rdtsc")==0){out("    rdtsc\n    shlq $32,%%rdx\n    orq %%rdx,%%rax\n");return;}
    if(strcmp(nm,"__peek")==0){
        /* Falcon's `int` is signed — sign-extend the 4-byte cell into the
           64-bit result register (movslq), matching how the 32-bit backend's
           plain 32-bit eax naturally behaves as a signed value on print. A
           zero-extending movl here would turn negative peeked ints (e.g.
           0xDEADBEEF) into large positive 64-bit values instead. */
        gen_expr64(n->args.d[0]);out("    movslq (%%rax),%%rax\n");return;
    }
    if(strcmp(nm,"__poke")==0){
        gen_expr64(n->args.d[1]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[0]);out("    popq %%rcx\n    movl %%ecx,(%%rax)\n    xorl %%eax,%%eax\n");return;
    }
    if(strcmp(nm,"__peekb")==0){
        gen_expr64(n->args.d[0]);out("    movzbl (%%rax),%%eax\n");return;
    }
    if(strcmp(nm,"__pokeb")==0){
        gen_expr64(n->args.d[1]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[0]);out("    popq %%rcx\n    movb %%cl,(%%rax)\n    xorl %%eax,%%eax\n");return;
    }
    if(strcmp(nm,"__memset")==0){
        /* rdi=ptr, rsi=val, rdx=n */
        gen_expr64(n->args.d[2]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[1]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[0]);
        out("    movq %%rax,%%rdi\n");
        out("    popq %%rsi\n    popq %%rdx\n");
        out("    call _flr_memset\n");
        return;
    }
    if(strcmp(nm,"__memcpy")==0){
        gen_expr64(n->args.d[2]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[1]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[0]);
        out("    movq %%rax,%%rdi\n");
        out("    popq %%rsi\n    popq %%rdx\n");
        out("    call _flr_memcpy\n");
        return;
    }
    die("unknown intrinsic '%s'",nm);
}

static void gen_expr64(Node *n){
    if(!n){out("    xorl %%eax,%%eax\n");return;}
    switch(n->kind){
    case N_INTLIT:
        out("    movq $%lld,%%rax\n",n->ival); break;
    case N_LONGLIT:
        out("    movq $%lld,%%rax\n",n->ival); break;
    case N_BOOLLIT:
        out("    movq $%d,%%rax\n",n->bval?1:0); break;
    case N_FLOATLIT:{
        int idx=add_flit(n->dval,0);
        out("    movss .Lfl%d(%%rip),%%xmm0\n",idx); break;
    }
    case N_DOUBLELIT:{
        int idx=add_flit(n->dval,1);
        out("    movsd .Lfl%d(%%rip),%%xmm0\n",idx); break;
    }
    case N_STRLIT:
        out("    leaq .Lstr%d(%%rip),%%rax\n",add_strlit(n->sval)); break;
    case N_IDENT:{
        Var *v=find_var(n->name);
        if(!v)die("%s:%d: undefined variable '%s'",n->file,n->line,n->name);
        int gidx=(v->offset<=-999999)?(-999999-v->offset):-1;
        if(gidx>=0){
            /* global: address the .data symbol directly (RIP-relative),
               not (%rbp) — v->offset here is a sentinel, not a real
               stack offset. Every global is a full 8-byte quad on
               x86-64 (see emit_data), except float/double. */
            if(v->type&&v->type->kind==TY_FLOAT)
                out("    movss _gv_%s(%%rip),%%xmm0\n",gvars[gidx].name);
            else if(v->type&&v->type->kind==TY_DOUBLE)
                out("    movsd _gv_%s(%%rip),%%xmm0\n",gvars[gidx].name);
            else
                out("    movq _gv_%s(%%rip),%%rax\n",gvars[gidx].name);
            break;
        }
        if(v->type&&v->type->kind==TY_FLOAT){
            out("    movss %d(%%rbp),%%xmm0\n",v->offset);
        } else if(v->type&&v->type->kind==TY_DOUBLE){
            out("    movsd %d(%%rbp),%%xmm0\n",v->offset);
        } else {
            out("    movq %d(%%rbp),%%rax\n",v->offset);
        }
        break;
    }
    case N_BINOP:{
        const char *op=n->op;
        /* short-circuit `and` / `or` (see the x86-32 backend) */
        if(strcmp(op,"and")==0||strcmp(op,"or")==0){
            int is_and=(op[0]=='a');
            int Lsc=new_label();
            gen_expr64(n->left);
            out("    testq %%rax,%%rax\n");
            out(is_and?"    jz .Lscs%d\n":"    jnz .Lscs%d\n",Lsc);
            gen_expr64(n->right);
            out("    testq %%rax,%%rax\n");
            out(is_and?"    jz .Lscs%d\n":"    jnz .Lscs%d\n",Lsc);
            out("    movl $%d,%%eax\n    jmp .Lsce%d\n",is_and?1:0,Lsc);
            out(".Lscs%d:\n    movl $%d,%%eax\n.Lsce%d:\n",Lsc,is_and?0:1,Lsc);
            break;
        }
        int lf=(n->left&&n->left->etype&&(n->left->etype->kind==TY_FLOAT||n->left->etype->kind==TY_DOUBLE));
        int rf=(n->right&&n->right->etype&&(n->right->etype->kind==TY_FLOAT||n->right->etype->kind==TY_DOUBLE));
        int use_dbl=(n->etype&&n->etype->kind==TY_DOUBLE)||
                    (n->left&&n->left->etype&&n->left->etype->kind==TY_DOUBLE)||
                    (n->right&&n->right->etype&&n->right->etype->kind==TY_DOUBLE);

        /* str + str → str_concat */
        if(strcmp(op,"+")==0&&n->left->etype&&n->left->etype->kind==TY_STR){
            gen_expr64(n->left); out("    pushq %%rax\n");
            gen_expr64(n->right);
            out("    movq %%rax,%%rsi\n    popq %%rdi\n");
            out("    call _flr_str_concat\n");
            break;
        }

        /* float/double via SSE.  The right operand is spilled to the stack
           while the left is evaluated: the old code parked it in xmm1, which
           any nested float expression on the left (`a*b + c*d`) clobbered. */
        if(lf||rf){
            int d=use_dbl;
            int is_cmp=(!strcmp(op,"==")||!strcmp(op,"!=")||!strcmp(op,"<")||!strcmp(op,">")||!strcmp(op,"<=")||!strcmp(op,">="));
            TypeRef *CT=d?ty_dbl64():ty_flt64();
            gen_expr64(n->left);gen_convert64(n->left->etype,CT);
            out(d?"    subq $8,%%rsp\n    movsd %%xmm0,(%%rsp)\n":"    subq $8,%%rsp\n    movss %%xmm0,(%%rsp)\n");
            gen_expr64(n->right);gen_convert64(n->right->etype,CT);
            out("    movaps %%xmm0,%%xmm1\n");
            out(d?"    movsd (%%rsp),%%xmm0\n":"    movss (%%rsp),%%xmm0\n");
            out("    addq $8,%%rsp\n");               /* xmm0 = left, xmm1 = right */
            if(is_cmp){
                /* ucomis* sets CF/ZF/PF; unordered (NaN) sets all three. `<` and
                   `<=` are evaluated as the swapped `>` / `>=` so NaN yields
                   false; == and != also consult PF. */
                int swap=(!strcmp(op,"<")||!strcmp(op,"<="));
                if(!swap)out(d?"    ucomisd %%xmm1,%%xmm0\n":"    ucomiss %%xmm1,%%xmm0\n");
                else     out(d?"    ucomisd %%xmm0,%%xmm1\n":"    ucomiss %%xmm0,%%xmm1\n");
                if     (!strcmp(op,"==")) out("    sete %%al\n    setnp %%cl\n    andb %%cl,%%al\n");
                else if(!strcmp(op,"!=")) out("    setne %%al\n    setp %%cl\n    orb %%cl,%%al\n");
                else if(!strcmp(op,"<")||!strcmp(op,">")) out("    seta %%al\n");
                else                                      out("    setae %%al\n");
                out("    movzbq %%al,%%rax\n");
            } else {
                const char *sx=d?"sd":"ss";
                if     (!strcmp(op,"+"))out("    add%s %%xmm1,%%xmm0\n",sx);
                else if(!strcmp(op,"-"))out("    sub%s %%xmm1,%%xmm0\n",sx);
                else if(!strcmp(op,"*"))out("    mul%s %%xmm1,%%xmm0\n",sx);
                else if(!strcmp(op,"/"))out("    div%s %%xmm1,%%xmm0\n",sx);
                else if(!strcmp(op,"%")){
                    /* fmod via x87 fprem (no libm): st0=left, st1=right */
                    int L=new_label();
                    out("    subq $16,%%rsp\n    mov%s %%xmm1,(%%rsp)\n    mov%s %%xmm0,8(%%rsp)\n",sx,sx);
                    out(d?"    fldl (%%rsp)\n    fldl 8(%%rsp)\n":"    flds (%%rsp)\n    flds 8(%%rsp)\n");
                    out(".Lfmod%d:\n    fprem\n    fnstsw %%ax\n    testb $4,%%ah\n    jnz .Lfmod%d\n    fstp %%st(1)\n",L,L);
                    out(d?"    fstpl 8(%%rsp)\n":"    fstps 8(%%rsp)\n");
                    out("    mov%s 8(%%rsp),%%xmm0\n    addq $16,%%rsp\n",sx);
                }
                else die("%s:%d: operator '%s' is not defined for float/double",n->file,n->line,op);
            }
            break;
        }

        /* integer / long / bool — all native 64-bit */
        gen_expr64(n->left);  out("    pushq %%rax\n");
        gen_expr64(n->right); out("    movq %%rax,%%rcx\n    popq %%rax\n");
        if     (!strcmp(op,"+"))  out("    addq %%rcx,%%rax\n");
        else if(!strcmp(op,"-"))  out("    subq %%rcx,%%rax\n");
        else if(!strcmp(op,"*"))  out("    imulq %%rcx,%%rax\n");
        else if(!strcmp(op,"/"))  {out("    cqto\n");out("    idivq %%rcx\n");}
        else if(!strcmp(op,"%"))  {out("    cqto\n");out("    idivq %%rcx\n");out("    movq %%rdx,%%rax\n");}
        else if(!strcmp(op,"&"))  out("    andq %%rcx,%%rax\n");
        else if(!strcmp(op,"|"))  out("    orq  %%rcx,%%rax\n");
        else if(!strcmp(op,"^"))  out("    xorq %%rcx,%%rax\n");
        else if(!strcmp(op,"<<")) out("    shlq %%cl,%%rax\n");
        else if(!strcmp(op,">>")) out("    sarq %%cl,%%rax\n");
        else if(!strcmp(op,"==")) {out("    cmpq %%rcx,%%rax\n");out("    sete %%al\n");out("    movzbq %%al,%%rax\n");}
        else if(!strcmp(op,"!=")) {out("    cmpq %%rcx,%%rax\n");out("    setne %%al\n");out("    movzbq %%al,%%rax\n");}
        else if(!strcmp(op,"<"))  {out("    cmpq %%rcx,%%rax\n");out("    setl %%al\n");out("    movzbq %%al,%%rax\n");}
        else if(!strcmp(op,">"))  {out("    cmpq %%rcx,%%rax\n");out("    setg %%al\n");out("    movzbq %%al,%%rax\n");}
        else if(!strcmp(op,"<=")) {out("    cmpq %%rcx,%%rax\n");out("    setle %%al\n");out("    movzbq %%al,%%rax\n");}
        else if(!strcmp(op,">=")) {out("    cmpq %%rcx,%%rax\n");out("    setge %%al\n");out("    movzbq %%al,%%rax\n");}
        else if(!strcmp(op,"and")){
            out("    testq %%rax,%%rax\n");out("    setne %%al\n");
            out("    testq %%rcx,%%rcx\n");out("    setne %%cl\n");
            out("    andb %%cl,%%al\n");out("    movzbq %%al,%%rax\n");
        }
        else if(!strcmp(op,"or")){
            out("    orq %%rcx,%%rax\n");out("    setne %%al\n");out("    movzbq %%al,%%rax\n");
        }
        else die("unknown binop '%s'",op);
        break;
    }
    case N_UNOP:
        gen_expr64(n->left);
        if(!strcmp(n->op,"-")&&n->etype&&n->etype->kind==TY_FLOAT){
            /* flip the sign bit: 0.0 - x would turn -(0.0) into +0.0 */
            out("    movd %%xmm0,%%eax\n    btcl $31,%%eax\n    movd %%eax,%%xmm0\n");
        }
        else if(!strcmp(n->op,"-")&&n->etype&&n->etype->kind==TY_DOUBLE){
            out("    movq %%xmm0,%%rax\n    btcq $63,%%rax\n    movq %%rax,%%xmm0\n");
        }
        else if     (!strcmp(n->op,"-"))  out("    negq %%rax\n");
        else if(!strcmp(n->op,"~"))  out("    notq %%rax\n");
        else if(!strcmp(n->op,"not")){out("    testq %%rax,%%rax\n");out("    sete %%al\n");out("    movzbq %%al,%%rax\n");}
        break;
    case N_CALL:{
        if(is_intrinsic(n->callee)){gen_intrinsic64(n);break;}

        /* evaluate all args, push onto stack in reverse order,
           then load first 6 into registers (integer ABI) */
        int nargs=n->args.n;

        /* handle print specially */
        if(has_std&&strcmp(n->callee,"print")==0&&nargs==1){
            Node *arg=n->args.d[0];
            int is_str=(arg->etype&&arg->etype->kind==TY_STR);
            int is_flt=(arg->etype&&arg->etype->kind==TY_FLOAT);
            int is_dbl=(arg->etype&&arg->etype->kind==TY_DOUBLE);
            if(!is_flt&&!is_dbl&&arg->kind==N_IDENT){
                Var *v=find_var(arg->name);
                if(v&&v->type&&v->type->kind==TY_FLOAT)is_flt=1;
                if(v&&v->type&&v->type->kind==TY_DOUBLE)is_dbl=1;
            }
            gen_expr64(arg);
            if(is_flt){out("    call _flr_print_float\n");}
            else if(is_dbl){out("    call _flr_print_double\n");}
            else if(is_str){out("    movq %%rax,%%rdi\n    call _flr_print_str\n");}
            else           {out("    movq %%rax,%%rdi\n    call _flr_print_int\n");}
            break;
        }

        /* str_concat(a,b) */
        if(has_std&&strcmp(n->callee,"str_concat")==0&&nargs==2){
            gen_expr64(n->args.d[1]); out("    pushq %%rax\n");
            gen_expr64(n->args.d[0]);
            out("    movq %%rax,%%rdi\n    popq %%rsi\n");
            out("    call _flr_str_concat\n");
            break;
        }

        /* str_format(fmt, ...) */
        if(has_std&&strcmp(n->callee,"str_format")==0&&nargs>=1){
            /* push extra args right-to-left, then nargs-1 count, then fmt */
            for(int i=nargs-1;i>=1;i--){gen_expr64(n->args.d[i]);out("    pushq %%rax\n");}
            out("    pushq $%d\n",nargs-1);
            gen_expr64(n->args.d[0]); out("    pushq %%rax\n");
            /* load first 2 into rdi/rsi */
            out("    popq %%rdi\n    popq %%rsi\n");
            /* remaining on stack — _flr_str_format reads from rsp+16 */
            out("    call _flr_str_format\n");
            if(nargs>1) out("    addq $%d,%%rsp\n",(nargs-1)*8);
            break;
        }

        /* general call, System V AMD64: integer-class args go in
           rdi,rsi,rdx,rcx,r8,r9, float/double args in xmm0..xmm7 (counted
           separately), anything beyond that on the stack. Each argument is
           converted to the callee's declared parameter type first. */
        {
            FuncSig *sig=find_func(n->callee);
            int *where=calloc(nargs+1,sizeof(int));   /* >=100: xmm(where-100), 0..5: int reg, -1: stack */
            TypeRef **pts=calloc(nargs+1,sizeof(TypeRef*));
            int ni=0,nf=0,ns=0;
            for(int i=0;i<nargs;i++){
                Node *a=n->args.d[i];
                pts[i]=(sig&&i<sig->params.n)?sig->params.d[i].type:a->etype;
                if(kcls64(pts[i])==K_I){ if(ni<6)where[i]=ni++; else {where[i]=-1;ns++;} }
                else                   { if(nf<8)where[i]=100+nf++; else {where[i]=-1;ns++;} }
            }
            /* evaluate right-to-left into 8-byte temp slots: arg i ends up at 8*i(%rsp) */
            for(int i=nargs-1;i>=0;i--){
                gen_expr64(n->args.d[i]);
                gen_convert64(n->args.d[i]->etype,pts[i]);
                spill64(pts[i]);
            }
            for(int i=0;i<nargs;i++){
                char mem[32];snprintf(mem,sizeof mem,"%d(%%rsp)",8*i);
                if(where[i]>=100){
                    out(kcls64(pts[i])==K_D?"    movsd %s,%%xmm%d\n":"    movss %s,%%xmm%d\n",mem,where[i]-100);
                }else if(where[i]>=0){
                    out("    movq %s,%%%s\n",mem,argregs64[where[i]]);
                }
            }
            if(ns>0){
                /* copy the overflow args, in order, to the very top of the stack */
                out("    subq $%d,%%rsp\n",8*ns);
                int k=0;
                for(int i=0;i<nargs;i++)if(where[i]==-1){
                    out("    movq %d(%%rsp),%%rax\n    movq %%rax,%d(%%rsp)\n",8*ns+8*i,8*k);k++;
                }
            }
            out("    movl $%d,%%eax\n",nf);        /* al = number of vector registers used */
            out("    call %s\n",n->callee);
            if(nargs+ns>0)out("    addq $%d,%%rsp\n",8*(nargs+ns));
            free(where);free(pts);
        }
        break;
    }
    case N_INDEX:
        gen_expr64(n->right); out("    pushq %%rax\n");
        gen_expr64(n->left);  out("    popq %%rcx\n");
        out("    leaq 16(%%rax,%%rcx,8),%%rax\n");
        load64(n->etype,"(%rax)"); break;
    case N_FIELD:{
        gen_expr64(n->left);
        int found_off=field_offset(n->left,n->sval,8);
        char mem[48];snprintf(mem,sizeof mem,"%d(%%rax)",found_off);
        load64(n->etype,mem); break;
    }
    case N_ARRAYLIT:{
        TypeRef *elt=(n->etype&&n->etype->kind==TY_ARRAY&&n->etype->elem)?n->etype->elem:NULL;
        int cnt=n->elems.n;
        long long alloc_sz=16+(long long)cnt*8;
        out("    movq $%lld,%%rdi\n",alloc_sz);
        out("    call _flr_alloc\n");
        out("    pushq %%rax\n");
        out("    movq $%d,(%%rax)\n    movq $%d,8(%%rax)\n",cnt,cnt);
        for(int i=0;i<cnt;i++){
            gen_expr64(n->elems.d[i]);
            gen_convert64(n->elems.d[i]->etype,elt);
            /* reload the base AFTER evaluating the element (it may clobber rdi) */
            out("    movq (%%rsp),%%rdi\n");
            char mem[48];snprintf(mem,sizeof mem,"%d(%%rdi)",(int)(16+i*8));
            store64(elt,mem);
        }
        out("    popq %%rax\n"); break;
    }
    default:die("gen_expr64: unhandled kind %d",n->kind);
    }
}

static void gen_stmt64(Node *n){
    if(!n)return;
    switch(n->kind){
    case N_IMPORT:if(strcmp(n->import_path,"std")==0)has_std=1;break;
    case N_VARDECL:
    case N_LETDECL:
    case N_CONSTDECL:{
        if(n->is_static){
            /* function-local static: backed by the hidden global slot
               registered during type-check (n->sval / n->ival index).
               No init code — a literal initializer is already baked
               into .data by emit_data; anything else starts at 0. */
            if(nvars>=4096)die("too many variables");
            vars[nvars].name=xstrdup(n->name);
            vars[nvars].offset=-999999-(int)n->ival;
            vars[nvars].type=n->typeref?n->typeref:(n->left&&n->left->etype?n->left->etype:mktype(TY_INT,NULL,NULL));
            nvars++;
            break;
        }
        TypeRef *vtype=n->typeref;
        if(!vtype&&n->left&&n->left->etype)vtype=n->left->etype;
        int off=alloc_var(n->name,vtype);
        if(n->left){
            gen_expr64(n->left);
            gen_convert64(n->left->etype,vtype);   /* int->double, double->float, ... */
            char mem[48];snprintf(mem,sizeof mem,"%d(%%rbp)",off);
            store64(vtype,mem);
        } else {
            out("    movq $0,%d(%%rbp)\n",off);
        }
        break;
    }
    case N_ASSIGN:{
        /* `x op= y` was rewritten to `x = x op y` by the type checker */
        gen_expr64(n->right);
        gen_convert64(n->right->etype,n->left->etype);
        gen_store64(n->left);
        break;
    }
    case N_RETURN:
        if(n->left){gen_expr64(n->left);gen_convert64(n->left->etype,cur_ret_type);}
        else out("    xorl %%eax,%%eax\n");
        /* The callee-saved registers were pushed AFTER the locals were
           allocated, so they are popped straight off rsp. The old code added
           the frame size back first, which made these pops read local-variable
           slots instead — a `return` clobbered the caller's rbx/r13-r15. */
        out("    popq %%r15\n    popq %%r14\n    popq %%r13\n");
        out("    popq %%rbx\n");
        out("    leave\n    ret\n");
        break;
    case N_EXPRSTMT:{
        Node *e=n->left;
        /* special-case print at statement level (avoids duplicate dispatch) */
        if(has_std&&e->kind==N_CALL&&strcmp(e->callee,"print")==0&&e->args.n==1){
            Node *arg=e->args.d[0];
            int is_str=(arg->etype&&arg->etype->kind==TY_STR);
            int is_flt=(arg->etype&&arg->etype->kind==TY_FLOAT);
            int is_dbl=(arg->etype&&arg->etype->kind==TY_DOUBLE);
            if(!is_flt&&!is_dbl&&arg->kind==N_IDENT){
                Var *v=find_var(arg->name);
                if(v&&v->type&&v->type->kind==TY_FLOAT)is_flt=1;
                if(v&&v->type&&v->type->kind==TY_DOUBLE)is_dbl=1;
            }
            gen_expr64(arg);
            if(is_flt)      out("    call _flr_print_float\n");
            else if(is_dbl) out("    call _flr_print_double\n");
            else if(is_str) out("    movq %%rax,%%rdi\n    call _flr_print_str\n");
            else            out("    movq %%rax,%%rdi\n    call _flr_print_int\n");
            break;
        }
        gen_expr64(e); break;
    }
    case N_IF:{
        int lend=new_label(),lnext=new_label();
        gen_expr64(n->cond);out("    testq %%rax,%%rax\n    je .Lif%d\n",lnext);
        {int sv=nvars;for(int i=0;i<n->body.n;i++)gen_stmt64(n->body.d[i]);nvars=sv;}
        out("    jmp .Lif%d\n.Lif%d:\n",lend,lnext);
        for(int ei=0;ei<n->elifs.n;ei++){
            ElifClause *ec=&n->elifs.d[ei];int ln2=new_label();
            gen_expr64(ec->cond);out("    testq %%rax,%%rax\n    je .Lif%d\n",ln2);
            {int sv=nvars;for(int i=0;i<ec->body.n;i++)gen_stmt64(ec->body.d[i]);nvars=sv;}
            out("    jmp .Lif%d\n.Lif%d:\n",lend,ln2);
        }
        {int sv=nvars;for(int i=0;i<n->else_body.n;i++)gen_stmt64(n->else_body.d[i]);nvars=sv;}
        out(".Lif%d:\n",lend); break;
    }
    case N_WHILE:{
        int ls=new_label(),le=new_label();
        char ob[64],oc[64];strcpy(ob,break_lbl);strcpy(oc,cont_lbl);
        snprintf(break_lbl,64,".Lwh%d",le);snprintf(cont_lbl,64,".Lwh%d",ls);
        out(".Lwh%d:\n",ls);gen_expr64(n->cond);out("    testq %%rax,%%rax\n    je .Lwh%d\n",le);
        {int sv=nvars;for(int i=0;i<n->body.n;i++)gen_stmt64(n->body.d[i]);nvars=sv;}
        out("    jmp .Lwh%d\n.Lwh%d:\n",ls,le);
        strcpy(break_lbl,ob);strcpy(cont_lbl,oc); break;
    }
    case N_FOR:{
        int ls=new_label(),le=new_label(),lp=new_label();
        char ob[64],oc[64];strcpy(ob,break_lbl);strcpy(oc,cont_lbl);
        snprintf(break_lbl,64,".Lfor%d",le);snprintf(cont_lbl,64,".Lfor%d",lp);
        int sv=nvars;
        gen_stmt64(n->for_init);
        out(".Lfor%d:\n",ls);gen_expr64(n->cond);out("    testq %%rax,%%rax\n    je .Lfor%d\n",le);
        for(int i=0;i<n->body.n;i++)gen_stmt64(n->body.d[i]);
        out(".Lfor%d:\n",lp);gen_stmt64(n->for_post);
        out("    jmp .Lfor%d\n.Lfor%d:\n",ls,le);
        nvars=sv;
        strcpy(break_lbl,ob);strcpy(cont_lbl,oc); break;
    }
    case N_BREAK:
        if(!break_lbl[0])die("%s:%d: break outside loop",n->file,n->line);
        out("    jmp %s\n",break_lbl); break;
    case N_CONTINUE:
        if(!cont_lbl[0])die("%s:%d: continue outside loop",n->file,n->line);
        out("    jmp %s\n",cont_lbl); break;
    default:die("gen_stmt64: unhandled kind %d",n->kind);
    }
}

static void gen_func64(Node *fn){
    nvars=0;frame_sz=0;
    memset(break_lbl,0,sizeof break_lbl);memset(cont_lbl,0,sizeof cont_lbl);

    /* parameters (System V AMD64): integer-class args arrive in
       rdi,rsi,rdx,rcx,r8,r9 and float/double args in xmm0..xmm7 — each class
       is counted separately — everything else is on the stack above rbp+16. */
    cur_ret_type=fn->rettype;
    for(int i=0;i<fn->params.n;i++){
        int off=alloc_var(fn->params.d[i].name,fn->params.d[i].type);
        (void)off;
    }

    /* generate body into temp buffer; param spills go first, inside it */
    size_t body_start=out_len;
    {
        int ni=0,nf=0,nstack=0;
        for(int i=0;i<fn->params.n;i++){
            Var *v=&vars[i]; /* params are allocated first */
            char mem[48];snprintf(mem,sizeof mem,"%d(%%rbp)",v->offset);
            int k=kcls64(v->type);
            if(k==K_I){
                if(ni<6)out("    movq %%%s,%s\n",argregs64[ni++],mem);
                else{
                    out("    movq %d(%%rbp),%%rax\n    movq %%rax,%s\n",16+8*nstack++,mem);
                }
            }else{
                if(nf<8){
                    out(k==K_D?"    movsd %%xmm%d,%s\n":"    movss %%xmm%d,%s\n",nf++,mem);
                }else{
                    out("    movq %d(%%rbp),%%rax\n    movq %%rax,%s\n",16+8*nstack++,mem);
                }
            }
        }
    }
    for(int i=0;i<fn->body.n;i++)gen_stmt64(fn->body.d[i]);

    size_t body_len=out_len-body_start;
    char *body_asm=malloc(body_len+1);
    memcpy(body_asm,out_buf+body_start,body_len);body_asm[body_len]=0;
    out_len=body_start;out_buf[out_len]=0;

    int fsz=(frame_sz+15)&~15;

    out("%s %s\n%s:\n",fn->is_static?".local":".globl",fn->fname,fn->fname);
    out("    pushq %%rbp\n    movq %%rsp,%%rbp\n");
    if(fsz>0) out("    subq $%d,%%rsp\n",fsz);
    /* save callee-saved registers */
    out("    pushq %%rbx\n");
    out("    pushq %%r13\n    pushq %%r14\n    pushq %%r15\n");

    /* paste body */
    if(out_len+body_len+1>out_cap){out_cap=out_cap*2+body_len+64;out_buf=realloc(out_buf,out_cap);}
    memcpy(out_buf+out_len,body_asm,body_len);out_len+=body_len;out_buf[out_len]=0;free(body_asm);

    if(strcmp(fn->fname,"main")==0){
        if(!freestanding){
            out("    popq %%r15\n    popq %%r14\n    popq %%r13\n    popq %%rbx\n");
            out("    xorl %%edi,%%edi\n    movl $60,%%eax\n    syscall\n");
        } else {
            out("    popq %%r15\n    popq %%r14\n    popq %%r13\n    popq %%rbx\n");
            out("    cli\n.Lhlt_loop:\n    hlt\n    jmp .Lhlt_loop\n");
        }
    } else {
        out("    popq %%r15\n    popq %%r14\n    popq %%r13\n    popq %%rbx\n");
        out("    leave\n    ret\n");
    }
    out("\n");
}

/* ── 64-bit runtime ──────────────────────────────────────────────── */
static void emit_runtime64(void){
    /* Same rationale as emit_runtime() above: _start must exist for any
       hosted program regardless of has_std. */
    if(!freestanding){
        out(".globl _start\n_start:\n");
        out("    xorl %%ebp,%%ebp\n");
        if(has_std){
            /* Build a Falcon-shaped argv array (16-byte header + 8-byte
               elements) from the kernel's raw argc/argv, System V AMD64
               entry layout: [rsp]=argc, [rsp+8..]=argv[0..]. Keep values
               in callee-saved regs (r12/r13/r14) so they survive the
               call to _flr_alloc (which clobbers caller-saved regs). */
            out("    movq (%%rsp),%%r12\n");        /* r12 = argc */
            out("    leaq 8(%%rsp),%%r13\n");        /* r13 = &argv[0] */
            out("    movq %%r12,%%rax\n");
            out("    imulq $8,%%rax,%%rax\n");
            out("    addq $16,%%rax\n");
            out("    movq %%rax,%%rdi\n");
            out("    call _flr_alloc\n");
            out("    movq %%rax,%%r14\n");           /* r14 = argv array ptr */
            out("    movq %%r12,(%%r14)\n");
            out("    movq %%r12,8(%%r14)\n");
            out("    xorq %%rcx,%%rcx\n");
            out(".Lflr_argv_copy64:\n");
            out("    cmpq %%r12,%%rcx\n");
            out("    jge .Lflr_argv_done64\n");
            out("    movq (%%r13,%%rcx,8),%%rdx\n");
            out("    movq %%rdx,16(%%r14,%%rcx,8)\n");
            out("    incq %%rcx\n");
            out("    jmp .Lflr_argv_copy64\n");
            out(".Lflr_argv_done64:\n");
            out("    movq %%r12,%%rdi\n");           /* argc -> 1st arg */
            out("    movq %%r14,%%rsi\n");           /* argv -> 2nd arg */
            out("    call main\n");
        } else {
            out("    call main\n");
        }
        out("    movl %%eax,%%edi\n");
        out("    movl $60,%%eax\n");
        out("    syscall\n\n");
    }
    if(!has_std||freestanding)return;
    out("# ── Falcon Runtime 64-bit (inline) ───────────────────────────\n");
    /* memset(ptr:rdi, val:rsi, n:rdx) */
    out("_flr_memset:\n");
    out("    movq %%rdi,%%rax\n    movq %%rdx,%%rcx\n    movb %%sil,%%al\n");
    out("    rep stosb\n    ret\n\n");

    /* memcpy(dst:rdi, src:rsi, n:rdx) */
    out("_flr_memcpy:\n");
    out("    movq %%rdx,%%rcx\n    rep movsb\n    ret\n\n");

    /* print_str(s:rdi) */
    out("_flr_print_str:\n");
    out("    pushq %%rbx\n");
    out("    movq %%rdi,%%rbx\n");
    /* strlen: walk rsi until NUL */
    out("    movq %%rdi,%%rsi\n");
    out(".Lfps64_l:\n    cmpb $0,(%%rsi)\n    je .Lfps64_d\n    incq %%rsi\n    jmp .Lfps64_l\n");
    out(".Lfps64_d:\n    subq %%rbx,%%rsi\n    movq %%rsi,%%rdx\n"); /* rdx = len */
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    movq %%rbx,%%rsi\n    syscall\n");
    /* newline */
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    leaq .Lflr_nl(%%rip),%%rsi\n    movq $1,%%rdx\n    syscall\n");
    out("    popq %%rbx\n    ret\n\n");

    /* print_int(n:rdi) */
    out("_flr_print_int:\n");
    out("    pushq %%rbx\n    pushq %%r12\n    pushq %%r13\n");
    out("    movq %%rdi,%%rax\n");
    out("    leaq .Lflr_ibuf+22(%%rip),%%r12\n    movb $10,(%%r12)\n    decq %%r12\n");
    out("    testq %%rax,%%rax\n    jge .Lfpi64_pos\n");
    out("    negq %%rax\n    movq $1,%%r13\n    jmp .Lfpi64_l\n");
    out(".Lfpi64_pos:\n    xorl %%r13d,%%r13d\n");
    out(".Lfpi64_l:\n    movq $10,%%rcx\n    xorl %%edx,%%edx\n    divq %%rcx\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%r12)\n    decq %%r12\n");
    out("    testq %%rax,%%rax\n    jne .Lfpi64_l\n");
    out("    testq %%r13,%%r13\n    je .Lfpi64_nom\n");
    out("    movb $45,(%%r12)\n    decq %%r12\n");
    out(".Lfpi64_nom:\n    incq %%r12\n");
    out("    leaq .Lflr_ibuf+23(%%rip),%%rdx\n    subq %%r12,%%rdx\n");
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    movq %%r12,%%rsi\n    syscall\n");
    out("    popq %%r13\n    popq %%r12\n    popq %%rbx\n    ret\n\n");

    /* exit(code:rdi) */
    out("_flr_exit:\n    movl $60,%%eax\n    syscall\n\n");

    /* print_float(val in xmm0) — widen to double and print */
    out("_flr_print_float:\n");
    out("    cvtss2sd %%xmm0,%%xmm0\n");
    out("    call _flr_print_double\n    ret\n\n");

    /* print_double(val in xmm0) — integer digits + 6 decimal places.
       Same scheme as the 32-bit version: round v*1e6 to an integer first,
       then split by 1e6, so a fraction that rounds up carries into the
       integer part (2.9999999 -> "3.000000", not "2.000000"). Values >= 9e12
       skip the scaling; NaN / +-inf print as nan / inf. */
    out("_flr_print_double:\n");
    out("    pushq %%rbx\n    pushq %%r12\n    pushq %%r13\n    pushq %%r14\n");
    out("    movq %%xmm0,%%rax\n    movq %%rax,%%rcx\n    shrq $52,%%rcx\n    andl $0x7ff,%%ecx\n");
    out("    cmpl $0x7ff,%%ecx\n    je .Lfpd64_special\n");
    out("    testq %%rax,%%rax\n    jns .Lfpd64_pos\n");
    out("    btrq $63,%%rax\n    movq %%rax,%%xmm0\n");
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    leaq .Lflr_minus(%%rip),%%rsi\n    movq $1,%%rdx\n    syscall\n");
    out(".Lfpd64_pos:\n");
    out("    movsd .Lflr_big(%%rip),%%xmm1\n    ucomisd %%xmm1,%%xmm0\n    jae .Lfpd64_big\n");
    out("    mulsd .Lflr_1e6(%%rip),%%xmm0\n    cvtsd2si %%xmm0,%%rax\n");
    out("    xorl %%edx,%%edx\n    movq $1000000,%%rcx\n    divq %%rcx\n");
    out("    movq %%rdx,%%r14\n    movq %%rax,%%rdi\n    jmp .Lfpd64_emit\n");
    out(".Lfpd64_big:\n");
    out("    cvttsd2si %%xmm0,%%rdi\n    xorl %%r14d,%%r14d\n");
    out("    testq %%rdi,%%rdi\n    jns .Lfpd64_bok\n    movabsq $0x7fffffffffffffff,%%rdi\n    jmp .Lfpd64_emit\n");
    /* fraction = v - trunc(v), scaled by 1e6 and rounded; carry into the integer part */
    out(".Lfpd64_bok:\n    cvtsi2sdq %%rdi,%%xmm1\n    subsd %%xmm1,%%xmm0\n    mulsd .Lflr_1e6(%%rip),%%xmm0\n    cvtsd2si %%xmm0,%%r14\n");
    out("    cmpq $1000000,%%r14\n    jb .Lfpd64_emit\n    xorl %%r14d,%%r14d\n    incq %%rdi\n");
    out(".Lfpd64_emit:\n    call _flr_print_int_nonl\n");
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    leaq .Lflr_dot(%%rip),%%rsi\n    movq $1,%%rdx\n    syscall\n");
    out("    movq %%r14,%%rax\n");
    out("    leaq .Lflr_ibuf+12(%%rip),%%r12\n    decq %%r12\n    movl $6,%%ecx\n");
    out(".Lfpd64_fl:\n    movq $10,%%r13\n    xorl %%edx,%%edx\n    divq %%r13\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%r12)\n    decq %%r12\n    loop .Lfpd64_fl\n");
    out("    incq %%r12\n");
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    movq %%r12,%%rsi\n    movq $6,%%rdx\n    syscall\n");
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    leaq .Lflr_nl(%%rip),%%rsi\n    movq $1,%%rdx\n    syscall\n");
    out("    jmp .Lfpd64_ret\n");
    out(".Lfpd64_special:\n");
    out("    movq %%xmm0,%%rax\n    movabsq $0x000fffffffffffff,%%rcx\n    andq %%rcx,%%rax\n    jne .Lfpd64_nan\n");
    out("    movq %%xmm0,%%rax\n    testq %%rax,%%rax\n    js .Lfpd64_ninf\n");
    out("    leaq .Lflr_inf(%%rip),%%rsi\n    movq $4,%%rdx\n    jmp .Lfpd64_sp_w\n");
    out(".Lfpd64_ninf:\n    leaq .Lflr_ninf(%%rip),%%rsi\n    movq $5,%%rdx\n    jmp .Lfpd64_sp_w\n");
    out(".Lfpd64_nan:\n    leaq .Lflr_nan(%%rip),%%rsi\n    movq $4,%%rdx\n");
    out(".Lfpd64_sp_w:\n    movq $1,%%rax\n    movq $1,%%rdi\n    syscall\n");
    out(".Lfpd64_ret:\n");
    out("    popq %%r14\n    popq %%r13\n    popq %%r12\n    popq %%rbx\n    ret\n\n");

    /* print_int_nonl — print int in rdi without newline (helper for double) */
    out("_flr_print_int_nonl:\n");
    out("    pushq %%rbx\n    pushq %%r12\n    pushq %%r13\n");
    out("    movq %%rdi,%%rax\n");
    out("    leaq .Lflr_ibuf+22(%%rip),%%r12\n    movb $0,(%%r12)\n    decq %%r12\n");
    out("    testq %%rax,%%rax\n    jge .Lpni64_pos\n");
    out("    negq %%rax\n    movq $1,%%r13\n    jmp .Lpni64_l\n");
    out(".Lpni64_pos:\n    xorl %%r13d,%%r13d\n");
    out(".Lpni64_l:\n    movq $10,%%rcx\n    xorl %%edx,%%edx\n    divq %%rcx\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%r12)\n    decq %%r12\n");
    out("    testq %%rax,%%rax\n    jne .Lpni64_l\n");
    out("    testq %%r13,%%r13\n    je .Lpni64_nom\n");
    out("    movb $45,(%%r12)\n    decq %%r12\n");
    out(".Lpni64_nom:\n    incq %%r12\n");
    out("    leaq .Lflr_ibuf+22(%%rip),%%rdx\n    subq %%r12,%%rdx\n");
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    movq %%r12,%%rsi\n    syscall\n");
    out("    popq %%r13\n    popq %%r12\n    popq %%rbx\n    ret\n\n");

    /* strlen */
    out("_flr_strlen:\n");
    out("    movq %%rdi,%%rax\n");
    out(".Lflrsl64:\n    cmpb $0,(%%rax)\n    je .Lflrsld64\n    incq %%rax\n    jmp .Lflrsl64\n");
    out(".Lflrsld64:\n    subq %%rdi,%%rax\n    ret\n\n");
    out("_flr_str_len:\n    jmp _flr_strlen\n\n");

    /* str_eq(a:rdi, b:rsi) */
    out("_flr_str_eq:\n");
    out(".Lfseq64:\n    movzbl (%%rdi),%%eax\n    movzbl (%%rsi),%%ecx\n");
    out("    cmpl %%ecx,%%eax\n    jne .Lfseq64_no\n    testl %%eax,%%eax\n    je .Lfseq64_yes\n");
    out("    incq %%rdi\n    incq %%rsi\n    jmp .Lfseq64\n");
    out(".Lfseq64_yes:\n    movl $1,%%eax\n    ret\n");
    out(".Lfseq64_no:\n    xorl %%eax,%%eax\n    ret\n\n");

    /* int_to_str(n:rdi) -> rax (static buffer) */
    out("_flr_int_to_str:\n");
    out("    pushq %%rbx\n    pushq %%r12\n");
    out("    movq %%rdi,%%rax\n");
    out("    leaq .Lflr_ibuf+11(%%rip),%%r12\n    movb $0,(%%r12)\n    decq %%r12\n");
    out("    testq %%rax,%%rax\n    jge .Lfits64_p\n    negq %%rax\n    movq $1,%%rbx\n    jmp .Lfits64_l\n");
    out(".Lfits64_p:\n    xorl %%ebx,%%ebx\n");
    out(".Lfits64_l:\n    movq $10,%%rcx\n    xorl %%edx,%%edx\n    divq %%rcx\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%r12)\n    decq %%r12\n");
    out("    testq %%rax,%%rax\n    jne .Lfits64_l\n");
    out("    testq %%rbx,%%rbx\n    je .Lfits64_n\n    movb $45,(%%r12)\n    decq %%r12\n");
    out(".Lfits64_n:\n    incq %%r12\n    movq %%r12,%%rax\n");
    out("    popq %%r12\n    popq %%rbx\n    ret\n\n");

    /* str_to_int(s:rdi) -> rax */
    out("_flr_str_to_int:\n");
    out("    xorl %%eax,%%eax\n    xorl %%ecx,%%ecx\n");
    out("    cmpb $45,(%%rdi)\n    jne .Lfsti64_l\n    movl $1,%%ecx\n    incq %%rdi\n");
    out(".Lfsti64_l:\n    movzbl (%%rdi),%%edx\n    cmpl $48,%%edx\n    jl .Lfsti64_d\n");
    out("    cmpl $57,%%edx\n    jg .Lfsti64_d\n    imulq $10,%%rax\n    subl $48,%%edx\n    addl %%edx,%%eax\n");
    out("    incq %%rdi\n    jmp .Lfsti64_l\n");
    out(".Lfsti64_d:\n    testl %%ecx,%%ecx\n    je .Lfsti64_r\n    negq %%rax\n");
    out(".Lfsti64_r:\n    ret\n\n");

    /* abs/min/max */
    out("_flr_abs:\n    movq %%rdi,%%rax\n    testq %%rax,%%rax\n    jge .Labs64_ok\n    negq %%rax\n.Labs64_ok:\n    ret\n\n");
    out("_flr_min:\n    movq %%rdi,%%rax\n    cmpq %%rsi,%%rax\n    jle .Lmin64_ok\n    movq %%rsi,%%rax\n.Lmin64_ok:\n    ret\n\n");
    out("_flr_max:\n    movq %%rdi,%%rax\n    cmpq %%rsi,%%rax\n    jge .Lmax64_ok\n    movq %%rsi,%%rax\n.Lmax64_ok:\n    ret\n\n");

    /* assert(cond:rdi, msg:rsi) */
    out("_flr_assert:\n");
    out("    testq %%rdi,%%rdi\n    jne .Lassert64_ok\n");
    out("    movq $1,%%rax\n    movq $2,%%rdi\n    leaq .Lflr_assert_msg(%%rip),%%rsi\n    movq $19,%%rdx\n    syscall\n");
    out("    pushq %%rsi\n    call _flr_strlen\n    popq %%rsi\n");
    out("    movq %%rax,%%rdx\n    movq $1,%%rax\n    movq $2,%%rdi\n    syscall\n");
    out("    movq $1,%%rax\n    movq $2,%%rdi\n    leaq .Lflr_nl(%%rip),%%rsi\n    movq $1,%%rdx\n    syscall\n");
    out("    movq $60,%%rax\n    movq $1,%%rdi\n    syscall\n");
    out(".Lassert64_ok:\n    ret\n\n");

    /* alloc(size:rdi) -> rax  — uses mmap(NULL,size,PROT_RW,MAP_ANON|MAP_PRIV,-1,0) */
    out("_flr_alloc:\n");
    out("    pushq %%rbx\n");
    out("    movq %%rdi,%%rsi\n");            /* length */
    out("    addq $7,%%rsi\n    andq $-8,%%rsi\n"); /* align to 8 */
    out("    xorl %%edi,%%edi\n");             /* addr = NULL */
    out("    movl $3,%%edx\n");               /* PROT_READ|PROT_WRITE */
    out("    movl $0x22,%%r10d\n");            /* MAP_PRIVATE|MAP_ANONYMOUS */
    out("    movq $-1,%%r8\n");               /* fd = -1 */
    out("    xorl %%r9d,%%r9d\n");             /* offset = 0 */
    out("    movl $9,%%eax\n");               /* sys_mmap */
    out("    syscall\n");
    out("    popq %%rbx\n    ret\n\n");
    out("_flr_free:\n    ret\n\n");           /* no-op bump */

    /* str_concat(a:rdi, b:rsi) -> rax */
    out("_flr_str_concat:\n");
    out("    pushq %%rbx\n    pushq %%r12\n    pushq %%r13\n");
    out("    movq %%rdi,%%r12\n    movq %%rsi,%%r13\n");
    out("    call _flr_strlen\n    movq %%rax,%%rbx\n"); /* len(a) */
    out("    movq %%r13,%%rdi\n    call _flr_strlen\n");  /* len(b) */
    out("    addq %%rbx,%%rax\n    incq %%rax\n");        /* total */
    out("    movq %%rax,%%rdi\n    call _flr_alloc\n");
    out("    pushq %%rax\n");
    /* copy a */
    out("    movq %%rax,%%rdi\n    movq %%r12,%%rsi\n");
    out(".Lsca64_l:\n    movzbl (%%rsi),%%ecx\n    testl %%ecx,%%ecx\n    je .Lsca64_d\n");
    out("    movb %%cl,(%%rdi)\n    incq %%rsi\n    incq %%rdi\n    jmp .Lsca64_l\n");
    out(".Lsca64_d:\n");
    /* copy b */
    out("    movq %%r13,%%rsi\n");
    out(".Lscb64_l:\n    movzbl (%%rsi),%%ecx\n    movb %%cl,(%%rdi)\n    testl %%ecx,%%ecx\n    je .Lscb64_d\n");
    out("    incq %%rsi\n    incq %%rdi\n    jmp .Lscb64_l\n");
    out(".Lscb64_d:\n");
    out("    popq %%rax\n");
    out("    popq %%r13\n    popq %%r12\n    popq %%rbx\n    ret\n\n");

    /* str_format(fmt:rdi, nargs:rsi, arg0...) -> rax */
    out("_flr_str_format:\n");
    out("    pushq %%rbx\n    pushq %%r12\n    pushq %%r13\n    pushq %%r14\n    pushq %%r15\n");
    out("    movq %%rdi,%%r12\n");   /* fmt */
    out("    movq %%rsi,%%r13\n");   /* nargs */
    out("    leaq 48(%%rsp),%%r14\n"); /* &arg0 on stack: 5 pushq (40) + return addr (8) = 48 */
    /* alloc 512 output bytes */
    out("    movq $512,%%rdi\n    call _flr_alloc\n");
    out("    movq %%rax,%%r15\n    pushq %%rax\n"); /* save output ptr */
    out("    movq %%rax,%%rbx\n");  /* rbx = write ptr */
    out(".Lsfmt64_l:\n");
    out("    movzbl (%%r12),%%eax\n    testl %%eax,%%eax\n    je .Lsfmt64_end\n");
    out("    cmpl $37,%%eax\n    jne .Lsfmt64_copy\n");
    out("    incq %%r12\n    movzbl (%%r12),%%eax\n");
    out("    cmpl $37,%%eax\n    je .Lsfmt64_copy\n");
    out("    cmpl $100,%%eax\n    je .Lsfmt64_d\n");
    out("    cmpl $115,%%eax\n    je .Lsfmt64_s\n");
    out("    jmp .Lsfmt64_copy\n");
    /* %d */
    out(".Lsfmt64_d:\n    incq %%r12\n");
    out("    testq %%r13,%%r13\n    je .Lsfmt64_l\n");
    /* r13 (remaining count), r14 (arg ptr), rbx (output ptr) must advance
       PERMANENTLY into the next loop iteration. _flr_int_to_str already
       properly callee-saves rbx/r12 itself and never touches r13/r14/rsi,
       so none of them need saving here — the original push/pop-everything
       wrapper was actively wrong: it silently discarded every substituted
       value and the arg pointer never advanced past arg 0. */
    out("    movq (%%r14),%%rdi\n    addq $8,%%r14\n    decq %%r13\n");
    out("    call _flr_int_to_str\n    movq %%rax,%%rsi\n");
    out(".Lsfmt64_dc:\n    movzbl (%%rsi),%%eax\n    testl %%eax,%%eax\n    je .Lsfmt64_dr\n");
    out("    movb %%al,(%%rbx)\n    incq %%rsi\n    incq %%rbx\n    jmp .Lsfmt64_dc\n");
    out(".Lsfmt64_dr:\n");
    out("    jmp .Lsfmt64_l\n");
    /* %s */
    out(".Lsfmt64_s:\n    incq %%r12\n");
    out("    testq %%r13,%%r13\n    je .Lsfmt64_l\n");
    out("    movq (%%r14),%%rsi\n    addq $8,%%r14\n    decq %%r13\n");
    out(".Lsfmt64_sc:\n    movzbl (%%rsi),%%eax\n    testl %%eax,%%eax\n    je .Lsfmt64_sr\n");
    out("    movb %%al,(%%rbx)\n    incq %%rsi\n    incq %%rbx\n    jmp .Lsfmt64_sc\n");
    out(".Lsfmt64_sr:\n");
    out("    jmp .Lsfmt64_l\n");
    /* plain copy */
    out(".Lsfmt64_copy:\n    movb %%al,(%%rbx)\n    incq %%r12\n    incq %%rbx\n    jmp .Lsfmt64_l\n");
    out(".Lsfmt64_end:\n    movb $0,(%%rbx)\n");
    out("    popq %%rax\n"); /* return result ptr */
    out("    popq %%r15\n    popq %%r14\n    popq %%r13\n    popq %%r12\n    popq %%rbx\n    ret\n\n");
}

static void emit_strlit(const char *s){
    out("    .ascii \"");
    for(const char *p=s;*p;p++){
        if     (*p=='\n')out("\\n");
        else if(*p=='\t')out("\\t");
        else if(*p=='\\')out("\\\\");
        else if(*p=='"' )out("\\\"");
        else if(*p=='\0')out("\\0");
        else             out("%c",*p);
    }
    out("\\0\"\n");
}

static void emit_data(void){
    out(".section .data\n");
    if((has_std&&!freestanding)||long_helpers_emitted){
        out(".Lflr_ibuf:\n    .space 32\n");
        out(".Lflr_nl:\n    .byte 10\n");
        out(".Lflr_dot:\n    .ascii \".\"\n");
        out(".Lflr_minus:\n    .ascii \"-\"\n");
        /* 1000000.0 as IEEE-754 double (little-endian 64-bit) */
        out(".Lflr_1e6:\n    .long 0\n    .long 1093567616\n");
        /* 9.0e12 — above this v*1e6 would overflow int64, so print_double skips the scaling */
        out(".Lflr_big:\n    .long 2602565632\n    .long 1117806323\n");
        out(".Lflr_nan:\n    .ascii \"nan\\n\"\n");
        out(".Lflr_inf:\n    .ascii \"inf\\n\"\n");
        out(".Lflr_ninf:\n    .ascii \"-inf\\n\"\n");
        out(".Lflr_assert_msg:\n    .ascii \"assertion failed: \"\n");
    }
    out(".section .rodata\n");
    for(int i=0;i<nstr_lits;i++){out(".Lstr%d:\n",i);emit_strlit(str_lits[i]);}
    for(int i=0;i<nflits;i++){
        out(".Lfl%d:\n",i);
        if(flits[i].is_double){
            union{double d;unsigned u[2];}cv;cv.d=flits[i].val;
            out("    .long %u\n    .long %u\n",cv.u[0],cv.u[1]);
        } else {
            union{float f;unsigned u;}cv;cv.f=(float)flits[i].val;
            out("    .long %u\n",cv.u);
        }
    }
    out(".Lfl_zero:\n    .long 0\n    .long 0\n");
    /* emit global variables */
    if(ngvars>0){
        int any_local=0;
        for(int i=0;i<ngvars;i++)if(!gvars[i].is_extern){any_local=1;break;}
        if(any_local)out(".section .data\n");
        for(int i=0;i<ngvars;i++){
            if(gvars[i].is_extern){
                /* declared here, defined in another object file: no storage,
                   just tell the assembler the symbol is external. */
                out(".extern _gv_%s\n",gvars[i].name);
                continue;
            }
            /* On x86-64 every slot is 8 bytes, same as alloc_var's local-slot
               convention — a plain `int` global can legally hold a heap
               address from _flr_alloc (or any other 64-bit value that fits
               through the same int/long widening), so it must not be
               narrowed to 4 bytes just because it wasn't declared `long`.
               On x86-32, int is already the native pointer width, so the
               original int(4)/long+double(8) split still applies. */
            int sz=4;
            if(arch==ARCH_X86_64)sz=8;
            else if(gvars[i].type&&(gvars[i].type->kind==TY_LONG||gvars[i].type->kind==TY_DOUBLE))sz=8;
            out("%s _gv_%s\n_gv_%s:\n",gvars[i].is_static?".local":".globl",gvars[i].name,gvars[i].name);
            if(gvars[i].type&&gvars[i].type->kind==TY_DOUBLE){
                /* always 8 bytes: raw IEEE-754 double bit pattern, same
                   approach as the .Lfl%d float-literal constant pool above */
                union{double d;unsigned u[2];}cv;cv.d=gvars[i].dval;
                out("    .long %u\n    .long %u\n",cv.u[0],cv.u[1]);
            } else if(gvars[i].type&&gvars[i].type->kind==TY_FLOAT){
                union{float f;unsigned u;}cv;cv.f=(float)gvars[i].dval;
                if(sz==8)out("    .long %u\n    .long 0\n",cv.u); /* pad to uniform 8-byte x86-64 slot; only the low 4 bytes are the float (matches how movss reads/writes local float slots) */
                else     out("    .long %u\n",cv.u);
            } else if(sz==8){
                out("    .quad %lld\n",(long long)gvars[i].ival);
            } else {
                out("    .long %d\n",(int)gvars[i].ival);
            }
        }
    }
}

static void codegen(Node *prog){
    /* structs and has_std already registered by typecheck; re-scan for has_std */
    for(int i=0;i<prog->body.n;i++){
        Node *n=prog->body.d[i];
        if(n->kind==N_IMPORT&&n->import_path&&strcmp(n->import_path,"std")==0)has_std=1;
    }
    out(".section .text\n\n");
    if(arch==ARCH_X86_64){
        for(int i=0;i<prog->body.n;i++){
            Node *n=prog->body.d[i];if(n->kind==N_FUNC&&!n->is_extern)gen_func64(n);
        }
        emit_runtime64();
    } else {
        for(int i=0;i<prog->body.n;i++){
            Node *n=prog->body.d[i];if(n->kind==N_FUNC&&!n->is_extern)gen_func(n);
        }
        emit_runtime();
    }
    emit_data();
}

/* ═══════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════ */

static int valid_ext(const char *p){
    const char *e[]={".fl",".fal",".flc",".flsrc",NULL};
    const char *d=strrchr(p,'.');if(!d)return 0;
    for(int i=0;e[i];i++)if(strcmp(d,e[i])==0)return 1;
    return 0;
}

int main(int argc,char **argv){
    const char *infile=NULL,*outfile=NULL;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--freestanding")==0)     freestanding=1;
        else if(strcmp(argv[i],"-o")==0&&i+1<argc)  outfile=argv[++i];
        else if(strncmp(argv[i],"-I",2)==0)          add_search(argv[i]+2);
        else if(strcmp(argv[i],"-arch")==0&&i+1<argc){
            i++;
            if(strcmp(argv[i],"x86-64-linux")==0)       arch=ARCH_X86_64;
            else if(strcmp(argv[i],"x86-32-linux")==0)  arch=ARCH_X86_32;
            else die("unknown arch '%s' (valid: x86-32-linux, x86-64-linux)",argv[i]);
        }
        else if(!infile)                             infile=argv[i];
    }
    if(!infile){
        fprintf(stderr,
            "falconc v4 — Falcon language compiler\n"
            "usage: falconc <input.fl> [-o out.s] [-arch <target>] [--freestanding] [-I<path>...]\n\n"
            "targets:\n"
            "  -arch x86-32-linux   32-bit Linux, cdecl, int 0x80  (default)\n"
            "  -arch x86-64-linux   64-bit Linux, System V AMD64, syscall\n\n"
            "new in v4: -arch x86-64-linux\n"
            "new in v3: float, double, let, const\n"
            "new in v2: long, type checker, str_concat, str_format\n\n"
            "32-bit linking:\n"
            "  as --32 out.s -o out.o\n"
            "  ld -m elf_i386 flr.o out.o -o prog\n\n"
            "64-bit linking:\n"
            "  as out.s -o out.o\n"
            "  ld out.o -o prog\n"
        );
        return 1;
    }
    if(!valid_ext(infile))die("unrecognised extension (expected .fl .fal .flc .flsrc)");

    char *indir=path_dir(infile);add_search(indir);free(indir);

    char *src=read_file(infile);
    if(!src)die("cannot open '%s'",infile);
    mark_imported(infile);
    tokenize(src,infile);
    free(src);
    expand_imports(0,infile);
    emit_tok(TT_EOF,"",0,infile);

    Node *prog=parse_program();
    typecheck(prog);   /* ← new: type-check before codegen */
    codegen(prog);

    if(outfile){
        FILE *f=fopen(outfile,"w");
        if(!f)die("cannot write '%s'",outfile);
        fwrite(out_buf,1,out_len,f);fclose(f);
        fprintf(stderr,"falconc: wrote %zu bytes to %s\n",out_len,outfile);
    }else{
        fwrite(out_buf,1,out_len,stdout);
    }
    return 0;
}
