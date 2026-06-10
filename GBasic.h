// ================================================================
// gbasic.h  --  GBasic Interpreter, Editor, and Runner for GigaOS
// v1.0 beta  |  Arduino GIGA R1 WiFi  |  STM32H747XI
//
// Plain-english BASIC interpreter. Runs on M7, low-priority threads.
// SDRAM layout (included here is 0x60200000..0x607FFFFF):
//   0x60200000  2MB  variable stores  (8 scripts x 256KB)
//   0x60400000  3MB  temp filesystem  (.gbpf .gbf .ram)
//   0x60700000  1MB  reserved
//   hello its me the human making comments. i dont know why that 1mb is reserved.
//   also theres random arbitrary decisiosn i made. its fine. probably. idk.
// ================================================================
#pragma once
#include <Wire.h>
#include <math.h>
#include <stdarg.h>
#include "mbed.h"

// GigaOS app mode -- 0=shell, 1=gbasic_edit, 2=gbasic_run
// gOSMode is declared volatile int in GigaOS.ino
#define GMODE_SHELL       0
#define GMODE_GBASIC_EDIT 1
#define GMODE_GBASIC_RUN  2

// -- SDRAM addresses -----------------------------------------------
#define GB_VAR_BASE        0x60200000UL
#define GB_VAR_SLOT_SIZE   (256*1024UL)    // 256KB per script slot
#define GB_VAR_TOTAL       (2*1024*1024UL) // 2MB region total
#define GB_MAX_SCRIPTS     8

#define GB_TEMPFS_BASE     0x60400000UL    // 3MB temp FS
#define GB_TEMPFS_SIZE     (3*1024*1024UL)
#define GB_TEMPFS_MAGIC    0x47425346UL    // "GBSF"
#define GB_TEMPFS_MAXFILES 256

// -- Interpreter limits --------------------------------------------
#define GB_MAX_LINES       2048
#define GB_MAX_LINE_LEN    256
#define GB_MAX_VARS        512
#define GB_MAX_LABELS      256
#define GB_CTRL_DEPTH      32
#define GB_CALL_DEPTH      32
#define GB_UNDO_LEVELS     20
#define GB_MAX_SCHED       16
#define GB_MAX_PWM         8
#define GB_RUNNER_LINES    120

// -- Enums ---------------------------------------------------------
enum GBVarType  { GBV_NUM=0, GBV_STR=1, GBV_LIST=2, GBV_DEL=3 };
enum GBFrmType  { GBF_FOR=0, GBF_WHILE=1, GBF_REPEAT=2, GBF_IF=3 };
enum GBRunMode  { GBR_FG=0, GBR_BG=1 };

// -- GBVar -- 64 bytes, lives in SDRAM var slot ---------------------
struct GBVar {
    char    name[28];
    uint8_t type;
    uint8_t isPrivate;
    uint8_t ownerIdx;
    uint8_t _pad0;
    union {
        double num;
        struct { uint32_t off; uint16_t len; uint16_t cap; } str;
    };
    uint8_t _pad1[16];
};

// -- GBVarStore -- header at base of each 256KB SDRAM slot ----------
struct GBVarStore {
    uint32_t magic;          // 0x47424153 "GBAS"
    uint16_t varCount;
    uint32_t heapTop;        // next free byte offset within this slot
    uint8_t  slotIdx;
    uint8_t  _pad[53];       // pad to 64 bytes
    GBVar    vars[GB_MAX_VARS];
    // heap data follows (offset heapTop from slot base)
};

// -- GBFrame -- control flow stack entry ---------------------------
struct GBFrame {
    GBFrmType type;
    int  startLine, endLine, elseLine;
    char forVar[32];
    double forLimit, forStep;
    char whileCond[GB_MAX_LINE_LEN];
};

// -- GBLabel -------------------------------------------------------
struct GBLabel { char name[32]; int line; };

// -- TempFS structs ------------------------------------------------
struct GBFSEntry {
    char     name[48];
    uint32_t offset;       // byte offset from GB_TEMPFS_BASE
    uint32_t size;
    uint32_t capacity;
    char     owner[48];    // owning script name, or "" if none
    bool     leaveBehind;
    bool     abandoned;
    uint32_t createdMs;
    uint32_t modifiedMs;
    bool     active;
    uint8_t  _pad[13];
};

struct GBFSHeader {
    uint32_t   magic;
    uint16_t   count;
    uint32_t   dataStart;  // first byte for file data
    uint32_t   dataTop;    // next free byte
    uint8_t    _pad[50];
    GBFSEntry  files[GB_TEMPFS_MAXFILES];
};

// -- GBVal -- runtime value (stack/return) -------------------------
struct GBVal {
    bool   isStr;
    double num;
    char   str[GB_MAX_LINE_LEN];
};

// -- GBCtx -- per-script interpreter state -------------------------
struct GBCtx {
    char     name[64];
    bool     isFg;
    uint8_t  idx;             // slot 0-7
    char*    src;             // pointer into TempFS
    uint32_t srcLen;
    uint32_t lineOff[GB_MAX_LINES];
    int      lineCount;
    GBLabel  labels[GB_MAX_LABELS];
    int      labelCount;
    int      endLines[GB_MAX_LINES];   // matching END line for block starters
    int      elseLines[GB_MAX_LINES];  // ELSE line for IF starters
    int      curLine;
    bool     running;
    volatile bool abortFlag;
    GBFrame  ctrl[GB_CTRL_DEPTH];
    int      ctrlTop;
    int      callStack[GB_CALL_DEPTH];
    int      callDepth;
    int      errHandlerLine;   // -1 = none
    bool     inErrHandler;
    GBVarStore* store;
    uint8_t  i2cClaims[16];
    int      i2cClaimCount;
    uint8_t  pwmPins[GB_MAX_PWM];
    mbed::PwmOut* pwmOuts[GB_MAX_PWM];
    int      pwmCount;
    GBRunMode declMode;       // declared run mode
    rtos::Thread* thread;
    char     threadName[64];
};

// -- GBEditor -- light-mode coding interface ------------------------
struct GBEditor {
    char   filename[64];
    char*  lines[GB_MAX_LINES];
    int    lineCount;
    int    curLine, curCol;
    int    scrollTop;
    bool   modified;
    char   statusMsg[80];
    struct { int line; char* txt; } undo[GB_UNDO_LEVELS];
    int    undoTop, undoCount;
};

// -- GBSched -- scheduled task entry -------------------------------
struct GBSched {
    char     name[64];
    uint8_t  type;       // 0=once,1=daily,2=weekly,3=interval
    uint8_t  hour, min;
    uint8_t  dow;        // day-of-week for weekly (0=Sun)
    uint32_t intervalMs;
    uint32_t lastFireMs;
    bool     active, overlapOk, firedOnce;
};

// -- Editor/runner colors ------------------------------------------
#define GB_ED_BG      0xFFFF   // white
#define GB_ED_TEXT    0x0000   // black
#define GB_ED_LNUM    0x8410   // gray
#define GB_ED_KW      0x000F   // blue
#define GB_ED_CMT     0x03E0   // green
#define GB_ED_STR     0x8000   // dark red
#define GB_ED_HDR     0x2104
#define GB_ED_STAT    0x2104
#define GB_RN_BG      0x0000
#define GB_RN_HDR     0x2104
#define GB_RN_TXT     0xC618
#define GB_RN_HTXT    0xFFFF

// -- Editor layout -------------------------------------------------
#define GB_ED_HDR_H    16
#define GB_ED_STAT_H   16
#define GB_ED_TOP      (TERM_Y + GB_ED_HDR_H)
#define GB_ED_STAT_Y   (TERM_Y + TERM_H - GB_ED_STAT_H)
#define GB_ED_ROWS     ((TERM_H - GB_ED_HDR_H - GB_ED_STAT_H) / TERM_CH)
#define GB_ED_LNUMW    (4 * TERM_CW)
#define GB_ED_CODEX    GB_ED_LNUMW

// -- Runner layout -------------------------------------------------
#define GB_RN_HDR_H    16
#define GB_RN_TOP      (TERM_Y + GB_RN_HDR_H)
#define GB_RN_ROWS     ((TERM_H - GB_RN_HDR_H - TERM_CH) / TERM_CH)
#define GB_RN_INPUT_Y  (TERM_Y + TERM_H - TERM_CH)

// -- Global state --------------------------------------------------
// Runner line struct must be defined before the pointer below
struct GBRnLine { char text[GB_MAX_LINE_LEN]; uint16_t color; };

// -- SDRAM-allocated (large, moved out of precious SRAM) -----
// Initialized in gbInit() via SDRAM.malloc(). Never nullptr after init.
static GBCtx*     gBCtx   = nullptr;   // 8 x GBCtx  (~345KB)
static GBRnLine*  gBRnBuf = nullptr;   // runner terminal buffer (~30KB)
static GBEditor*  gBEd    = nullptr;   // editor state (~16KB)
static GBSched*   gBSched = nullptr;   // schedule table (~2KB)
// -- SRAM (small, or contains RTOS objects) -------------------
static uint8_t    gBSlot[GB_MAX_SCRIPTS];  // 0=free,1=used (8 bytes)
static bool       gBEdFullDirty  = true;
static bool       gBEdLineDirty  = false;
static bool       gBEdQuitConfirm= false;
static int        gBEdPrevLine   = 0;
static bool       gBRnDirty  = true;
static GBFSHeader* gBFS      = nullptr;
static int        gBSchedN   = 0;

// gBRnBuf declared in globals above as SDRAM pointer
static int        gBRnCount   = 0;
static bool       gBRnAsk     = false;
static char       gBRnAskBuf[GB_MAX_LINE_LEN];
static int        gBRnAskLen  = 0;
static rtos::EventFlags gBRnEvt;
static rtos::Mutex gBVarMtx;

// -- Forward declarations ------------------------------------------
static void gbError(GBCtx* ctx, const char* fmt, ...);
static GBVal gbEvalAndChain(GBCtx* ctx, const char* expr);
static bool  gbEvalCond(GBCtx* ctx, const char* cond);
static void  gbExecuteLine(GBCtx* ctx, const char* line, int ln);

// ================================================================
// UTILITIES
// ================================================================
static const char* gbSkipWS(const char* p){while(*p==' '||*p=='\t')p++;return p;}

static bool gbSW(const char* s, const char* pfx){
    return strncmp(s,pfx,strlen(pfx))==0;
}

static bool gbMW(const char** p, const char* w){
    const char* s=gbSkipWS(*p);
    size_t n=strlen(w);
    if(strncmp(s,w,n)==0&&(s[n]==' '||s[n]=='\0'||s[n]=='\t'||s[n]=='(')){
        *p=gbSkipWS(s+n); return true;
    }
    return false;
}

static bool gbMS(const char** p, const char* tok){
    const char* s=gbSkipWS(*p);
    size_t n=strlen(tok);
    if(strncmp(s,tok,n)==0){*p=s+n;return true;}
    return false;
}

static void gbToStr(GBVal v, char* out, int mx){
    if(v.isStr){strncpy(out,v.str,mx-1);out[mx-1]='\0';}
    else{
        if(v.num==floor(v.num)&&fabs(v.num)<1e12)snprintf(out,mx,"%.0f",v.num);
        else snprintf(out,mx,"%g",v.num);
    }
}

static GBVal gbNV(double n){GBVal v;v.isStr=false;v.num=n;v.str[0]='\0';return v;}
static GBVal gbSV(const char* s){GBVal v;v.isStr=true;v.num=0;strncpy(v.str,s,GB_MAX_LINE_LEN-1);v.str[GB_MAX_LINE_LEN-1]='\0';return v;}
static bool  gbFalsy(GBVal v){return v.isStr?(v.str[0]=='\0'):(v.num==0);}

static void gbRnPrint(const char* text, uint16_t color=GB_RN_TXT){
    if(gBRnCount>=GB_RUNNER_LINES){
        memmove(gBRnBuf,gBRnBuf+1,sizeof(GBRnLine)*(GB_RUNNER_LINES-1));
        gBRnCount=GB_RUNNER_LINES-1;
    }
    strncpy(gBRnBuf[gBRnCount].text,text,GB_MAX_LINE_LEN-1);
    gBRnBuf[gBRnCount].color=color;
    gBRnCount++;
    gBRnDirty=true;
}

static void gbRnPrintf(uint16_t col, const char* fmt,...){
    char b[GB_MAX_LINE_LEN];va_list v;va_start(v,fmt);vsnprintf(b,sizeof(b),fmt,v);va_end(v);
    gbRnPrint(b,col);
}

static void gbOut(GBCtx* ctx, const char* text, uint16_t color=COL_TEXT){
    if(ctx->isFg) gbRnPrint(text,color);
    else termPrint(text,color);
}

// ================================================================
// TEMP FILESYSTEM
// ================================================================
static void gbInitFS(){
    gBFS=(GBFSHeader*)GB_TEMPFS_BASE;
    if(gBFS->magic!=GB_TEMPFS_MAGIC){
        memset(gBFS,0,sizeof(GBFSHeader));
        gBFS->magic=GB_TEMPFS_MAGIC;
        gBFS->dataStart=sizeof(GBFSHeader);
        gBFS->dataTop=sizeof(GBFSHeader);
    }
}

static GBFSEntry* gbFSFind(const char* name){
    if(!gBFS)return nullptr;
    for(int i=0;i<GB_TEMPFS_MAXFILES;i++)
        if(gBFS->files[i].active&&strcmp(gBFS->files[i].name,name)==0)
            return &gBFS->files[i];
    return nullptr;
}

static GBFSEntry* gbFSCreate(const char* name,const char* owner){
    if(!gBFS)return nullptr;
    for(int i=0;i<GB_TEMPFS_MAXFILES;i++){
        if(!gBFS->files[i].active){
            GBFSEntry* e=&gBFS->files[i];
            memset(e,0,sizeof(GBFSEntry));
            strncpy(e->name,name,47);
            if(owner)strncpy(e->owner,owner,47);
            e->capacity=4096;
            e->offset=gBFS->dataTop;
            gBFS->dataTop+=e->capacity;
            e->active=true;
            e->createdMs=e->modifiedMs=millis();
            gBFS->count++;
            return e;
        }
    }
    return nullptr;
}

static bool gbFSAppend(const char* name,const char* data,const char* owner){
    GBFSEntry* e=gbFSFind(name);
    if(!e)e=gbFSCreate(name,owner);
    if(!e)return false;
    if(e->owner[0]&&strcmp(e->owner,owner)!=0)return false; // access denied
    uint32_t dl=strlen(data);
    while(e->size+dl+1>e->capacity){
        if(gBFS->dataTop+4096>GB_TEMPFS_SIZE)return false;
        e->capacity+=4096;gBFS->dataTop+=4096;
    }
    char* ptr=(char*)GB_TEMPFS_BASE+e->offset+e->size;
    memcpy(ptr,data,dl);e->size+=dl;ptr[0]='\0';
    e->modifiedMs=millis();
    return true;
}

static String gbFSRead(const char* name){
    GBFSEntry* e=gbFSFind(name);
    if(!e)return String("");
    return String((char*)GB_TEMPFS_BASE+e->offset);
}

static bool gbFSDelete(const char* name,const char* requestor,bool isOS){
    GBFSEntry* e=gbFSFind(name);
    if(!e)return false;
    if(!isOS&&e->owner[0]&&strcmp(e->owner,requestor)!=0)return false;
    memset((char*)GB_TEMPFS_BASE+e->offset,0,e->size);
    memset(e,0,sizeof(GBFSEntry));
    if(gBFS->count>0)gBFS->count--;
    return true;
}

// ================================================================
// VARIABLE STORE
// ================================================================
static GBVarStore* gbGetStore(int slot){
    return(GBVarStore*)(GB_VAR_BASE+(uint32_t)slot*GB_VAR_SLOT_SIZE);
}
static int  gbAllocSlot(){for(int i=0;i<GB_MAX_SCRIPTS;i++)if(!gBSlot[i]){gBSlot[i]=1;return i;}return -1;}
static void gbFreeSlot(int i){if(i>=0&&i<GB_MAX_SCRIPTS)gBSlot[i]=0;}

static void gbInitStore(GBVarStore* s,uint8_t idx){
    memset(s,0,sizeof(GBVarStore));
    s->magic=0x47424153UL;
    s->heapTop=sizeof(GBVarStore);
    s->slotIdx=idx;
}

static uint32_t gbHAlloc(GBVarStore* s,uint32_t sz){
    sz=(sz+3)&~3;
    uint32_t nt=s->heapTop+sz;
    if(nt>GB_VAR_SLOT_SIZE)return 0;
    uint32_t off=s->heapTop;s->heapTop=nt;return off;
}
static char* gbHP(GBVarStore* s,uint32_t off){return(char*)s+off;}

static GBVar* gbFindVar(GBVarStore* s,const char* name,uint8_t idx){
    for(int i=0;i<s->varCount;i++){
        if(s->vars[i].type==GBV_DEL)continue;
        if(strcmp(s->vars[i].name,name)==0){
            if(s->vars[i].isPrivate&&s->vars[i].ownerIdx!=idx)return nullptr;
            return&s->vars[i];
        }
    }
    return nullptr;
}

static GBVar* gbMakeVar(GBVarStore* s,const char* name,uint8_t idx){
    if(s->varCount>=GB_MAX_VARS)return nullptr;
    GBVar* v=&s->vars[s->varCount++];
    memset(v,0,sizeof(GBVar));
    strncpy(v->name,name,27);v->ownerIdx=idx;
    return v;
}

static GBVar* gbGetMake(GBVarStore* s,const char* name,uint8_t idx){
    GBVar* v=gbFindVar(s,name,idx);
    return v?v:gbMakeVar(s,name,idx);
}

static GBVal gbGetVal(GBVarStore* s,const char* name,uint8_t idx){
    GBVar* v=gbFindVar(s,name,idx);
    if(!v)return gbNV(0);
    if(v->type==GBV_NUM)return gbNV(v->num);
    if(v->type==GBV_STR)return gbSV(gbHP(s,v->str.off));
    return gbNV(0);
}

static bool gbSetNum(GBVarStore* s,const char* name,double val,uint8_t idx){
    gBVarMtx.lock();
    GBVar* v=gbGetMake(s,name,idx);
    bool ok=(v!=nullptr);
    if(ok){v->type=GBV_NUM;v->num=val;}
    gBVarMtx.unlock();
    if(ok&&s->heapTop>GB_VAR_SLOT_SIZE){gBSlot[idx]=0;return false;}
    return ok;
}

static bool gbSetStr(GBVarStore* s,const char* name,const char* val,uint8_t idx){
    gBVarMtx.lock();
    GBVar* v=gbGetMake(s,name,idx);
    if(!v){gBVarMtx.unlock();return false;}
    uint32_t slen=(uint32_t)strlen(val)+1;
    if(v->type!=GBV_STR||v->str.cap<slen){
        uint32_t off=gbHAlloc(s,slen+64);
        if(!off){gBVarMtx.unlock();return false;}
        v->str.off=off;v->str.cap=(uint16_t)(slen+64);
    }
    v->type=GBV_STR;
    strncpy(gbHP(s,v->str.off),val,v->str.cap-1);
    v->str.len=(uint16_t)(slen-1);
    gBVarMtx.unlock();
    return true;
}

// ================================================================
// EXPRESSION EVALUATOR  (recursive descent)
// ================================================================
static GBVal gbEvalExpr(GBCtx* ctx, const char** p);
static GBVal gbEvalTerm(GBCtx* ctx, const char** p);

static GBVal gbEvalFactor(GBCtx* ctx, const char** p){
    const char* s=gbSkipWS(*p);
    // unary minus on number
    if(*s=='-'&&isdigit((unsigned char)s[1])){
        s++;*p=s;GBVal r=gbEvalFactor(ctx,p);r.num=-r.num;return r;
    }
    // parentheses
    if(*s=='('){
        s++;*p=s;GBVal r=gbEvalExpr(ctx,p);
        s=gbSkipWS(*p);if(*s==')')s++;*p=s;return r;
    }
    // quoted string
    if(*s=='"'){
        s++;char buf[GB_MAX_LINE_LEN];int i=0;
        while(*s&&*s!='"'&&i<GB_MAX_LINE_LEN-1)buf[i++]=*s++;
        buf[i]='\0';if(*s=='"')s++;*p=s;return gbSV(buf);
    }
    // known function names (SIN, COS, SQRT etc.)
    char fn[24];int fi=0;const char* fs=s;
    while(isalpha((unsigned char)*fs)&&fi<23)fn[fi++]=*fs++;
    fn[fi]='\0';
    if(*fs=='('&&fi>0){
        s=fs+1;*p=s;
        GBVal a=gbEvalExpr(ctx,p);
        s=gbSkipWS(*p);if(*s==')')s++;*p=s;
        GBVal b=gbNV(0);bool has2=false;
        if(*gbSkipWS(*p)==','){s=gbSkipWS(*p)+1;*p=s;b=gbEvalExpr(ctx,p);s=gbSkipWS(*p);if(*s==')')s++;*p=s;has2=true;}
        double x=a.num;
        if     (!strcmp(fn,"SIN"))  return gbNV(sin(x));
        else if(!strcmp(fn,"COS"))  return gbNV(cos(x));
        else if(!strcmp(fn,"TAN"))  return gbNV(tan(x));
        else if(!strcmp(fn,"ASIN")) return gbNV(asin(x));
        else if(!strcmp(fn,"ACOS")) return gbNV(acos(x));
        else if(!strcmp(fn,"ATAN")) return gbNV(atan(x));
        else if(!strcmp(fn,"SQRT")) return gbNV(sqrt(x));
        else if(!strcmp(fn,"ABS"))  return gbNV(fabs(x));
        else if(!strcmp(fn,"INT"))  return gbNV(floor(x));
        else if(!strcmp(fn,"LOG"))  return gbNV(log10(x));
        else if(!strcmp(fn,"LN"))   return gbNV(log(x));
        else if(!strcmp(fn,"ROUND"))return gbNV(has2?round(x*pow(10,b.num))/pow(10,b.num):round(x));
        else if(!strcmp(fn,"MIN"))  return gbNV(fmin(x,b.num));
        else if(!strcmp(fn,"MAX"))  return gbNV(fmax(x,b.num));
        else if(!strcmp(fn,"MOD"))  return gbNV(fmod(x,b.num));
        else if(!strcmp(fn,"POW"))  return gbNV(pow(x,b.num));
        else if(!strcmp(fn,"LEN"))  return gbNV(a.isStr?(double)strlen(a.str):(double)snprintf(nullptr,0,"%.10g",x));
        else if(!strcmp(fn,"STRTONUM")){GBVal r=gbNV(atof(a.str));return r;}
        return gbNV(0);
    }
    // inline get-info keywords (no parens)
    if(gbSW(s,"GETTIME")){
        *p=s+7;char buf[16];tm t;
        _rtc_localtime(time(NULL),&t,RTC_4_YEAR_LEAP_YEAR_SUPPORT);
        strftime(buf,sizeof(buf),"%I:%M:%S %p",&t);return gbSV(buf);
    }
    if(gbSW(s,"GETDATE")){
        *p=s+7;char buf[16];tm t;
        _rtc_localtime(time(NULL),&t,RTC_4_YEAR_LEAP_YEAR_SUPPORT);
        strftime(buf,sizeof(buf),"%Y-%m-%d",&t);return gbSV(buf);
    }
    if(gbSW(s,"GETMS"))  {*p=s+5;return gbNV((double)millis());}
    if(gbSW(s,"GETCPU")) {*p=s+6;return gbNV(gBusy1s);}
    if(gbSW(s,"GETTEMP")){*p=s+7;return gbNV(gTempF);}
    if(gbSW(s,"GETWIFI")){*p=s+7;return gbSV(gWifiConnected?WiFi.SSID():"not connected");}
    if(gbSW(s,"GETIP"))  {
        *p=s+5;if(!gWifiConnected)return gbSV("not connected");
        IPAddress ip=WiFi.localIP();char b[24];
        snprintf(b,sizeof(b),"%d.%d.%d.%d",ip[0],ip[1],ip[2],ip[3]);return gbSV(b);
    }
    if(gbSW(s,"GETMEM")) {
        *p=s+6;mbed_stats_heap_t h;mbed_stats_heap_get(&h);
        return gbNV((double)(h.reserved_size-h.current_size));
    }
    // numeric literal
    if(isdigit((unsigned char)*s)||(*s=='.'&&isdigit((unsigned char)s[1]))){
        char* end;double n=strtod(s,&end);*p=end;return gbNV(n);
    }
    // uppercase variable name
    if(isupper((unsigned char)*s)||*s=='_'){
        char vn[32];int vi=0;
        while((isupper((unsigned char)*s)||isdigit((unsigned char)*s)||*s=='_')&&vi<31)vn[vi++]=*s++;
        vn[vi]='\0';*p=s;
        return gbGetVal(ctx->store,vn,ctx->idx);
    }
    *p=s;return gbNV(0);
}

static GBVal gbEvalTerm(GBCtx* ctx,const char** p){
    GBVal r=gbEvalFactor(ctx,p);
    const char* s=gbSkipWS(*p);
    while(*s=='*'||*s=='/'){
        char op=*s++;s=gbSkipWS(s);*p=s;
        GBVal rhs=gbEvalFactor(ctx,p);s=gbSkipWS(*p);
        if(op=='*')r.num*=rhs.num;
        else if(rhs.num!=0)r.num/=rhs.num;
        else{gbError(ctx,"division by zero");return gbNV(0);}
    }
    *p=s;return r;
}

static GBVal gbEvalExpr(GBCtx* ctx,const char** p){
    GBVal r=gbEvalTerm(ctx,p);
    const char* s=gbSkipWS(*p);
    while((*s=='+'||*s=='-')&&s[1]!='='){
        char op=*s++;s=gbSkipWS(s);*p=s;
        GBVal rhs=gbEvalTerm(ctx,p);s=gbSkipWS(*p);
        if(!r.isStr&&!rhs.isStr){
            if(op=='+')r.num+=rhs.num;else r.num-=rhs.num;
        } else {
            char ls[GB_MAX_LINE_LEN],rs[GB_MAX_LINE_LEN];
            gbToStr(r,ls,sizeof(ls));gbToStr(rhs,rs,sizeof(rs));
            r=gbSV(ls);strncat(r.str,rs,GB_MAX_LINE_LEN-strlen(r.str)-1);
        }
    }
    *p=s;return r;
}

// AND chain: numeric+numeric=add, either string=concat
static GBVal gbEvalAndChain(GBCtx* ctx,const char* expr){
    const char* p=gbSkipWS(expr);
    GBVal r=gbEvalExpr(ctx,&p);p=gbSkipWS(p);
    while(gbSW(p,"AND")&&(p[3]==' '||p[3]=='\0'||p[3]=='\t')){
        p=gbSkipWS(p+3);
        GBVal rhs=gbEvalExpr(ctx,&p);p=gbSkipWS(p);
        if(!r.isStr&&!rhs.isStr){r.num+=rhs.num;}
        else{
            char ls[GB_MAX_LINE_LEN],rs[GB_MAX_LINE_LEN];
            gbToStr(r,ls,sizeof(ls));gbToStr(rhs,rs,sizeof(rs));
            char cb[GB_MAX_LINE_LEN];snprintf(cb,sizeof(cb),"%s%s",ls,rs);
            r=gbSV(cb);
        }
    }
    return r;
}

// Condition evaluator
static bool gbEvalCond(GBCtx* ctx,const char* cond){
    const char* p=gbSkipWS(cond);
    bool notF=false;if(gbMW(&p,"NOT"))notF=true;
    GBVal lhs=gbEvalExpr(ctx,&p);p=gbSkipWS(p);
    bool result;
    if     (gbMS(&p,">="))    {GBVal r=gbEvalExpr(ctx,&p);result=lhs.num>=r.num;}
    else if(gbMS(&p,"<="))    {GBVal r=gbEvalExpr(ctx,&p);result=lhs.num<=r.num;}
    else if(gbMS(&p,">"))     {GBVal r=gbEvalExpr(ctx,&p);result=lhs.num>r.num;}
    else if(gbMS(&p,"<"))     {GBVal r=gbEvalExpr(ctx,&p);result=lhs.num<r.num;}
    else if(gbMW(&p,"ISNOT")||( gbMW(&p,"IS")&&gbMW(&p,"NOT") )){
        GBVal r=gbEvalAndChain(ctx,p);
        char ls[GB_MAX_LINE_LEN],rs[GB_MAX_LINE_LEN];
        gbToStr(lhs,ls,sizeof(ls));gbToStr(r,rs,sizeof(rs));
        result=(!lhs.isStr&&!r.isStr)?(lhs.num!=r.num):(strcmp(ls,rs)!=0);
    }
    else if(gbMW(&p,"IS")){
        GBVal r=gbEvalAndChain(ctx,p);
        char ls[GB_MAX_LINE_LEN],rs[GB_MAX_LINE_LEN];
        gbToStr(lhs,ls,sizeof(ls));gbToStr(r,rs,sizeof(rs));
        result=(!lhs.isStr&&!r.isStr)?(lhs.num==r.num):(strcmp(ls,rs)==0);
    }
    else if(gbMW(&p,"CONTAINS")){
        GBVal r=gbEvalAndChain(ctx,p);
        char ls[GB_MAX_LINE_LEN],rs[GB_MAX_LINE_LEN];
        gbToStr(lhs,ls,sizeof(ls));gbToStr(r,rs,sizeof(rs));
        result=(strstr(ls,rs)!=nullptr);
    }
    else{result=!gbFalsy(lhs);}
    if(notF)result=!result;
    p=gbSkipWS(p);
    if(gbMW(&p,"AND"))result=result&&gbEvalCond(ctx,p);
    else if(gbMW(&p,"OR"))result=result||gbEvalCond(ctx,p);
    return result;
}

// ================================================================
// ERROR REPORTING
// ================================================================
static void gbError(GBCtx* ctx,const char* fmt,...){
    char msg[GB_MAX_LINE_LEN];
    va_list v;va_start(v,fmt);vsnprintf(msg,sizeof(msg),fmt,v);va_end(v);
    char full[GB_MAX_LINE_LEN+32];
    snprintf(full,sizeof(full),"ERROR line %d: %s",ctx->curLine,msg);
    gbOut(ctx,full,COL_ERROR);
    ctx->running=false;
}

static void gbPreErr(const char* script,int line,const char* msg){
    char buf[GB_MAX_LINE_LEN];
    snprintf(buf,sizeof(buf),"  pre-check line %d: %s",line,msg);
    termPrint(buf,COL_ERROR);
}

// ================================================================
// PRE-PROCESSING PASS  (index lines, find labels, validate blocks)
// Returns true if no errors found
// ================================================================
static const char* gbGetLine(GBCtx* ctx,int ln,char* buf){
    if(ln<1||ln>ctx->lineCount){buf[0]='\0';return buf;}
    uint32_t off=ctx->lineOff[ln-1],end=off;
    while(end<ctx->srcLen&&ctx->src[end]!='\n'&&ctx->src[end]!='\r')end++;
    int len=min((int)(end-off),GB_MAX_LINE_LEN-1);
    memcpy(buf,ctx->src+off,len);buf[len]='\0';return buf;
}

static int gbFindLabel(GBCtx* ctx,const char* name){
    for(int i=0;i<ctx->labelCount;i++)
        if(strcmp(ctx->labels[i].name,name)==0)return ctx->labels[i].line;
    return -1;
}

static bool gbPrePass(GBCtx* ctx){
    ctx->lineCount=0;ctx->labelCount=0;
    memset(ctx->endLines,-1,sizeof(ctx->endLines));
    memset(ctx->elseLines,-1,sizeof(ctx->elseLines));
    if(!ctx->srcLen)return true;
    ctx->lineOff[ctx->lineCount++]=0;
    for(uint32_t i=0;i<ctx->srcLen&&ctx->lineCount<GB_MAX_LINES;i++)
        if(ctx->src[i]=='\n'&&i+1<ctx->srcLen)ctx->lineOff[ctx->lineCount++]=i+1;
    int ifSt[64]={},ifT=0,forSt[64]={},forT=0,whSt[64]={},whT=0,repSt[64]={},repT=0;
    bool bad=false;
    for(int ln=1;ln<=ctx->lineCount;ln++){
        char line[GB_MAX_LINE_LEN];gbGetLine(ctx,ln,line);
        const char* p=gbSkipWS(line);
        if(!*p||*p=='#')continue;
        if(gbSW(p,"LBL ")){
            const char* np=gbSkipWS(p+4);char lb[32];int li=0;
            while(*np&&*np!=' '&&li<31)lb[li++]=*np++;lb[li]='\0';
            for(int j=0;j<ctx->labelCount;j++){
                if(strcmp(ctx->labels[j].name,lb)==0){
                    char m[64];snprintf(m,sizeof(m),"duplicate label \"%s\" (also at line %d)",lb,ctx->labels[j].line);
                    gbPreErr(ctx->name,ln,m);bad=true;
                }
            }
            if(ctx->labelCount<GB_MAX_LABELS){strncpy(ctx->labels[ctx->labelCount].name,lb,31);ctx->labels[ctx->labelCount++].line=ln;}
        }
        else if(gbSW(p,"IF ")&&strstr(p," THEN")){if(ifT<64)ifSt[ifT++]=ln;}
        else if(gbSW(p,"FOR "))                  {if(forT<64)forSt[forT++]=ln;}
        else if(gbSW(p,"WHILE "))                {if(whT<64)whSt[whT++]=ln;}
        else if(gbSW(p,"REPEAT"))                {if(repT<64)repSt[repT++]=ln;}
        else if(gbSW(p,"ELSE"))                  {if(ifT>0)ctx->elseLines[ifSt[ifT-1]-1]=ln;}
        else if(gbSW(p,"END IF"))   {if(ifT<=0){gbPreErr(ctx->name,ln,"END IF without IF");bad=true;}else{int s=ifSt[--ifT];ctx->endLines[s-1]=ln;}}
        else if(gbSW(p,"END FOR"))  {if(forT<=0){gbPreErr(ctx->name,ln,"END FOR without FOR");bad=true;}else{int s=forSt[--forT];ctx->endLines[s-1]=ln;}}
        else if(gbSW(p,"END WHILE")){if(whT<=0){gbPreErr(ctx->name,ln,"END WHILE without WHILE");bad=true;}else{int s=whSt[--whT];ctx->endLines[s-1]=ln;}}
        else if(gbSW(p,"UNTIL "))   {if(repT<=0){gbPreErr(ctx->name,ln,"UNTIL without REPEAT");bad=true;}else{int s=repSt[--repT];ctx->endLines[s-1]=ln;}}
    }
    if(ifT>0) {gbPreErr(ctx->name,0,"unclosed IF");bad=true;}
    if(forT>0){gbPreErr(ctx->name,0,"unclosed FOR");bad=true;}
    if(whT>0) {gbPreErr(ctx->name,0,"unclosed WHILE");bad=true;}
    if(repT>0){gbPreErr(ctx->name,0,"unclosed REPEAT");bad=true;}
    return!bad;
}

// ================================================================
// EXECUTE ONE LINE -- full command dispatcher
// ================================================================
static void gbSkipToEnd(GBCtx* ctx,int sl){
    int el=(sl>=1&&sl<=ctx->lineCount)?ctx->endLines[sl-1]:-1;
    if(el>0)ctx->curLine=el;else ctx->running=false;
}
static void gbSkipToElseOrEnd(GBCtx* ctx,int il){
    int el=ctx->elseLines[il-1];
    if(el>0){ctx->curLine=el;return;}
    gbSkipToEnd(ctx,il);
}

static void gbExecuteLine(GBCtx* ctx,const char* rawLine,int ln){
    ctx->curLine=ln;
    const char* line=gbSkipWS(rawLine);
    if(!*line||*line=='#')return;

    // RUNMODE
    if(gbSW(line,"RUNMODE ")){
        const char* p=gbSkipWS(line+8);
        if(gbMW(&p,"BACKGROUND"))ctx->declMode=GBR_BG;
        else if(gbMW(&p,"FOREGROUND"))ctx->declMode=GBR_FG;
        return;
    }
    // STOP
    if(gbSW(line,"STOP")){ctx->running=false;return;}

    // STORE [expr AND...] TO VAR V
    if(gbSW(line,"STORE ")){
        const char* rest=line+6;
        const char* tv=strstr(rest," TO VAR ");
        if(!tv){gbError(ctx,"STORE missing 'TO VAR'");return;}
        char expr[GB_MAX_LINE_LEN];
        int el=min((int)(tv-rest),GB_MAX_LINE_LEN-1);
        memcpy(expr,rest,el);expr[el]='\0';
        const char* vn=gbSkipWS(tv+8);
        char vb[32];int vi=0;
        while((isupper((unsigned char)*vn)||isdigit((unsigned char)*vn))&&vi<31)vb[vi++]=*vn++;
        vb[vi]='\0';
        if(!vb[0]){gbError(ctx,"STORE: invalid variable name");return;}
        GBVal val=gbEvalAndChain(ctx,expr);
        bool ok=val.isStr?gbSetStr(ctx->store,vb,val.str,ctx->idx):gbSetNum(ctx->store,vb,val.num,ctx->idx);
        if(!ok){gbError(ctx,"memory limit exceeded (256KB)");return;}
        return;
    }
    // ADD [expr] TO VAR V
    if(gbSW(line,"ADD ")&&strstr(line," TO VAR ")&&!strstr(line," TO FILE ")){
        const char* p=line+4;
        const char* tv=strstr(p," TO VAR ");
        if(!tv){gbError(ctx,"ADD missing 'TO VAR'");return;}
        char expr[GB_MAX_LINE_LEN];int el=min((int)(tv-p),GB_MAX_LINE_LEN-1);
        memcpy(expr,p,el);expr[el]='\0';
        const char* vn=gbSkipWS(tv+8);char vb[32];int vi=0;
        while((isupper((unsigned char)*vn)||isdigit((unsigned char)*vn))&&vi<31)vb[vi++]=*vn++;vb[vi]='\0';
        GBVal av=gbEvalAndChain(ctx,expr),cv=gbGetVal(ctx->store,vb,ctx->idx);
        if(!cv.isStr&&!av.isStr)gbSetNum(ctx->store,vb,cv.num+av.num,ctx->idx);
        else{char ls[GB_MAX_LINE_LEN],rs[GB_MAX_LINE_LEN];gbToStr(cv,ls,sizeof(ls));gbToStr(av,rs,sizeof(rs));char cb[GB_MAX_LINE_LEN];snprintf(cb,sizeof(cb),"%s%s",ls,rs);gbSetStr(ctx->store,vb,cb,ctx->idx);}
        return;
    }
    // SUB [expr] FROM VAR V
    if(gbSW(line,"SUB ")){
        const char* p=line+4,*fv=strstr(p," FROM VAR ");
        if(!fv){gbError(ctx,"SUB missing 'FROM VAR'");return;}
        char expr[GB_MAX_LINE_LEN];int el=min((int)(fv-p),GB_MAX_LINE_LEN-1);memcpy(expr,p,el);expr[el]='\0';
        const char* vn=gbSkipWS(fv+10);char vb[32];int vi=0;
        while((isupper((unsigned char)*vn)||isdigit((unsigned char)*vn))&&vi<31)vb[vi++]=*vn++;vb[vi]='\0';
        GBVal sv=gbEvalAndChain(ctx,expr),cv=gbGetVal(ctx->store,vb,ctx->idx);
        gbSetNum(ctx->store,vb,cv.num-sv.num,ctx->idx);return;
    }
    // MUL VAR V BY [expr]
    if(gbSW(line,"MUL VAR ")){
        const char* p=gbSkipWS(line+8);char vb[32];int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';p=gbSkipWS(p);
        if(!gbMW(&p,"BY")){gbError(ctx,"MUL: missing BY");return;}
        GBVal mv=gbEvalAndChain(ctx,p),cv=gbGetVal(ctx->store,vb,ctx->idx);
        gbSetNum(ctx->store,vb,cv.num*mv.num,ctx->idx);return;
    }
    // DIV VAR V BY [expr]
    if(gbSW(line,"DIV VAR ")){
        const char* p=gbSkipWS(line+8);char vb[32];int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';p=gbSkipWS(p);
        if(!gbMW(&p,"BY")){gbError(ctx,"DIV: missing BY");return;}
        GBVal dv=gbEvalAndChain(ctx,p);if(dv.num==0){gbError(ctx,"division by zero");return;}
        GBVal cv=gbGetVal(ctx->store,vb,ctx->idx);gbSetNum(ctx->store,vb,cv.num/dv.num,ctx->idx);return;
    }
    // INC / DEC VAR V
    if(gbSW(line,"INC VAR ")){const char* p=gbSkipWS(line+8);char vb[32];int vi=0;while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';gbSetNum(ctx->store,vb,gbGetVal(ctx->store,vb,ctx->idx).num+1,ctx->idx);return;}
    if(gbSW(line,"DEC VAR ")){const char* p=gbSkipWS(line+8);char vb[32];int vi=0;while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';gbSetNum(ctx->store,vb,gbGetVal(ctx->store,vb,ctx->idx).num-1,ctx->idx);return;}

    // CLEAR ALL VARS / CLEAR VAR V
    if(gbSW(line,"CLEAR ALL VARS")){gBVarMtx.lock();gbInitStore(ctx->store,ctx->idx);gBVarMtx.unlock();return;}
    if(gbSW(line,"CLEAR VAR ")){
        const char* p=gbSkipWS(line+10);char vb[32];int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';
        GBVar* v=gbFindVar(ctx->store,vb,ctx->idx);
        if(v){if(v->type==GBV_NUM)v->num=0;else if(v->type==GBV_STR){char* sp=gbHP(ctx->store,v->str.off);sp[0]='\0';v->str.len=0;}}
        return;
    }
    // SWAP VAR A AND VAR B
    if(gbSW(line,"SWAP VAR ")){
        const char* p=gbSkipWS(line+9);char v1[32]={},v2[32]={};int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)v1[vi++]=*p++;p=gbSkipWS(p);
        if(!gbMW(&p,"AND")){gbError(ctx,"SWAP: missing AND");return;}
        if(gbMW(&p,"VAR"))p=gbSkipWS(p);
        vi=0;while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)v2[vi++]=*p++;
        GBVal a=gbGetVal(ctx->store,v1,ctx->idx),b=gbGetVal(ctx->store,v2,ctx->idx);
        if(a.isStr)gbSetStr(ctx->store,v2,a.str,ctx->idx);else gbSetNum(ctx->store,v2,a.num,ctx->idx);
        if(b.isStr)gbSetStr(ctx->store,v1,b.str,ctx->idx);else gbSetNum(ctx->store,v1,b.num,ctx->idx);
        return;
    }
    // COPY VAR A TO VAR B
    if(gbSW(line,"COPY VAR ")){
        const char* p=gbSkipWS(line+9);char v1[32]={},v2[32]={};int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)v1[vi++]=*p++;
        p=gbSkipWS(p);if(gbMW(&p,"TO"))p=gbSkipWS(p);if(gbMW(&p,"VAR"))p=gbSkipWS(p);
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)v2[vi++]=*p++;v2[vi]='\0';
        GBVal a=gbGetVal(ctx->store,v1,ctx->idx);
        if(a.isStr)gbSetStr(ctx->store,v2,a.str,ctx->idx);else gbSetNum(ctx->store,v2,a.num,ctx->idx);
        return;
    }
    // PRIVATE / PUBLIC VAR V
    if(gbSW(line,"PRIVATE VAR ")){
        const char* p=gbSkipWS(line+12);char vb[32];int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';
        GBVar* v=gbGetMake(ctx->store,vb,ctx->idx);if(v)v->isPrivate=1;return;
    }
    if(gbSW(line,"PUBLIC VAR ")){
        const char* p=gbSkipWS(line+11);char vb[32];int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';
        GBVar* v=gbGetMake(ctx->store,vb,ctx->idx);if(v)v->isPrivate=0;return;
    }

    // -- OUTPUT ----------------------------------------------------
    if(gbSW(line,"DISPLN ")){
        GBVal val=gbEvalAndChain(ctx,line+7);char buf[GB_MAX_LINE_LEN];gbToStr(val,buf,sizeof(buf));
        gbOut(ctx,buf);gbOut(ctx,"");return;
    }
    if(gbSW(line,"DISP ")&&!gbSW(line,"DISPLN")){
        GBVal val=gbEvalAndChain(ctx,line+5);char buf[GB_MAX_LINE_LEN];gbToStr(val,buf,sizeof(buf));
        gbOut(ctx,buf);return;
    }
    if(strcmp(line,"DISP")==0){gbOut(ctx,"");return;}
    if(gbSW(line,"OUTPUT ")){
        const char* p=line+7;uint16_t color=GB_RN_TXT;
        const char* tc=strstr(p," TEXTCOLOR ");
        char expr[GB_MAX_LINE_LEN];
        if(tc){int el=min((int)(tc-p),GB_MAX_LINE_LEN-1);memcpy(expr,p,el);expr[el]='\0';
            uint32_t rgb=(uint32_t)strtol(tc+11,nullptr,16);
            uint8_t r=(rgb>>16)&0xFF,g=(rgb>>8)&0xFF,b=rgb&0xFF;
            color=((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3);
        } else strncpy(expr,p,GB_MAX_LINE_LEN-1);
        GBVal val=gbEvalAndChain(ctx,expr);char buf[GB_MAX_LINE_LEN];gbToStr(val,buf,sizeof(buf));
        gbOut(ctx,buf,color);return;
    }
    if(strcmp(line,"CLS")==0){
        if(ctx->isFg){gBRnCount=0;gBRnDirty=true;dispMtx.lock();display.fillRect(0,GB_RN_TOP,DISP_W,TERM_H-GB_RN_HDR_H,GB_RN_BG);dispMtx.unlock();}
        return;
    }
    if(gbSW(line,"BEEP")){tone(5,880,200);return;}

    // -- INPUT -----------------------------------------------------
    if(gbSW(line,"ASK ")){
        const char* p=line+4;char prompt[GB_MAX_LINE_LEN]="";
        if(*gbSkipWS(p)=='"'){p=gbSkipWS(p)+1;int i=0;while(*p&&*p!='"'&&i<GB_MAX_LINE_LEN-1)prompt[i++]=*p++;prompt[i]='\0';if(*p=='"')p++;}
        p=gbSkipWS(p);if(!gbMW(&p,"INTO")){gbError(ctx,"ASK: missing INTO VAR");return;}
        if(gbMW(&p,"VAR"))p=gbSkipWS(p);
        char vb[32];int vi=0;while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';
        if(ctx->isFg){
            gbRnPrint(prompt,COL_PROMPT);
            gBRnAskLen=0;gBRnAskBuf[0]='\0';gBRnAsk=true;gBRnDirty=true;
            gBRnEvt.wait_any(0x01);gBRnAsk=false;
            gbSetStr(ctx->store,vb,gBRnAskBuf,ctx->idx);
        } else gbSetStr(ctx->store,vb,"",ctx->idx);
        return;
    }

    // -- TIMING ----------------------------------------------------
    if(gbSW(line,"WAITTIME ")){
        const char* p=gbSkipWS(line+9);
        char nb[32];int ni=0;while((*p=='-'||isdigit((unsigned char)*p)||*p=='.')&&ni<31)nb[ni++]=*p++;nb[ni]='\0';p=gbSkipWS(p);
        double val=atof(nb);uint32_t ms=0;
        if     (gbMW(&p,"ms"))ms=(uint32_t)val;
        else if(gbMW(&p,"s")) ms=(uint32_t)(val*1000);
        else if(gbMW(&p,"m")) ms=(uint32_t)(val*60000);
        else if(gbMW(&p,"h")) ms=(uint32_t)(val*3600000UL);
        else if(gbMW(&p,"d")) ms=(uint32_t)(val*86400000UL);
        else                   ms=(uint32_t)val;
        uint32_t t0=millis();while(millis()-t0<ms&&!ctx->abortFlag)rtos::ThisThread::sleep_for(50);
        return;
    }
    if(gbSW(line,"PAUSE")){
        const char* p=gbSkipWS(line+5);
        if(*p&&isdigit((unsigned char)*p)){uint32_t ms=(uint32_t)atol(p);uint32_t t0=millis();while(millis()-t0<ms&&!ctx->abortFlag)rtos::ThisThread::sleep_for(50);}
        else{if(ctx->isFg){gbRnPrint("[press any key]",COL_DIM);gBRnAsk=true;gBRnDirty=true;gBRnEvt.wait_any(0x02);gBRnAsk=false;}}
        return;
    }
    if(gbSW(line,"DOAT ")){
        const char* p=gbSkipWS(line+5);int h=0,m=0;
        if(sscanf(p,"%d:%d",&h,&m)!=2){gbError(ctx,"DOAT: expected HH:MM");return;}
        while(!ctx->abortFlag){
            tm t;_rtc_localtime(time(NULL),&t,RTC_4_YEAR_LEAP_YEAR_SUPPORT);
            if(t.tm_hour==h&&t.tm_min==m)break;
            rtos::ThisThread::sleep_for(5000);
        }
        return;
    }
    if(gbSW(line,"WAIT UNTIL ")){
        const char* cond=gbSkipWS(line+11);uint32_t timeout=0;
        const char* tp=strstr(cond," TIMEOUT ");char condBuf[GB_MAX_LINE_LEN];
        if(tp){int cl=min((int)(tp-cond),GB_MAX_LINE_LEN-1);memcpy(condBuf,cond,cl);condBuf[cl]='\0';timeout=(uint32_t)atol(tp+9);cond=condBuf;}
        uint32_t t0=millis();
        while(!gbEvalCond(ctx,cond)&&!ctx->abortFlag){if(timeout>0&&millis()-t0>=timeout)break;rtos::ThisThread::sleep_for(100);}
        return;
    }

    // -- FLOW CONTROL ----------------------------------------------
    if(gbSW(line,"GOTO LBL ")){
        const char* ln2=gbSkipWS(line+9);int t=gbFindLabel(ctx,ln2);
        if(t<0){gbError(ctx,"GOTO: label \"%s\" not found",ln2);return;}
        ctx->curLine=t;return;
    }
    if(gbSW(line,"GOTO ")){
        int t=atoi(gbSkipWS(line+5));
        if(t<1||t>ctx->lineCount){gbError(ctx,"GOTO: line %d out of range",t);return;}
        ctx->curLine=t;return;
    }
    if(gbSW(line,"LBL "))return;
    if(gbSW(line,"GOSUB LBL ")){
        if(ctx->callDepth>=GB_CALL_DEPTH){gbError(ctx,"call stack overflow");return;}
        const char* ln2=gbSkipWS(line+10);int t=gbFindLabel(ctx,ln2);
        if(t<0){gbError(ctx,"GOSUB: label \"%s\" not found",ln2);return;}
        ctx->callStack[ctx->callDepth++]=ctx->curLine;ctx->curLine=t;return;
    }
    if(gbSW(line,"RETURN")){
        if(ctx->callDepth<=0){gbError(ctx,"RETURN without GOSUB");return;}
        ctx->curLine=ctx->callStack[--ctx->callDepth];return;
    }

    // -- IF / ELSE / END IF ----------------------------------------
    if(gbSW(line,"IF ")&&strstr(line," THEN")){
        const char* p=line+3,*th=strstr(p," THEN");
        if(!th){gbError(ctx,"IF: missing THEN");return;}
        char cond[GB_MAX_LINE_LEN];int cl=min((int)(th-p),GB_MAX_LINE_LEN-1);memcpy(cond,p,cl);cond[cl]='\0';
        bool res=gbEvalCond(ctx,cond);
        if(ctx->ctrlTop>=GB_CTRL_DEPTH){gbError(ctx,"control stack overflow");return;}
        GBFrame& fr=ctx->ctrl[ctx->ctrlTop++];
        fr.type=GBF_IF;fr.startLine=ln;fr.endLine=ctx->endLines[ln-1];fr.elseLine=ctx->elseLines[ln-1];
        if(!res){gbSkipToElseOrEnd(ctx,ln);ctx->ctrlTop--;}
        return;
    }
    if(gbSW(line,"ELSE IF ")){
        if(ctx->ctrlTop>0&&ctx->ctrl[ctx->ctrlTop-1].type==GBF_IF)ctx->ctrlTop--;
        const char* p=line+8,*th=strstr(p," THEN");
        if(!th){gbError(ctx,"ELSE IF: missing THEN");return;}
        char cond[GB_MAX_LINE_LEN];int cl=min((int)(th-p),GB_MAX_LINE_LEN-1);memcpy(cond,p,cl);cond[cl]='\0';
        bool res=gbEvalCond(ctx,cond);
        if(ctx->ctrlTop>=GB_CTRL_DEPTH){gbError(ctx,"control stack overflow");return;}
        GBFrame& fr=ctx->ctrl[ctx->ctrlTop++];
        fr.type=GBF_IF;fr.startLine=ln;fr.endLine=(ln>=1&&ln<=ctx->lineCount)?ctx->endLines[ln-1]:-1;fr.elseLine=(ln>=1&&ln<=ctx->lineCount)?ctx->elseLines[ln-1]:-1;
        if(!res)gbSkipToElseOrEnd(ctx,ln);
        return;
    }
    if(gbSW(line,"ELSE")){
        if(ctx->ctrlTop>0&&ctx->ctrl[ctx->ctrlTop-1].type==GBF_IF){int sl=ctx->ctrl[--ctx->ctrlTop].startLine;gbSkipToEnd(ctx,sl);}
        return;
    }
    if(gbSW(line,"END IF")){if(ctx->ctrlTop>0&&ctx->ctrl[ctx->ctrlTop-1].type==GBF_IF)ctx->ctrlTop--;return;}

    // -- FOR -------------------------------------------------------
    if(gbSW(line,"FOR VAR ")){
        const char* p=gbSkipWS(line+8);char vb[32];int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';p=gbSkipWS(p);
        if(!gbMW(&p,"FROM")){gbError(ctx,"FOR: missing FROM");return;}
        GBVal sv=gbEvalExpr(ctx,&p);
        const char* tp2=strstr(p," TO ");if(!tp2){gbError(ctx,"FOR: missing TO");return;}
        p=tp2+4;GBVal lv=gbEvalExpr(ctx,&p);
        double step=1;const char* sp=strstr(p," STEP ");if(sp)step=gbEvalAndChain(ctx,sp+6).num;
        if(step==0){gbError(ctx,"FOR: STEP cannot be 0");return;}
        gbSetNum(ctx->store,vb,sv.num,ctx->idx);
        bool runs=(step>0)?(sv.num<=lv.num):(sv.num>=lv.num);
        if(!runs){gbSkipToEnd(ctx,ln);return;}
        if(ctx->ctrlTop>=GB_CTRL_DEPTH){gbError(ctx,"control stack overflow");return;}
        GBFrame& fr=ctx->ctrl[ctx->ctrlTop++];
        fr.type=GBF_FOR;fr.startLine=ln;fr.endLine=ctx->endLines[ln-1];
        strncpy(fr.forVar,vb,31);fr.forLimit=lv.num;fr.forStep=step;
        return;
    }
    if(gbSW(line,"END FOR")){
        if(ctx->ctrlTop<=0||ctx->ctrl[ctx->ctrlTop-1].type!=GBF_FOR)return;
        GBFrame& fr=ctx->ctrl[ctx->ctrlTop-1];
        double cur=gbGetVal(ctx->store,fr.forVar,ctx->idx).num+fr.forStep;
        gbSetNum(ctx->store,fr.forVar,cur,ctx->idx);
        bool cont=(fr.forStep>0)?(cur<=fr.forLimit):(cur>=fr.forLimit);
        if(cont)ctx->curLine=fr.startLine+1;else ctx->ctrlTop--;
        return;
    }

    // -- WHILE -----------------------------------------------------
    if(gbSW(line,"WHILE ")){
        const char* cond=gbSkipWS(line+6);bool res=gbEvalCond(ctx,cond);
        if(!res){gbSkipToEnd(ctx,ln);return;}
        if(ctx->ctrlTop>=GB_CTRL_DEPTH){gbError(ctx,"control stack overflow");return;}
        GBFrame& fr=ctx->ctrl[ctx->ctrlTop++];
        fr.type=GBF_WHILE;fr.startLine=ln;fr.endLine=ctx->endLines[ln-1];strncpy(fr.whileCond,cond,GB_MAX_LINE_LEN-1);
        return;
    }
    if(gbSW(line,"END WHILE")){
        if(ctx->ctrlTop<=0||ctx->ctrl[ctx->ctrlTop-1].type!=GBF_WHILE)return;
        GBFrame& fr=ctx->ctrl[ctx->ctrlTop-1];
        if(gbEvalCond(ctx,fr.whileCond))ctx->curLine=fr.startLine;else ctx->ctrlTop--;
        return;
    }

    // -- REPEAT / UNTIL --------------------------------------------
    if(gbSW(line,"REPEAT")){
        if(ctx->ctrlTop>=GB_CTRL_DEPTH){gbError(ctx,"control stack overflow");return;}
        GBFrame& fr=ctx->ctrl[ctx->ctrlTop++];fr.type=GBF_REPEAT;fr.startLine=ln;fr.endLine=ctx->endLines[ln-1];
        return;
    }
    if(gbSW(line,"UNTIL ")){
        if(ctx->ctrlTop<=0||ctx->ctrl[ctx->ctrlTop-1].type!=GBF_REPEAT)return;
        const char* cond=gbSkipWS(line+6);bool done=gbEvalCond(ctx,cond);
        if(!done)ctx->curLine=ctx->ctrl[ctx->ctrlTop-1].startLine;else ctx->ctrlTop--;
        return;
    }

    // -- BREAK / CONTINUE ------------------------------------------
    if(gbSW(line,"BREAK")){
        for(int i=ctx->ctrlTop-1;i>=0;i--){
            if(ctx->ctrl[i].type==GBF_FOR||ctx->ctrl[i].type==GBF_WHILE||ctx->ctrl[i].type==GBF_REPEAT){
                int sl=ctx->ctrl[i].startLine;ctx->ctrlTop=i;gbSkipToEnd(ctx,sl);return;
            }
        }
        gbError(ctx,"BREAK outside loop");return;
    }
    if(gbSW(line,"CONTINUE")){
        for(int i=ctx->ctrlTop-1;i>=0;i--){
            if(ctx->ctrl[i].type==GBF_FOR){if(ctx->ctrl[i].endLine>0)ctx->curLine=ctx->ctrl[i].endLine;return;}
            if(ctx->ctrl[i].type==GBF_WHILE){ctx->curLine=ctx->ctrl[i].startLine;return;}
        }
        gbError(ctx,"CONTINUE outside loop");return;
    }

    // -- ERROR HANDLING --------------------------------------------
    if(gbSW(line,"ON ERROR GOTO LBL ")){
        const char* ln2=gbSkipWS(line+18);int t=gbFindLabel(ctx,ln2);ctx->errHandlerLine=(t>0)?t:-1;return;
    }
    if(gbSW(line,"ON ERROR GOTO ")){ctx->errHandlerLine=atoi(gbSkipWS(line+14));return;}
    if(gbSW(line,"RESUME")){ctx->inErrHandler=false;return;}

    // -- RANDOM / RAND ---------------------------------------------
    // RANDOM(X,Y) -- clamp Y>=X, return random int in [X..Y]
    if(gbSW(line,"RANDOM(")){
        const char* p=line+7;
        GBVal lo=gbEvalExpr(ctx,&p);p=gbSkipWS(p);if(*p==',')p++;
        GBVal hi=gbEvalExpr(ctx,&p);p=gbSkipWS(p);if(*p==')')p++;
        // find INTO VAR (optional)
        char vb[32]="DEFAULT_RAND";
        const char* iv=strstr(p," INTO VAR ");
        if(iv){p=gbSkipWS(iv+10);int vi=0;while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';}
        long lv=(long)lo.num,hv=(long)hi.num;
        if(hv<lv){long tmp=lv;lv=hv;hv=tmp;}  // swap if Y<X -- never fatal
        long result=lv+(long)random(hv-lv+1);
        gbSetNum(ctx->store,vb,(double)result,ctx->idx);return;
    }
    // RANDINT A TO B INTO VAR V
    if(gbSW(line,"RANDINT ")){
        const char* p=gbSkipWS(line+8);
        GBVal a=gbEvalExpr(ctx,&p);p=gbSkipWS(p);if(!gbMW(&p,"TO")){gbError(ctx,"RANDINT: missing TO");return;}
        GBVal b=gbEvalExpr(ctx,&p);p=gbSkipWS(p);if(!gbMW(&p,"INTO")){gbError(ctx,"RANDINT: missing INTO VAR");return;}
        if(gbMW(&p,"VAR"))p=gbSkipWS(p);char vb[32];int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';
        long lo=(long)a.num,hi=(long)b.num;if(hi<lo){long t=lo;lo=hi;hi=t;}
        gbSetNum(ctx->store,vb,(double)(lo+random(hi-lo+1)),ctx->idx);return;
    }
    // RAND INTO VAR V  (0.0-1.0)
    if(gbSW(line,"RAND INTO VAR ")){
        const char* p=gbSkipWS(line+14);char vb[32];int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';
        gbSetNum(ctx->store,vb,(double)random(1000000)/1000000.0,ctx->idx);return;
    }

    // -- STRING OPERATIONS -----------------------------------------
    if(gbSW(line,"UPPER ")){
        const char* p=gbSkipWS(line+6);const char* iv=strstr(p," INTO VAR ");
        if(!iv){gbError(ctx,"UPPER: missing INTO VAR");return;}
        char expr[GB_MAX_LINE_LEN];int el=min((int)(iv-p),GB_MAX_LINE_LEN-1);memcpy(expr,p,el);expr[el]='\0';
        GBVal sv=gbEvalAndChain(ctx,expr);char buf[GB_MAX_LINE_LEN];gbToStr(sv,buf,sizeof(buf));
        for(int i=0;buf[i];i++)buf[i]=toupper((unsigned char)buf[i]);
        char vb[32]="";int vi=0;const char* vp=gbSkipWS(iv+10);while((isupper((unsigned char)*vp)||isdigit((unsigned char)*vp))&&vi<31)vb[vi++]=*vp++;vb[vi]='\0';
        gbSetStr(ctx->store,vb,buf,ctx->idx);return;
    }
    if(gbSW(line,"LOWER ")){
        const char* p=gbSkipWS(line+6);const char* iv=strstr(p," INTO VAR ");
        if(!iv){gbError(ctx,"LOWER: missing INTO VAR");return;}
        char expr[GB_MAX_LINE_LEN];int el=min((int)(iv-p),GB_MAX_LINE_LEN-1);memcpy(expr,p,el);expr[el]='\0';
        GBVal sv=gbEvalAndChain(ctx,expr);char buf[GB_MAX_LINE_LEN];gbToStr(sv,buf,sizeof(buf));
        for(int i=0;buf[i];i++)buf[i]=tolower((unsigned char)buf[i]);
        char vb[32]="";int vi=0;const char* vp=gbSkipWS(iv+10);while((isupper((unsigned char)*vp)||isdigit((unsigned char)*vp))&&vi<31)vb[vi++]=*vp++;vb[vi]='\0';
        gbSetStr(ctx->store,vb,buf,ctx->idx);return;
    }
    if(gbSW(line,"JOIN ")){
        const char* p=gbSkipWS(line+5);const char* iv=strstr(p," INTO VAR ");
        if(!iv){gbError(ctx,"JOIN: missing INTO VAR");return;}
        char expr[GB_MAX_LINE_LEN];int el=min((int)(iv-p),GB_MAX_LINE_LEN-1);memcpy(expr,p,el);expr[el]='\0';
        GBVal res=gbEvalAndChain(ctx,expr);char buf[GB_MAX_LINE_LEN];gbToStr(res,buf,sizeof(buf));
        char vb[32]="";int vi=0;const char* vp=gbSkipWS(iv+10);while((isupper((unsigned char)*vp)||isdigit((unsigned char)*vp))&&vi<31)vb[vi++]=*vp++;vb[vi]='\0';
        gbSetStr(ctx->store,vb,buf,ctx->idx);return;
    }
    if(gbSW(line,"FIND ")){
        const char* p=gbSkipWS(line+5);GBVal needle=gbEvalExpr(ctx,&p);p=gbSkipWS(p);
        if(gbMW(&p,"IN"))p=gbSkipWS(p);if(gbMW(&p,"VAR"))p=gbSkipWS(p);
        char sn[32]={};int si=0;while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&si<31)sn[si++]=*p++;p=gbSkipWS(p);
        if(gbMW(&p,"INTO"))p=gbSkipWS(p);if(gbMW(&p,"VAR"))p=gbSkipWS(p);
        char vb[32]={};int vi=0;while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';
        GBVal hs=gbGetVal(ctx->store,sn,ctx->idx);char ns[GB_MAX_LINE_LEN],hss[GB_MAX_LINE_LEN];
        gbToStr(needle,ns,sizeof(ns));gbToStr(hs,hss,sizeof(hss));
        char* found=strstr(hss,ns);gbSetNum(ctx->store,vb,found?(double)(found-hss):-1,ctx->idx);return;
    }
    if(gbSW(line,"SLICE ")){
        const char* p=gbSkipWS(line+6);GBVal sv=gbEvalExpr(ctx,&p);p=gbSkipWS(p);
        if(gbMW(&p,"FROM"))p=gbSkipWS(p);GBVal fv=gbEvalExpr(ctx,&p);p=gbSkipWS(p);
        if(gbMW(&p,"TO"))p=gbSkipWS(p);GBVal tv2=gbEvalExpr(ctx,&p);p=gbSkipWS(p);
        if(gbMW(&p,"INTO"))p=gbSkipWS(p);if(gbMW(&p,"VAR"))p=gbSkipWS(p);
        char vb[32]={};int vi=0;while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';
        char src_s[GB_MAX_LINE_LEN];gbToStr(sv,src_s,sizeof(src_s));
        int f=(int)fv.num,t2=(int)tv2.num,sl=(int)strlen(src_s);
        f=max(0,min(f,sl));t2=max(f,min(t2,sl));
        char res[GB_MAX_LINE_LEN];memcpy(res,src_s+f,t2-f);res[t2-f]='\0';
        gbSetStr(ctx->store,vb,res,ctx->idx);return;
    }
    if(gbSW(line,"NUMTOSTR ")){
        const char* p=gbSkipWS(line+9);const char* iv=strstr(p," INTO VAR ");
        if(!iv){gbError(ctx,"NUMTOSTR: missing INTO VAR");return;}
        char expr[GB_MAX_LINE_LEN];int el=min((int)(iv-p),GB_MAX_LINE_LEN-1);memcpy(expr,p,el);expr[el]='\0';
        GBVal nv=gbEvalAndChain(ctx,expr);char ns[32];gbToStr(nv,ns,sizeof(ns));
        char vb[32]="";int vi=0;const char* vp=gbSkipWS(iv+10);while((isupper((unsigned char)*vp)||isdigit((unsigned char)*vp))&&vi<31)vb[vi++]=*vp++;vb[vi]='\0';
        gbSetStr(ctx->store,vb,ns,ctx->idx);return;
    }
    if(gbSW(line,"STRTONUM ")){
        const char* p=gbSkipWS(line+9);const char* iv=strstr(p," INTO VAR ");
        if(!iv){gbError(ctx,"STRTONUM: missing INTO VAR");return;}
        char expr[GB_MAX_LINE_LEN];int el=min((int)(iv-p),GB_MAX_LINE_LEN-1);memcpy(expr,p,el);expr[el]='\0';
        GBVal sv=gbEvalAndChain(ctx,expr);char buf[GB_MAX_LINE_LEN];gbToStr(sv,buf,sizeof(buf));
        char vb[32]="";int vi=0;const char* vp=gbSkipWS(iv+10);while((isupper((unsigned char)*vp)||isdigit((unsigned char)*vp))&&vi<31)vb[vi++]=*vp++;vb[vi]='\0';
        gbSetNum(ctx->store,vb,atof(buf),ctx->idx);return;
    }

    // -- GPIO ------------------------------------------------------
    if(gbSW(line,"READGPIO ")){
        const char* p=gbSkipWS(line+9);int pin=(int)gbEvalExpr(ctx,&p).num;p=gbSkipWS(p);
        if(!gbMW(&p,"INTO")){gbError(ctx,"READGPIO: missing INTO VAR");return;}
        if(gbMW(&p,"VAR"))p=gbSkipWS(p);char vb[32];int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';
        pinMode(pin,INPUT);gbSetNum(ctx->store,vb,(double)digitalRead(pin),ctx->idx);return;
    }
    if(gbSW(line,"WRITEGPIO ")){
        const char* p=gbSkipWS(line+10);int pin=(int)gbEvalExpr(ctx,&p).num;p=gbSkipWS(p);
        if(!gbMW(&p,"VALUE")){gbError(ctx,"WRITEGPIO: missing VALUE");return;}
        GBVal val=gbEvalExpr(ctx,&p);pinMode(pin,OUTPUT);digitalWrite(pin,(int)val.num?HIGH:LOW);return;
    }
    if(gbSW(line,"SETGPIO ")){
        const char* p=gbSkipWS(line+8);int pin=(int)gbEvalExpr(ctx,&p).num;p=gbSkipWS(p);
        if(!gbMW(&p,"MODE")){gbError(ctx,"SETGPIO: missing MODE");return;}
        if(gbMW(&p,"INPUT_PULLUP")||gbMW(&p,"PULLUP"))pinMode(pin,INPUT_PULLUP);
        else if(gbMW(&p,"INPUT"))pinMode(pin,INPUT);
        else if(gbMW(&p,"OUTPUT"))pinMode(pin,OUTPUT);
        return;
    }
    if(gbSW(line,"READADC ")){
        const char* p=gbSkipWS(line+8);int pin=(int)gbEvalExpr(ctx,&p).num;p=gbSkipWS(p);
        if(!gbMW(&p,"INTO")){gbError(ctx,"READADC: missing INTO VAR");return;}
        if(gbMW(&p,"VAR"))p=gbSkipWS(p);char vb[32];int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';
        analogReadResolution(16);gbSetNum(ctx->store,vb,(double)analogRead(pin),ctx->idx);return;
    }

    // -- I2C -------------------------------------------------------
    if(gbSW(line,"READI2C ")){
        const char* p=gbSkipWS(line+8);int addr=(int)gbEvalExpr(ctx,&p).num;p=gbSkipWS(p);
        int reg=-1,count=1;
        if(gbMW(&p,"REG")){reg=(int)gbEvalExpr(ctx,&p).num;p=gbSkipWS(p);}
        if(gbMW(&p,"COUNT")){count=(int)gbEvalExpr(ctx,&p).num;p=gbSkipWS(p);}
        if(!gbMW(&p,"INTO")){gbError(ctx,"READI2C: missing INTO VAR");return;}
        if(gbMW(&p,"VAR"))p=gbSkipWS(p);char vb[32];int vi=0;
        while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';
        Wire.begin();
        if(reg>=0){Wire.beginTransmission((uint8_t)addr);Wire.write((uint8_t)reg);if(Wire.endTransmission(false)!=0){gbError(ctx,"READI2C: no ACK from 0x%02X",addr);return;}}
        uint8_t rcvd=Wire.requestFrom((uint8_t)addr,(uint8_t)max(1,count));
        if(!rcvd){gbError(ctx,"READI2C: no response from 0x%02X",addr);return;}
        if(count==1)gbSetNum(ctx->store,vb,(double)Wire.read(),ctx->idx);
        else{char buf[GB_MAX_LINE_LEN]="",bb[8];while(Wire.available()){snprintf(bb,sizeof(bb),"0x%02X ",(uint8_t)Wire.read());strncat(buf,bb,sizeof(buf)-strlen(buf)-1);}gbSetStr(ctx->store,vb,buf,ctx->idx);}
        return;
    }
    if(gbSW(line,"WRITEI2C ")){
        const char* p=gbSkipWS(line+9);int addr=(int)gbEvalExpr(ctx,&p).num;p=gbSkipWS(p);
        int reg=0;if(gbMW(&p,"REG")){reg=(int)gbEvalExpr(ctx,&p).num;p=gbSkipWS(p);}
        if(!gbMW(&p,"VALUE")){gbError(ctx,"WRITEI2C: missing VALUE");return;}
        GBVal val=gbEvalExpr(ctx,&p);
        Wire.begin();Wire.beginTransmission((uint8_t)addr);Wire.write((uint8_t)reg);Wire.write((uint8_t)val.num);Wire.endTransmission();
        return;
    }
    if(gbSW(line,"CLAIM I2C ")){const char* p=gbSkipWS(line+10);int addr=(int)gbEvalExpr(ctx,&p).num;if(ctx->i2cClaimCount<16)ctx->i2cClaims[ctx->i2cClaimCount++]=(uint8_t)addr;return;}
    if(gbSW(line,"RELEASE ALL I2C")){ctx->i2cClaimCount=0;return;}
    if(gbSW(line,"RELEASE I2C ")){
        const char* p=gbSkipWS(line+12);uint8_t addr=(uint8_t)gbEvalExpr(ctx,&p).num;
        for(int i=0;i<ctx->i2cClaimCount;i++)if(ctx->i2cClaims[i]==addr){memmove(ctx->i2cClaims+i,ctx->i2cClaims+i+1,ctx->i2cClaimCount-i-1);ctx->i2cClaimCount--;break;}
        return;
    }

    // -- FLASHPWMGPIO [pin] [freq]  (0=stop, 1-1024 Hz) -----------
    if(gbSW(line,"FLASHPWMGPIO ")){
        const char* p=gbSkipWS(line+13);
        int pin=(int)gbEvalExpr(ctx,&p).num;p=gbSkipWS(p);
        int freq=(int)gbEvalExpr(ctx,&p).num;
        int slot=-1;for(int i=0;i<ctx->pwmCount;i++)if(ctx->pwmPins[i]==(uint8_t)pin){slot=i;break;}
        if(freq<=0){
            if(slot>=0){delete ctx->pwmOuts[slot];ctx->pwmOuts[slot]=nullptr;memmove(ctx->pwmPins+slot,ctx->pwmPins+slot+1,ctx->pwmCount-slot-1);memmove(ctx->pwmOuts+slot,ctx->pwmOuts+slot+1,(ctx->pwmCount-slot-1)*sizeof(mbed::PwmOut*));ctx->pwmCount--;}
            return;
        }
        if(freq>1024)freq=1024;
        if(slot<0&&ctx->pwmCount<GB_MAX_PWM){slot=ctx->pwmCount++;ctx->pwmPins[slot]=(uint8_t)pin;ctx->pwmOuts[slot]=new mbed::PwmOut(digitalPinToPinName(pin));}
        if(slot>=0&&ctx->pwmOuts[slot]){ctx->pwmOuts[slot]->period(1.0f/freq);ctx->pwmOuts[slot]->write(0.5f);}
        return;
    }

    // -- FILE OPS --------------------------------------------------
    if(gbSW(line,"ADD ")&&strstr(line," TO FILE ")){
        const char* tf=strstr(line," TO FILE ");if(!tf){gbError(ctx,"ADD TO FILE: missing TO FILE");return;}
        char expr[GB_MAX_LINE_LEN];int el=min((int)(tf-(line+4)),GB_MAX_LINE_LEN-1);memcpy(expr,line+4,el);expr[el]='\0';
        GBVal val=gbEvalAndChain(ctx,expr);const char* fn=gbSkipWS(tf+9);char fb[48];int fi=0;
        while(*fn&&*fn!=' '&&fi<47)fb[fi++]=*fn++;fb[fi]='\0';
        char vs[GB_MAX_LINE_LEN];gbToStr(val,vs,sizeof(vs));strncat(vs,"\n",sizeof(vs)-strlen(vs)-1);
        if(!gbFSAppend(fb,vs,ctx->name))gbError(ctx,"ADD TO FILE: write denied or FS full");
        return;
    }
    if(gbSW(line,"READ ")&&strstr(line," INTO VAR ")){
        const char* p=gbSkipWS(line+5);char fb[48];int fi=0;
        while(*p&&*p!=' '&&fi<47)fb[fi++]=*p++;fb[fi]='\0';p=gbSkipWS(p);
        if(gbMW(&p,"INTO"))p=gbSkipWS(p);if(gbMW(&p,"VAR"))p=gbSkipWS(p);
        char vb[32];int vi=0;while((isupper((unsigned char)*p)||isdigit((unsigned char)*p))&&vi<31)vb[vi++]=*p++;vb[vi]='\0';
        String c2=gbFSRead(fb);if(!gbFSFind(fb)){gbError(ctx,"READ: \"%s\" not found",fb);return;}
        gbSetStr(ctx->store,vb,c2.c_str(),ctx->idx);return;
    }
    if(gbSW(line,"DELETE FILE ")){
        const char* p=gbSkipWS(line+12);char fb[48];int fi=0;while(*p&&*p!=' '&&fi<47)fb[fi++]=*p++;fb[fi]='\0';
        if(!gbFSDelete(fb,ctx->name,false))gbError(ctx,"DELETE FILE: \"%s\" not found or access denied",fb);return;
    }
    if(gbSW(line,"LIST FILES")){
        if(!gBFS){gbOut(ctx,"no filesystem",COL_DIM);return;}
        bool any=false;
        for(int i=0;i<GB_TEMPFS_MAXFILES;i++){
            if(!gBFS->files[i].active)continue;
            char buf[GB_MAX_LINE_LEN];
            snprintf(buf,sizeof(buf),"  %-32s %5luB  %s",gBFS->files[i].name,gBFS->files[i].size,gBFS->files[i].owner[0]?gBFS->files[i].owner:"(unowned)");
            gbOut(ctx,buf);any=true;
        }
        if(!any)gbOut(ctx,"  (no files)",COL_DIM);
        return;
    }

    // -- GIGAOS command passthrough (whitelist) --------------------
    if(gbSW(line,"GIGAOS ")){
        const char* p=gbSkipWS(line+7);char cmd[GB_MAX_LINE_LEN]="";
        if(*p=='"'){p++;int i=0;while(*p&&*p!='"'&&i<GB_MAX_LINE_LEN-1)cmd[i++]=*p++;cmd[i]='\0';}
        static const char* allow[]={"wifi status","ping ","ifconfig","date","uptime","cpustat","mem",
            "nslookup ","uname","threads","rtc show","rtc time","rtc date",
            "i2c scan","gpio ","hex ","bin ","units ","ps","gbasic list","gbasic files",nullptr};
        bool ok=false;for(int i=0;allow[i];i++)if(gbSW(cmd,allow[i])){ok=true;break;}
        if(!ok){gbError(ctx,"GIGAOS \"%s\" not permitted",cmd);return;}
        extern void dispatch(String);dispatch(String(cmd));
        return;
    }

    // -- unknown command -- exact text, no autocorrect --------------
    gbError(ctx,"unknown command: %s",line);
}

// ================================================================
// INTERPRETER CORE LOOP
// ================================================================
static void gbRunScript(GBCtx* ctx){
    ctx->running=true;ctx->curLine=1;ctx->ctrlTop=0;ctx->callDepth=0;ctx->inErrHandler=false;
    char lb[GB_MAX_LINE_LEN];
    while(ctx->running&&!ctx->abortFlag){
        if(ctx->curLine<1||ctx->curLine>ctx->lineCount)break;
        int tl=ctx->curLine;gbGetLine(ctx,tl,lb);
        ctx->curLine=tl+1;
        gbExecuteLine(ctx,lb,tl);
    }
}

// ================================================================
// FOREGROUND TASK  -- runs GBasic/fg/NAME.gbpf thread
// ================================================================
static void gbFgTaskFn(){
    while(true){
        GBCtx* ctx=&gBCtx[0];
        if(!ctx->running||!ctx->src){rtos::ThisThread::sleep_for(100);continue;}
        gbRunScript(ctx);
        ctx->running=false;
        ctx->i2cClaimCount=0;
        for(int i=0;i<ctx->pwmCount;i++){if(ctx->pwmOuts[i]){delete ctx->pwmOuts[i];ctx->pwmOuts[i]=nullptr;}}
        ctx->pwmCount=0;
        gbRnPrint("",GB_RN_TXT);gbRnPrint("--- finished. press any key to return ---",COL_DIM);
        gBRnDirty=true;
        gBRnAsk=true;gBRnEvt.wait_any(0x02);gBRnAsk=false;
        gOSMode=GMODE_SHELL;
        extern volatile bool termDirty;termDirty=true;
    }
}

// ================================================================
// SCHEDULER TASK  -- GigaOS/scheduler thread
// ================================================================
void gbSchedulerTaskFn(){
    while(true){
        rtos::ThisThread::sleep_for(60000);
        tm t;_rtc_localtime(time(NULL),&t,RTC_4_YEAR_LEAP_YEAR_SUPPORT);
        for(int i=0;i<gBSchedN;i++){
            GBSched& s=gBSched[i];if(!s.active)continue;
            bool fire=false;
            switch(s.type){
                case 0:if(!s.firedOnce&&t.tm_hour==s.hour&&t.tm_min==s.min){fire=true;s.firedOnce=true;}break;
                case 1:if(t.tm_hour==s.hour&&t.tm_min==s.min)fire=true;break;
                case 2:if(t.tm_wday==s.dow&&t.tm_hour==s.hour&&t.tm_min==s.min)fire=true;break;
                case 3:if(millis()-s.lastFireMs>=s.intervalMs)fire=true;break;
            }
            if(!fire)continue;
            s.lastFireMs=millis();
            GBFSEntry* e=gbFSFind(s.name);if(!e)continue;
            int slot=gbAllocSlot();
            if(slot<0){termPrint("GBasic scheduler: no free slots",COL_ERROR);continue;}
            GBCtx* ctx=&gBCtx[slot];
            memset(ctx,0,sizeof(GBCtx));
            strncpy(ctx->name,s.name,63);ctx->isFg=false;ctx->idx=(uint8_t)slot;
            ctx->src=(char*)(GB_TEMPFS_BASE+e->offset);ctx->srcLen=e->size;
            ctx->errHandlerLine=-1;ctx->store=gbGetStore(slot);gbInitStore(ctx->store,(uint8_t)slot);
            snprintf(ctx->threadName,sizeof(ctx->threadName),"GBasic/sched/%s",s.name);
            ctx->thread=new rtos::Thread(osPriorityLow,8192,nullptr,ctx->threadName);
            if(ctx->thread){ctx->running=true;ctx->thread->start([ctx](){gbRunScript(ctx);gbFreeSlot(ctx->idx);delete ctx->thread;ctx->thread=nullptr;});}
        }
    }
}

// ================================================================
// RUNNER UI  -- dark terminal for foreground script I/O
// also loss again
// I  | II
// -------
// II | I_
// ================================================================
static void gbRunnerDraw(){
    if(!gBRnDirty)return;
    dispMtx.lock();
    display.fillRect(0,TERM_Y,DISP_W,GB_RN_HDR_H,GB_RN_HDR);
    display.setTextSize(1);display.setTextColor(GB_RN_HTXT);display.setCursor(4,TERM_Y+4);
    char hdr[96];snprintf(hdr,sizeof(hdr)," %s  |  Running  |  Ctrl+C to stop",gBCtx[0].name[0]?gBCtx[0].name:"GBasic");
    display.print(hdr);
    display.fillRect(0,GB_RN_TOP,DISP_W,TERM_H-GB_RN_HDR_H-TERM_CH,GB_RN_BG);
    display.setTextSize(TERM_FONT_SZ);
    int first=max(0,gBRnCount-GB_RN_ROWS);
    for(int i=0;i<gBRnCount-first;i++){
        display.setCursor(0,GB_RN_TOP+i*TERM_CH);
        display.setTextColor(gBRnBuf[first+i].color);
        display.print(gBRnBuf[first+i].text);
    }
    display.fillRect(0,GB_RN_INPUT_Y,DISP_W,TERM_CH,GB_RN_BG);
    if(gBRnAsk){
        display.setCursor(0,GB_RN_INPUT_Y);display.setTextColor(COL_PROMPT);display.print("> ");
        display.setTextColor(COL_INPUT);display.print(gBRnAskBuf);
        int cx=(2+gBRnAskLen)*TERM_CW;display.fillRect(cx,GB_RN_INPUT_Y,TERM_CW-2,TERM_CH,COL_INPUT);
    }
    dispMtx.unlock();gBRnDirty=false;
}

void gbRunnerLoop(){
    if(gBRnDirty)gbRunnerDraw();
    extern Keyboard keyb;extern uint8_t lastHIDKey;extern uint32_t lastKeyMs;
    if(!keyb.available())return;
    auto rk=keyb.read();uint8_t hid=rk.keys[0];char c=keyb.getAscii(rk);uint32_t now=millis();
    if(hid==lastHIDKey&&(now-lastKeyMs)<100)return;
    if(hid!=lastHIDKey&&(now-lastKeyMs)<10)return;
    lastHIDKey=hid;lastKeyMs=now;
    // Ctrl+C
    if(c==3){gBCtx[0].abortFlag=true;gbRnPrint("^C -- interrupted",COL_ERROR);gBRnDirty=true;return;}
    if(gBRnAsk){
        if(hid==0x2A||c==8||c==127){if(gBRnAskLen>0){gBRnAskBuf[--gBRnAskLen]='\0';gBRnDirty=true;}}
        else if(c=='\r'||c=='\n'){
            gBRnAskBuf[gBRnAskLen]='\0';
            char echo[GB_MAX_LINE_LEN];snprintf(echo,sizeof(echo),"> %s",gBRnAskBuf);
            gbRnPrint(echo,COL_INPUT);gBRnEvt.set(0x01);gBRnDirty=true;
        } else if(c>=32&&c<127&&gBRnAskLen<GB_MAX_LINE_LEN-1){gBRnAskBuf[gBRnAskLen++]=c;gBRnAskBuf[gBRnAskLen]='\0';gBRnDirty=true;}
        else if(c!=0)gBRnEvt.set(0x02);
    } else if(c!=0)gBRnEvt.set(0x02);
}

// ================================================================
// EDITOR UI  -- light mode coding interface
// ================================================================
static const char* gbKw[]={"STORE","ADD","SUB","MUL","DIV","INC","DEC","SWAP","CLEAR","COPY",
    "DISP","DISPLN","OUTPUT","CLS","BEEP","ASK","PAUSE","WAITTIME","DOAT","WAIT",
    "GOTO","LBL","GOSUB","RETURN","IF","THEN","ELSE","END","FOR","FROM","TO","STEP",
    "WHILE","REPEAT","UNTIL","BREAK","CONTINUE","STOP","ON","ERROR","RESUME",
    "AND","OR","NOT","IS","VAR","IN","BY","INTO","FILE","BACKGROUND","AS",
    "SCHEDULE","UNSCHEDULE","CLAIM","RELEASE","ADOPT","PRIVATE","PUBLIC",
    "GIGAOS","READGPIO","WRITEGPIO","SETGPIO","READADC","READI2C","WRITEI2C",
    "RUN","RUNMODE","FLASHPWMGPIO","RANDOM","RANDINT","RAND",
    "GETDATE","GETTIME","GETMS","GETTEMP","GETCPU","GETMEM","GETIP","GETWIFI",
    "NUMTOSTR","STRTONUM","UPPER","LOWER","JOIN","FIND","SLICE","LIST","READ","DELETE",
    nullptr};

static uint16_t gbLineColor(const char* line){
    const char* p=gbSkipWS(line);
    if(*p=='#')return GB_ED_CMT;
    for(int i=0;gbKw[i];i++){size_t kl=strlen(gbKw[i]);if(gbSW(p,gbKw[i])&&(p[kl]==' '||p[kl]=='\0'||p[kl]=='\t'))return GB_ED_KW;}
    return GB_ED_TEXT;
}

static void gbEdInit(const char* fn){
    memset(gBEd,0,sizeof(GBEditor));strncpy(gBEd->filename,fn,63);
    for(int i=0;i<GB_MAX_LINES;i++){if(gBEd->lines[i]){free(gBEd->lines[i]);gBEd->lines[i]=nullptr;}}
    gBEd->lineCount=0;gBEd->curLine=0;gBEd->curCol=0;gBEd->scrollTop=0;gBEd->modified=false;
    strncpy(gBEd->statusMsg,"Ctrl+S save  Ctrl+Q quit  Ctrl+G goto  Ctrl+D del line",79);
}

static void gbEdLoad(const char* fn){
    gbEdInit(fn);
    GBFSEntry* e=gbFSFind(fn);
    if(!e){gBEd->lines[0]=(char*)malloc(GB_MAX_LINE_LEN);if(gBEd->lines[0])gBEd->lines[0][0]='\0';gBEd->lineCount=1;return;}
    char* src=(char*)(GB_TEMPFS_BASE+e->offset);int pos=0,lc=0;
    while(pos<(int)e->size&&lc<GB_MAX_LINES){
        int end=pos;while(end<(int)e->size&&src[end]!='\n'&&src[end]!='\r')end++;
        int len=min(end-pos,GB_MAX_LINE_LEN-1);
        gBEd->lines[lc]=(char*)malloc(GB_MAX_LINE_LEN);
        if(gBEd->lines[lc]){memcpy(gBEd->lines[lc],src+pos,len);gBEd->lines[lc][len]='\0';}
        lc++;pos=end;if(pos<(int)e->size&&(src[pos]=='\n'||src[pos]=='\r'))pos++;if(pos<(int)e->size&&src[pos]=='\r')pos++;
    }
    if(lc==0){gBEd->lines[0]=(char*)malloc(GB_MAX_LINE_LEN);if(gBEd->lines[0])gBEd->lines[0][0]='\0';lc=1;}
    gBEd->lineCount=lc;
}

static void gbEdSave(){
    gbFSDelete(gBEd->filename,"",true);
    gbFSCreate(gBEd->filename,"");
    for(int i=0;i<gBEd->lineCount;i++){
        if(gBEd->lines[i]){gbFSAppend(gBEd->filename,gBEd->lines[i],"");gbFSAppend(gBEd->filename,"\n","");}
    }
    gBEd->modified=false;snprintf(gBEd->statusMsg,80,"Saved %s",gBEd->filename);
}

// -- draw helpers ------------------------------------------------
static void gbEdDrawStatus() {
    display.fillRect(0, GB_ED_STAT_Y, DISP_W, GB_ED_STAT_H, GB_ED_STAT);
    display.setTextSize(1);
    display.setTextColor(0xFFFF);
    display.setCursor(4, GB_ED_STAT_Y + 4);
    char stat[128];
    snprintf(stat, sizeof(stat), " line %d/%d  col %d  |  %s",
             gBEd->curLine+1, gBEd->lineCount,
             gBEd->curCol+1,  gBEd->statusMsg);
    display.print(stat);
}

static void gbEdDrawLine(int li) {
    if (li < gBEd->scrollTop || li >= gBEd->scrollTop + GB_ED_ROWS) return;
    int row = li - gBEd->scrollTop;
    int y   = GB_ED_TOP + row * TERM_CH;
    // clear the line
    display.fillRect(0, y, DISP_W, TERM_CH, GB_ED_BG);
    // line number
    display.setTextSize(TERM_FONT_SZ);
    display.setTextColor(GB_ED_LNUM);
    display.setCursor(0, y);
    char ln[6]; snprintf(ln, sizeof(ln), "%4d", li+1);
    display.print(ln);
    // code text
    if (gBEd->lines[li]) {
        uint16_t lc = (li == gBEd->curLine) ? GB_ED_TEXT : gbLineColor(gBEd->lines[li]);
        display.setTextColor(lc);
        display.setCursor(GB_ED_CODEX, y);
        display.print(gBEd->lines[li]);
    }
    // cursor highlight on active line
    if (li == gBEd->curLine) {
        display.drawRect(0, y, DISP_W, TERM_CH, GB_ED_KW);
        if (gBEd->lines[li]) {
            int cx = GB_ED_CODEX + gBEd->curCol * TERM_CW;
            display.fillRect(cx, y, TERM_CW - 1, TERM_CH, GB_ED_KW);
            if (gBEd->curCol < (int)strlen(gBEd->lines[li])) {
                display.setTextColor(GB_ED_BG);
                display.setCursor(cx, y);
                display.print(gBEd->lines[li][gBEd->curCol]);
            }
        }
    }
}

static void gbEdDraw() {
    dispMtx.lock();
    display.fillRect(0, TERM_Y, DISP_W, TERM_H, GB_ED_BG);
    // header
    display.fillRect(0, TERM_Y, DISP_W, GB_ED_HDR_H, GB_ED_HDR);
    display.setTextSize(1); display.setTextColor(0xFFFF);
    display.setCursor(4, TERM_Y+4);
    char hdr[128];
    snprintf(hdr, sizeof(hdr), " %s%s    Ctrl+S save  Ctrl+Q quit  Esc exit",
             gBEd->filename, gBEd->modified ? "  *" : "");
    display.print(hdr);
    // all visible lines
    for (int row = 0; row < GB_ED_ROWS; row++) {
        int li = gBEd->scrollTop + row;
        if (li >= gBEd->lineCount) break;
        gbEdDrawLine(li);
    }
    gbEdDrawStatus();
    dispMtx.unlock();
    gBEdFullDirty = false;
    gBEdLineDirty = false;
}

// ================================================================
// EDITOR HELPER FUNCTIONS
// ================================================================
static void gbEdInsert(char c){
    if(!gBEd->lines[gBEd->curLine])return;
    int len=strlen(gBEd->lines[gBEd->curLine]);
    if(len>=GB_MAX_LINE_LEN-2)return;
    memmove(gBEd->lines[gBEd->curLine]+gBEd->curCol+1,
            gBEd->lines[gBEd->curLine]+gBEd->curCol,
            len-gBEd->curCol+1);
    gBEd->lines[gBEd->curLine][gBEd->curCol]=c;
    gBEd->curCol++;
    gBEd->modified=true;
}

static void gbEdBS(){
    if(!gBEd->lines[gBEd->curLine])return;
    if(gBEd->curCol>0){
        int len=strlen(gBEd->lines[gBEd->curLine]);
        memmove(gBEd->lines[gBEd->curLine]+gBEd->curCol-1,
                gBEd->lines[gBEd->curLine]+gBEd->curCol,
                len-gBEd->curCol+1);
        gBEd->curCol--;
        gBEd->modified=true;
    } else if(gBEd->curLine>0){
        int pl=strlen(gBEd->lines[gBEd->curLine-1]);
        int cl=strlen(gBEd->lines[gBEd->curLine]);
        if(pl+cl<GB_MAX_LINE_LEN-1){
            strncat(gBEd->lines[gBEd->curLine-1],
                    gBEd->lines[gBEd->curLine],
                    GB_MAX_LINE_LEN-pl-1);
            free(gBEd->lines[gBEd->curLine]);
            memmove(gBEd->lines+gBEd->curLine,
                    gBEd->lines+gBEd->curLine+1,
                    (gBEd->lineCount-gBEd->curLine-1)*sizeof(char*));
            gBEd->lineCount--;
            gBEd->curLine--;
            gBEd->curCol=pl;
            gBEd->modified=true;
        }
    }
}

static void gbEdNewline(){
    if(gBEd->lineCount>=GB_MAX_LINES-1)return;
    char* cur=gBEd->lines[gBEd->curLine];
    int col=gBEd->curCol, len=cur?(int)strlen(cur):0;
    char* nw=(char*)malloc(GB_MAX_LINE_LEN);
    if(!nw)return;
    if(cur&&col<len){
        strncpy(nw,cur+col,GB_MAX_LINE_LEN-1);
        nw[GB_MAX_LINE_LEN-1]='\0';
        cur[col]='\0';
    } else nw[0]='\0';
    memmove(gBEd->lines+gBEd->curLine+2,
            gBEd->lines+gBEd->curLine+1,
            (gBEd->lineCount-gBEd->curLine-1)*sizeof(char*));
    gBEd->lines[gBEd->curLine+1]=nw;
    gBEd->lineCount++;
    gBEd->curLine++;
    gBEd->curCol=0;
    if(gBEd->curLine>=gBEd->scrollTop+GB_ED_ROWS)
        gBEd->scrollTop=gBEd->curLine-GB_ED_ROWS+1;
    gBEd->modified=true;
}

static void gbEdDelLine(){
    if(gBEd->lineCount<=1){
        if(gBEd->lines[0])gBEd->lines[0][0]='\0';
        gBEd->curCol=0;gBEd->modified=true;return;
    }
    free(gBEd->lines[gBEd->curLine]);
    memmove(gBEd->lines+gBEd->curLine,
            gBEd->lines+gBEd->curLine+1,
            (gBEd->lineCount-gBEd->curLine-1)*sizeof(char*));
    gBEd->lineCount--;
    if(gBEd->curLine>=gBEd->lineCount)
        gBEd->curLine=gBEd->lineCount-1;
    gBEd->curCol=0;
    gBEd->modified=true;
}

// ================================================================
// EDITOR LOOP
// ================================================================
void gbEditorLoop() {
    // Rendering: full > partial
    if (gBEdFullDirty) {
        gbEdDraw();
    } else if (gBEdLineDirty) {
        dispMtx.lock();
        if (gBEdPrevLine != gBEd->curLine)
            gbEdDrawLine(gBEdPrevLine);  // un-highlight old line
        gbEdDrawLine(gBEd->curLine);     // draw new/current line
        gbEdDrawStatus();
        dispMtx.unlock();
        gBEdLineDirty = false;
    }

    extern Keyboard  keyb;
    extern uint8_t   lastHIDKey;
    extern uint32_t  lastKeyMs;
    if (!keyb.available()) return;

    auto    rk  = keyb.read();
    uint8_t hid = rk.keys[0];
    char    c   = keyb.getAscii(rk);
    uint32_t now = millis();
    // Debounce
    if (hid == lastHIDKey && (now - lastKeyMs) < 100) return;
    if (hid != lastHIDKey && (now - lastKeyMs) < 10)  return;
    lastHIDKey = hid; lastKeyMs = now;

    // Modifier byte: bit0=LCtrl, bit4=RCtrl
    bool ctrl = (rk.lctrl || rk.rctrl);
    gBEdPrevLine = gBEd->curLine;

    // ?? Quit-confirm mode (Esc pressed while modified) ??????????
    if (gBEdQuitConfirm) {
        if (hid == 0x29) {
            // Esc again = discard and exit
            gBEdQuitConfirm = false;
            gOSMode = GMODE_SHELL;
            extern volatile bool termDirty; termDirty = true;
        } else if (ctrl && hid == 0x16) {
            // Ctrl+S = save then exit
            gBEdQuitConfirm = false;
            gbEdSave();
            gOSMode = GMODE_SHELL;
            extern volatile bool termDirty; termDirty = true;
        } else {
            // Any other key = cancel quit
            gBEdQuitConfirm = false;
            strncpy(gBEd->statusMsg,
                    "Ctrl+S save  Ctrl+Q quit  Ctrl+D del line  Ctrl+G goto", 79);
            gBEdLineDirty = true;
        }
        return;
    }

    // ?? Escape: exit (or enter quit-confirm if modified) ????????
    if (hid == 0x29) {
        if (!gBEd->modified) {
            gOSMode = GMODE_SHELL;
            extern volatile bool termDirty; termDirty = true;
        } else {
            gBEdQuitConfirm = true;
            strncpy(gBEd->statusMsg,
                    "Unsaved! Esc=discard  Ctrl+S=save & exit  other=cancel", 79);
            gBEdLineDirty = true;
        }
        return;
    }

    // ?? Ctrl+Q: same as Escape ???????????????????????????????????
    if (ctrl && hid == 0x14) {  // Q = 0x14
        if (!gBEd->modified) {
            gOSMode = GMODE_SHELL;
            extern volatile bool termDirty; termDirty = true;
        } else {
            gBEdQuitConfirm = true;
            strncpy(gBEd->statusMsg,
                    "Unsaved! Esc=discard  Ctrl+S=save & exit  other=cancel", 79);
            gBEdLineDirty = true;
        }
        return;
    }

    // ?? Ctrl+S: save ?????????????????????????????????????????????
    if (ctrl && hid == 0x16) {  // S = 0x16
        gbEdSave();
        gBEdLineDirty = true;
        return;
    }

    // ?? Ctrl+D: delete line ??????????????????????????????????????
    if (ctrl && hid == 0x07) {  // D = 0x07
        gbEdDelLine();
        gBEdFullDirty = true;
        return;
    }

    // ?? Ctrl+G: go to line ???????????????????????????????????????
    if (ctrl && hid == 0x0A) {  // G = 0x0A
        strncpy(gBEd->statusMsg, "Go to line: ", 79);
        gBEdLineDirty = true;
        return;
    }

    // ?? Ctrl+Z / Ctrl+R: undo/redo stubs ????????????????????????
    if (ctrl && hid == 0x1D) {  // Z = 0x1D
        strncpy(gBEd->statusMsg, "Undo: not yet implemented", 79);
        gBEdLineDirty = true;
        return;
    }
    if (ctrl && hid == 0x15) {  // R = 0x15
        strncpy(gBEd->statusMsg, "Redo: not yet implemented", 79);
        gBEdLineDirty = true;
        return;
    }

    // ?? Arrow keys ???????????????????????????????????????????????
    if (hid == 0x52) {  // Up
        if (gBEd->curLine > 0) {
            gBEd->curLine--;
            if (gBEd->curLine < gBEd->scrollTop) {
                gBEd->scrollTop = gBEd->curLine;
                gBEdFullDirty = true;
            } else gBEdLineDirty = true;
            int l = gBEd->lines[gBEd->curLine] ? strlen(gBEd->lines[gBEd->curLine]) : 0;
            if (gBEd->curCol > l) gBEd->curCol = l;
        }
        return;
    }
    if (hid == 0x51) {  // Down
        if (gBEd->curLine < gBEd->lineCount - 1) {
            gBEd->curLine++;
            if (gBEd->curLine >= gBEd->scrollTop + GB_ED_ROWS) {
                gBEd->scrollTop = gBEd->curLine - GB_ED_ROWS + 1;
                gBEdFullDirty = true;
            } else gBEdLineDirty = true;
            int l = gBEd->lines[gBEd->curLine] ? strlen(gBEd->lines[gBEd->curLine]) : 0;
            if (gBEd->curCol > l) gBEd->curCol = l;
        }
        return;
    }
    if (hid == 0x50) {  // Left
        if (gBEd->curCol > 0) { gBEd->curCol--; gBEdLineDirty = true; }
        return;
    }
    if (hid == 0x4F) {  // Right
        int l = gBEd->lines[gBEd->curLine] ? strlen(gBEd->lines[gBEd->curLine]) : 0;
        if (gBEd->curCol < l) { gBEd->curCol++; gBEdLineDirty = true; }
        return;
    }

    // ?? Home / End ???????????????????????????????????????????????
    if (hid == 0x4A) { gBEd->curCol = 0; gBEdLineDirty = true; return; }
    if (hid == 0x4D) {
        int l = gBEd->lines[gBEd->curLine] ? strlen(gBEd->lines[gBEd->curLine]) : 0;
        gBEd->curCol = l; gBEdLineDirty = true; return;
    }

    // ?? Page Up / Page Down ??????????????????????????????????????
    if (hid == 0x4B) {  // Page Up
        gBEd->curLine  = max(0, gBEd->curLine  - GB_ED_ROWS);
        gBEd->scrollTop = max(0, gBEd->scrollTop - GB_ED_ROWS);
        gBEdFullDirty = true; return;
    }
    if (hid == 0x4E) {  // Page Down
        gBEd->curLine  = min(gBEd->lineCount - 1, gBEd->curLine  + GB_ED_ROWS);
        gBEd->scrollTop = min(max(0, gBEd->lineCount - GB_ED_ROWS), gBEd->scrollTop + GB_ED_ROWS);
        gBEdFullDirty = true; return;
    }

    // ?? Backspace ????????????????????????????????????????????????
    if (hid == 0x2A || c == 8 || c == 127) {
        bool wasAtLineStart = (gBEd->curCol == 0);
        gbEdBS();
        if (wasAtLineStart) gBEdFullDirty = true;  // line count changed
        else                gBEdLineDirty = true;
        return;
    }

    // ?? Enter ????????????????????????????????????????????????????
    if (c == '\r' || c == '\n') {
        gbEdNewline();
        gBEdFullDirty = true;  // line count changed
        return;
    }

    // ?? Printable characters ?????????????????????????????????????
    if (c >= 32 && c < 127 && !ctrl) {
        gbEdInsert(c);
        gBEdLineDirty = true;  // only current line changed
        return;
    }
}



// ================================================================
// cmd_gbasic -- GigaOS shell dispatcher
// ================================================================
void cmd_gbasic(String args){
    args.trim();
    int sp=args.indexOf(' ');
    String sub=(sp<0)?args:args.substring(0,sp);
    String rest=(sp<0)?"":args.substring(sp+1);rest.trim();

    if(sub.equals("new")){
        gbEdLoad("UNTITLED.gbpf");gOSMode=GMODE_GBASIC_EDIT;gBEdLineDirty=true;return;
    }
    if(sub.equals("edit")){
        if(!rest.length()){termPrint("Usage: gbasic edit NAME.gbpf",COL_ERROR);return;}
        gbEdLoad(rest.c_str());gOSMode=GMODE_GBASIC_EDIT;gBEdLineDirty=true;return;
    }
    if(sub.equals("run")){
        bool bg=rest.endsWith(" background");
        String fn=bg?rest.substring(0,rest.lastIndexOf(' ')):rest;fn.trim();
        if(!fn.length()){termPrint("Usage: gbasic run NAME.gbpf [background]",COL_ERROR);return;}
        GBFSEntry* e=gbFSFind(fn.c_str());
        if(!e){termPrintf(COL_ERROR,"gbasic: \"%s\" not found",fn.c_str());return;}
        int slot=gbAllocSlot();if(slot<0){termPrint("gbasic: no free slots (max 8)",COL_ERROR);return;}
        GBCtx* ctx=&gBCtx[slot];
        memset(ctx,0,sizeof(GBCtx));strncpy(ctx->name,fn.c_str(),63);
        ctx->src=(char*)(GB_TEMPFS_BASE+e->offset);ctx->srcLen=e->size;
        ctx->idx=(uint8_t)slot;ctx->errHandlerLine=-1;
        ctx->store=gbGetStore(slot);gbInitStore(ctx->store,(uint8_t)slot);
        termPrintf(COL_DIM,"GBasic pre-check: %s",ctx->name);
        if(!gbPrePass(ctx)){termPrint("Script has errors. Not running.",COL_ERROR);gbFreeSlot(slot);return;}
        termPrintf(COL_DIM,"  %d lines, %d labels -- OK",ctx->lineCount,ctx->labelCount);
        // detect declared mode
        char lb[GB_MAX_LINE_LEN];
        for(int i=1;i<=min(10,ctx->lineCount);i++){gbGetLine(ctx,i,lb);const char* p=gbSkipWS(lb);if(gbSW(p,"RUNMODE BACKGROUND")||gbSW(p,"#!background"))ctx->declMode=GBR_BG;if(gbSW(p,"RUNMODE FOREGROUND")||gbSW(p,"#!foreground"))ctx->declMode=GBR_FG;}
        if(bg)ctx->declMode=GBR_BG;
        if(ctx->declMode==GBR_BG){
            ctx->isFg=false;snprintf(ctx->threadName,sizeof(ctx->threadName),"GBasic/bg/%s",ctx->name);
            ctx->thread=new rtos::Thread(osPriorityLow,8192,nullptr,ctx->threadName);
            if(ctx->thread){ctx->running=true;ctx->thread->start([ctx](){gbRunScript(ctx);gbFreeSlot(ctx->idx);delete ctx->thread;ctx->thread=nullptr;});}
            termPrintf(COL_INFO,"GBasic: %s started in background",ctx->name);
        } else {
            ctx->isFg=true;
            memcpy(&gBCtx[0],ctx,sizeof(GBCtx));if(slot!=0)gbFreeSlot(slot);
            snprintf(gBCtx[0].threadName,sizeof(gBCtx[0].threadName),"GBasic/fg/%s",gBCtx[0].name);
            gBRnCount=0;gOSMode=GMODE_GBASIC_RUN;gBRnDirty=true;
            gBCtx[0].running=true;gBCtx[0].abortFlag=false;
            extern rtos::Thread gbasicFgTask;
            gbasicFgTask.start(gbFgTaskFn);
        }
        return;
    }
    if(sub.equals("list")){
        if(!rest.length()){termPrint("Usage: gbasic list NAME.gbpf|NAME.gbf",COL_ERROR);return;}
        GBFSEntry* e=gbFSFind(rest.c_str());if(!e){termPrintf(COL_ERROR,"gbasic: \"%s\" not found",rest.c_str());return;}
        char* src=(char*)(GB_TEMPFS_BASE+e->offset);char lb[GB_MAX_LINE_LEN];int lnum=1,pos=0;
        while(pos<(int)e->size){int end=pos;while(end<(int)e->size&&src[end]!='\n'&&src[end]!='\r')end++;int len=min(end-pos,GB_MAX_LINE_LEN-1);memcpy(lb,src+pos,len);lb[len]='\0';char out[GB_MAX_LINE_LEN+8];snprintf(out,sizeof(out),"%4d  %s",lnum++,lb);termPrint(out,COL_TEXT);pos=end;if(pos<(int)e->size&&(src[pos]=='\n'||src[pos]=='\r'))pos++;}
        return;
    }
    if(sub.equals("files")){
        if(!gBFS){termPrint("gbasic: FS not initialized",COL_ERROR);return;}
        bool any=false;
        for(int i=0;i<GB_TEMPFS_MAXFILES;i++){
            if(!gBFS->files[i].active)continue;
            if(rest.length()>0&&!String(gBFS->files[i].name).endsWith(rest))continue;
            char buf[GB_MAX_LINE_LEN];snprintf(buf,sizeof(buf),"  %-32s %6luB  %s",gBFS->files[i].name,gBFS->files[i].size,gBFS->files[i].owner[0]?gBFS->files[i].owner:"(unowned)");
            termPrint(buf,COL_TEXT);any=true;
        }
        if(!any)termPrint("  (no files)",COL_DIM);return;
    }
    if(sub.equals("delete")){
        if(!rest.length()){termPrint("Usage: gbasic delete NAME",COL_ERROR);return;}
        if(gbFSDelete(rest.c_str(),"",true))termPrintf(COL_INFO,"Deleted %s",rest.c_str());
        else termPrintf(COL_ERROR,"gbasic: could not delete \"%s\"",rest.c_str());return;
    }
    if(sub.equals("ps")){
        termPrint("GBasic scripts:",COL_INFO);bool any=false;
        for(int i=0;i<GB_MAX_SCRIPTS;i++){if(!gBSlot[i])continue;GBCtx* c2=&gBCtx[i];char buf[TERM_LINE_LEN];snprintf(buf,sizeof(buf),"  [%d] %-32s %s  mem:%luKB",i,c2->name,c2->running?"Running":"Idle",c2->store?c2->store->heapTop/1024:0);termPrint(buf,COL_TEXT);any=true;}
        if(!any)termPrint("  (none)",COL_DIM);return;
    }
    if(sub.equals("kill")){
        if(rest.equals("all")){for(int i=0;i<GB_MAX_SCRIPTS;i++)if(gBSlot[i])gBCtx[i].abortFlag=true;termPrint("GBasic: all scripts signaled stop",COL_INFO);return;}
        for(int i=0;i<GB_MAX_SCRIPTS;i++){if(gBSlot[i]&&String(gBCtx[i].name).indexOf(rest)>=0){gBCtx[i].abortFlag=true;termPrintf(COL_INFO,"GBasic: %s signaled stop",gBCtx[i].name);return;}}
        termPrintf(COL_ERROR,"gbasic: no script matching \"%s\"",rest.c_str());return;
    }
    if(sub.equals("schedule")){
        int sp2=rest.indexOf(' ');if(sp2<0){termPrint("Usage: gbasic schedule NAME hh:mm|daily hh:mm|weekly [day] hh:mm|every N s/m/h",COL_ERROR);return;}
        String sn=rest.substring(0,sp2),sa=rest.substring(sp2+1);sa.trim();
        if(gBSchedN>=GB_MAX_SCHED){termPrint("gbasic: schedule table full",COL_ERROR);return;}
        GBSched& s=gBSched[gBSchedN];memset(&s,0,sizeof(GBSched));strncpy(s.name,sn.c_str(),63);s.active=true;
        if(sa.startsWith("every ")){
            s.type=3;String iv=sa.substring(6);int sp3=iv.indexOf(' ');String ns2=iv.substring(0,sp3),unit=iv.substring(sp3+1);float n=ns2.toFloat();
            if(unit.startsWith("s"))s.intervalMs=(uint32_t)(n*1000);else if(unit.startsWith("m"))s.intervalMs=(uint32_t)(n*60000);else s.intervalMs=(uint32_t)(n*3600000UL);
        } else if(sa.startsWith("daily ")){s.type=1;int h=0,m=0;sscanf(sa.c_str()+6,"%d:%d",&h,&m);s.hour=h;s.min=m;}
        else if(sa.startsWith("weekly ")){
            s.type=2;String r2=sa.substring(7);r2.trim();r2.toLowerCase();
            static const char* days[]={"sunday","monday","tuesday","wednesday","thursday","friday","saturday"};
            for(int d=0;d<7;d++)if(r2.startsWith(days[d])){s.dow=d;r2=r2.substring(strlen(days[d]));r2.trim();break;}
            int h=0,m=0;sscanf(r2.c_str(),"%d:%d",&h,&m);s.hour=h;s.min=m;
        } else {s.type=0;int h=0,m=0;sscanf(sa.c_str(),"%d:%d",&h,&m);s.hour=h;s.min=m;}
        gBSchedN++;termPrintf(COL_INFO,"Scheduled: %s",s.name);return;
    }
    if(sub.equals("scheduled")){
        static const char* tn[]={"once","daily","weekly","interval"};
        if(gBSchedN==0){termPrint("  (no scheduled tasks)",COL_DIM);return;}
        for(int i=0;i<gBSchedN;i++){GBSched& s=gBSched[i];if(!s.active)continue;char buf[TERM_LINE_LEN];if(s.type==3)snprintf(buf,sizeof(buf),"  %-32s %s  every %lums",s.name,tn[s.type],s.intervalMs);else snprintf(buf,sizeof(buf),"  %-32s %s  %02d:%02d",s.name,tn[s.type],s.hour,s.min);termPrint(buf,COL_TEXT);}
        return;
    }
    if(sub.equals("unschedule")){
        for(int i=0;i<gBSchedN;i++)if(String(gBSched[i].name).equals(rest)){gBSched[i].active=false;termPrintf(COL_INFO,"Unscheduled: %s",rest.c_str());return;}
        termPrintf(COL_ERROR,"gbasic: no schedule for \"%s\"",rest.c_str());return;
    }
    if(sub.equals("memstat")){
        termPrint("GBasic SDRAM variable regions:",COL_INFO);
        for(int i=0;i<GB_MAX_SCRIPTS;i++){if(!gBSlot[i])continue;GBVarStore* st=gbGetStore(i);uint32_t used=st->heapTop,pct=used*100/GB_VAR_SLOT_SIZE;char bar[21]="";for(uint32_t b=0;b<20;b++)strncat(bar,b<pct/5?"\xe2\x96\x88":".",sizeof(bar)-strlen(bar)-1);char buf[TERM_LINE_LEN];snprintf(buf,sizeof(buf),"  [%d] %-24s %luKB/256KB  %lu%%  %s",i,gBCtx[i].name,used/1024,pct,bar);termPrint(buf,COL_TEXT);}
        return;
    }
    termPrintf(COL_ERROR,"gbasic: unknown subcommand \"%s\"",sub.c_str());
    termPrint("  Subcommands: run edit new list files delete ps kill schedule scheduled unschedule memstat",COL_DIM);
}

// ================================================================
// cmd_ps / cmd_kill
// ================================================================
void cmd_ps(String args){
    const int MAX_T=24;mbed_stats_thread_t stats[MAX_T];int cnt=mbed_stats_thread_get_each(stats,MAX_T);
    termPrint("-- Processes ----------------------------------",COL_INFO);
    termPrint("  NAME                       STATE     PRIO  STACK",COL_DIM);
    static const char* sn[]={"Inactive","Ready","Running","Waiting","Error","Reserved"};
    for(int i=0;i<cnt;i++){uint32_t used=stats[i].stack_size-stats[i].stack_space;char buf[TERM_LINE_LEN];snprintf(buf,sizeof(buf),"  [%2d] %-24s %-8s %4lu  %luB/%luB",i,(const char*)"(thread)",stats[i].state<6?sn[stats[i].state]:"?",stats[i].priority,used,stats[i].stack_size);termPrint(buf,COL_TEXT);}
    termPrint("-----------------------------------------------",COL_DIM);
}

void cmd_kill(String args){
    args.trim();if(!args.length()){termPrint("Usage: kill <name>",COL_ERROR);return;}
    for(int i=0;i<GB_MAX_SCRIPTS;i++)if(gBSlot[i]&&(String(gBCtx[i].name).indexOf(args)>=0||String(gBCtx[i].threadName).indexOf(args)>=0)){gBCtx[i].abortFlag=true;termPrintf(COL_INFO,"killed: %s",gBCtx[i].name);return;}
    termPrintf(COL_ERROR,"kill: nothing matching \"%s\"",args.c_str());
}

// ================================================================
// SDRAM USAGE REPORT (called by GigaOS mem command)
// ================================================================
uint32_t gbSdramUsage() {
    uint32_t rt = sizeof(GBCtx)*GB_MAX_SCRIPTS
               + sizeof(GBRnLine)*GB_RUNNER_LINES
               + sizeof(GBEditor)
               + sizeof(GBSched)*GB_MAX_SCHED;
    uint32_t fs = gBFS ? gBFS->dataTop : 0;
    uint32_t vs = 0;
    for(int i=0;i<GB_MAX_SCRIPTS;i++)
        if(gBSlot[i]) vs += gbGetStore(i)->heapTop;
    return rt + fs + vs;
}

// ================================================================
// INIT -- called from setup() before threads start
// ================================================================
void gbInit(){
    // Allocate large runtime structures in SDRAM via SDRAM.malloc().
    // SDRAM.begin() must have been called before this (it is, in setup()).
    // These are never freed -- they live for the lifetime of the system.
    gBCtx   = (GBCtx*)    SDRAM.malloc(sizeof(GBCtx)    * GB_MAX_SCRIPTS);
    gBRnBuf = (GBRnLine*) SDRAM.malloc(sizeof(GBRnLine) * GB_RUNNER_LINES);
    gBEd    = (GBEditor*) SDRAM.malloc(sizeof(GBEditor));
    gBSched = (GBSched*)  SDRAM.malloc(sizeof(GBSched)  * GB_MAX_SCHED);

    if (!gBCtx || !gBRnBuf || !gBEd || !gBSched) {
        // Fatal -- SDRAM.malloc failed. This should never happen.
        extern void termPrint(const char*, uint16_t);
        termPrint("FATAL: GBasic SDRAM alloc failed -- check SDRAM.begin()", COL_ERROR);
        return;
    }

    memset(gBCtx,   0, sizeof(GBCtx)    * GB_MAX_SCRIPTS);
    memset(gBRnBuf, 0, sizeof(GBRnLine) * GB_RUNNER_LINES);
    memset(gBEd,    0, sizeof(GBEditor));
    memset(gBSched, 0, sizeof(GBSched)  * GB_MAX_SCHED);

    memset(gBSlot, 0, sizeof(gBSlot));
    for(int i=0;i<GB_MAX_SCRIPTS;i++) gBCtx[i].errHandlerLine=-1;
    gBSchedN=0;
    gbInitFS();
    // Clear var store headers only (not all 256KB -- that's SDRAM, it's fine)
    for(int i=0;i<GB_MAX_SCRIPTS;i++){
        GBVarStore* s=gbGetStore(i); memset(s,0,64);
    }

    // Report SDRAM usage
    uint32_t gbSdram = sizeof(GBCtx)*GB_MAX_SCRIPTS
                     + sizeof(GBRnLine)*GB_RUNNER_LINES
                     + sizeof(GBEditor)
                     + sizeof(GBSched)*GB_MAX_SCHED;
    char buf[80];
    extern void termPrint(const char*, uint16_t);
    snprintf(buf,sizeof(buf),"  GBasic SDRAM runtime: %luKB", gbSdram/1024);
    termPrint(buf, COL_DIM);
}
