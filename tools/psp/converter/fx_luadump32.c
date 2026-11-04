/*
** Lua 5.1 chunk writer for the PSP (32-bit size_t).
**
** Upstream's ldump.c (extern/lua-5.1.5/src/ldump.c) writes size_t with the
** HOST's width, and the header records it; a 64-bit PC's chunk is refused by
** the PSP's lundump ("bad header"). This is ldump.c with every size_t written
** as 4 bytes and the header saying so - the rest is unchanged, so the PSP's
** unchanged lundump.c reads it. lua_Number is whatever the converter's Lua was
** built with (the PSP luaconf.h copy: float by default).
*/
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LUA_CORE

#include "lua.h"

#include "lobject.h"
#include "lstate.h"
#include "lundump.h"

#include "fx_luadump32.h"

typedef struct {
 lua_State* L;
 lua_Writer writer;
 void* data;
 int strip;
 int status;
} DumpState;

#define DumpMem(b,n,size,D)	DumpBlock(b,(n)*(size),D)
#define DumpVar(x,D)	 	DumpMem(&x,1,sizeof(x),D)

static void DumpBlock(const void* b, size_t size, DumpState* D)
{
 if (D->status==0)
  D->status=(*D->writer)(D->L,b,size,D->data);
}

static void DumpChar(int y, DumpState* D)
{
 char x=(char)y;
 DumpVar(x,D);
}

static void DumpInt(int x, DumpState* D)
{
 int32_t v=(int32_t)x;
 DumpVar(v,D);
}

static void DumpNumber(lua_Number x, DumpState* D)
{
 DumpVar(x,D);
}

static void DumpVector(const void* b, int n, size_t size, DumpState* D)
{
 DumpInt(n,D);
 DumpMem(b,n,size,D);
}

/* PSP: size_t as 4 bytes */
static void DumpString(const TString* s, DumpState* D)
{
 if (s==NULL)  /* (ldump.c also tests getstr(s), which is never NULL) */
 {
  uint32_t size=0;
  DumpVar(size,D);
 }
 else
 {
  uint32_t size=(uint32_t)(s->tsv.len+1);		/* include trailing '\0' */
  DumpVar(size,D);
  DumpBlock(getstr(s),size,D);
 }
}

#define DumpCode(f,D)	 DumpVector(f->code,f->sizecode,sizeof(Instruction),D)

static void DumpFunction(const Proto* f, const TString* p, DumpState* D);

static void DumpConstants(const Proto* f, DumpState* D)
{
 int i,n=f->sizek;
 DumpInt(n,D);
 for (i=0; i<n; i++)
 {
  const TValue* o=&f->k[i];
  DumpChar(ttype(o),D);
  switch (ttype(o))
  {
   case LUA_TNIL:
	break;
   case LUA_TBOOLEAN:
	DumpChar(bvalue(o),D);
	break;
   case LUA_TNUMBER:
	DumpNumber(nvalue(o),D);
	break;
   case LUA_TSTRING:
	DumpString(rawtsvalue(o),D);
	break;
   default:
	break;
  }
 }
 n=f->sizep;
 DumpInt(n,D);
 for (i=0; i<n; i++) DumpFunction(f->p[i],f->source,D);
}

static void DumpDebug(const Proto* f, DumpState* D)
{
 int i,n;
 n= (D->strip) ? 0 : f->sizelineinfo;
 DumpVector(f->lineinfo,n,sizeof(int),D);
 n= (D->strip) ? 0 : f->sizelocvars;
 DumpInt(n,D);
 for (i=0; i<n; i++)
 {
  DumpString(f->locvars[i].varname,D);
  DumpInt(f->locvars[i].startpc,D);
  DumpInt(f->locvars[i].endpc,D);
 }
 n= (D->strip) ? 0 : f->sizeupvalues;
 DumpInt(n,D);
 for (i=0; i<n; i++) DumpString(f->upvalues[i],D);
}

static void DumpFunction(const Proto* f, const TString* p, DumpState* D)
{
 DumpString((f->source==p || D->strip) ? NULL : f->source,D);
 DumpInt(f->linedefined,D);
 DumpInt(f->lastlinedefined,D);
 DumpChar(f->nups,D);
 DumpChar(f->numparams,D);
 DumpChar(f->is_vararg,D);
 DumpChar(f->maxstacksize,D);
 DumpCode(f,D);
 DumpConstants(f,D);
 DumpDebug(f,D);
}

/* PSP: luaU_header with sizeof(size_t) = 4 */
static void DumpHeader(DumpState* D)
{
 char h[LUAC_HEADERSIZE];
 int x=1;
 memcpy(h,LUA_SIGNATURE,sizeof(LUA_SIGNATURE)-1);
 h[4]=(char)LUAC_VERSION;
 h[5]=(char)LUAC_FORMAT;
 h[6]=(char)*(char*)&x;				/* endianness */
 h[7]=(char)sizeof(int);
 h[8]=(char)4;					/* PSP: sizeof(size_t) */
 h[9]=(char)sizeof(Instruction);
 h[10]=(char)sizeof(lua_Number);
 h[11]=(char)(((lua_Number)0.5)==0);		/* is lua_Number integral? */
 DumpBlock(h,LUAC_HEADERSIZE,D);
}

int fxDump32(lua_State* L, int strip, lua_Writer w, void* data)
{
 DumpState D;
 const TValue* o=L->top-1;
 if (!ttisfunction(o) || clvalue(o)->c.isC) return 1;
 D.L=L;
 D.writer=w;
 D.data=data;
 D.strip=strip;
 D.status=0;
 DumpHeader(&D);
 DumpFunction(clvalue(o)->l.p,NULL,&D);
 return D.status;
}
