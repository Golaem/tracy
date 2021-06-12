#include <new>
#include <stdio.h>
#include <string.h>
#include "TracyCallstack.hpp"
#include "TracyFastVector.hpp"
#include "../common/TracyAlloc.hpp"
#include "../common/TracyStackFrames.hpp"
#include "TracyDebug.hpp"

#ifdef TRACY_HAS_CALLSTACK

#if TRACY_HAS_CALLSTACK == 1
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#  ifdef _MSC_VER
#    pragma warning( push )
#    pragma warning( disable : 4091 )
#  endif
#  include <dbghelp.h>
#  ifdef _MSC_VER
#    pragma warning( pop )
#  endif
#elif TRACY_HAS_CALLSTACK == 2 || TRACY_HAS_CALLSTACK == 3 || TRACY_HAS_CALLSTACK == 4 || TRACY_HAS_CALLSTACK == 6
#  include "../libbacktrace/backtrace.hpp"
#  include <algorithm>
#  include <dlfcn.h>
#  include <cxxabi.h>
#  include <stdlib.h>
#  include "TracyFastVector.hpp"
#elif TRACY_HAS_CALLSTACK == 5
#  include <dlfcn.h>
#  include <cxxabi.h>
#endif

#ifdef TRACY_DBGHELP_LOCK
#  include "TracyProfiler.hpp"

#  define DBGHELP_INIT TracyConcat( TRACY_DBGHELP_LOCK, Init() )
#  define DBGHELP_LOCK TracyConcat( TRACY_DBGHELP_LOCK, Lock() );
#  define DBGHELP_UNLOCK TracyConcat( TRACY_DBGHELP_LOCK, Unlock() );

extern "C"
{
    void DBGHELP_INIT;
    void DBGHELP_LOCK;
    void DBGHELP_UNLOCK;
};
#endif

namespace tracy
{

static inline char* CopyString( const char* src, size_t sz )
{
    assert( strlen( src ) == sz );
    auto dst = (char*)tracy_malloc( sz + 1 );
    memcpy( dst, src, sz );
    dst[sz] = '\0';
    return dst;
}

static inline char* CopyString( const char* src )
{
    const auto sz = strlen( src );
    auto dst = (char*)tracy_malloc( sz + 1 );
    memcpy( dst, src, sz );
    dst[sz] = '\0';
    return dst;
}

static inline char* CopyStringFast( const char* src, size_t sz )
{
    assert( strlen( src ) == sz );
    auto dst = (char*)tracy_malloc_fast( sz + 1 );
    memcpy( dst, src, sz );
    dst[sz] = '\0';
    return dst;
}

static inline char* CopyStringFast( const char* src )
{
    const auto sz = strlen( src );
    auto dst = (char*)tracy_malloc_fast( sz + 1 );
    memcpy( dst, src, sz );
    dst[sz] = '\0';
    return dst;
}



#if TRACY_HAS_CALLSTACK == 1

enum { MaxCbTrace = 16 };
enum { MaxNameSize = 8*1024 };

int cb_num;
CallstackEntry cb_data[MaxCbTrace];

extern "C"
{
    typedef unsigned long (__stdcall *t_RtlWalkFrameChain)( void**, unsigned long, unsigned long );
    t_RtlWalkFrameChain RtlWalkFrameChain = 0;
}

#if defined __MINGW32__ && API_VERSION_NUMBER < 12
extern "C" {
// Actual required API_VERSION_NUMBER is unknown because it is undocumented. These functions are not present in at least v11.
DWORD IMAGEAPI SymAddrIncludeInlineTrace(HANDLE hProcess, DWORD64 Address);
BOOL IMAGEAPI SymQueryInlineTrace(HANDLE hProcess, DWORD64 StartAddress, DWORD StartContext, DWORD64 StartRetAddress,
    DWORD64 CurAddress, LPDWORD CurContext, LPDWORD CurFrameIndex);
BOOL IMAGEAPI SymFromInlineContext(HANDLE hProcess, DWORD64 Address, ULONG InlineContext, PDWORD64 Displacement,
    PSYMBOL_INFO Symbol);
BOOL IMAGEAPI SymGetLineFromInlineContext(HANDLE hProcess, DWORD64 qwAddr, ULONG InlineContext,
    DWORD64 qwModuleBaseAddress, PDWORD pdwDisplacement, PIMAGEHLP_LINE64 Line64);
};
#endif

#ifndef __CYGWIN__
struct ModuleCache
{
    uint64_t start;
    uint64_t end;
    char* name;
};

static FastVector<ModuleCache>* s_modCache;
#endif

void InitCallstack()
{
    RtlWalkFrameChain = (t_RtlWalkFrameChain)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "RtlWalkFrameChain" );

#ifdef TRACY_DBGHELP_LOCK
    DBGHELP_INIT;
    DBGHELP_LOCK;
#endif

    SymInitialize( GetCurrentProcess(), nullptr, true );
    SymSetOptions( SYMOPT_LOAD_LINES );

#ifndef __CYGWIN__
    HMODULE mod[1024];
    DWORD needed;
    HANDLE proc = GetCurrentProcess();

    s_modCache = (FastVector<ModuleCache>*)tracy_malloc( sizeof( FastVector<ModuleCache> ) );
    new(s_modCache) FastVector<ModuleCache>( 512 );

    if( EnumProcessModules( proc, mod, sizeof( mod ), &needed ) != 0 )
    {
        const auto sz = needed / sizeof( HMODULE );
        for( size_t i=0; i<sz; i++ )
        {
            MODULEINFO info;
            if( GetModuleInformation( proc, mod[i], &info, sizeof( info ) ) != 0 )
            {
                const auto base = uint64_t( info.lpBaseOfDll );
                char name[1024];
                const auto res = GetModuleFileNameA( mod[i], name, 1021 );
                if( res > 0 )
                {
                    auto ptr = name + res;
                    while( ptr > name && *ptr != '\\' && *ptr != '/' ) ptr--;
                    if( ptr > name ) ptr++;
                    const auto namelen = name + res - ptr;
                    auto cache = s_modCache->push_next();
                    cache->start = base;
                    cache->end = base + info.SizeOfImage;
                    cache->name = (char*)tracy_malloc_fast( namelen+3 );
                    cache->name[0] = '[';
                    memcpy( cache->name+1, ptr, namelen );
                    cache->name[namelen+1] = ']';
                    cache->name[namelen+2] = '\0';
                }
            }
        }
    }
#endif

#ifdef TRACY_DBGHELP_LOCK
    DBGHELP_UNLOCK;
#endif
}

TRACY_API uintptr_t* CallTrace( int depth )
{
    auto trace = (uintptr_t*)tracy_malloc( ( 1 + depth ) * sizeof( uintptr_t ) );
    const auto num = RtlWalkFrameChain( (void**)( trace + 1 ), depth, 0 );
    *trace = num;
    return trace;
}

const char* DecodeCallstackPtrFast( uint64_t ptr )
{
    static char ret[MaxNameSize];
    const auto proc = GetCurrentProcess();

    char buf[sizeof( SYMBOL_INFO ) + MaxNameSize];
    auto si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof( SYMBOL_INFO );
    si->MaxNameLen = MaxNameSize;

#ifdef TRACY_DBGHELP_LOCK
    DBGHELP_LOCK;
#endif
    if( SymFromAddr( proc, ptr, nullptr, si ) == 0 )
    {
        *ret = '\0';
    }
    else
    {
        memcpy( ret, si->Name, si->NameLen );
        ret[si->NameLen] = '\0';
    }
#ifdef TRACY_DBGHELP_LOCK
    DBGHELP_UNLOCK;
#endif
    return ret;
}

static const char* GetModuleName( uint64_t addr )
{
    if( ( addr & 0x8000000000000000 ) != 0 ) return "[kernel]";

#ifndef __CYGWIN__
    for( auto& v : *s_modCache )
    {
        if( addr >= v.start && addr < v.end )
        {
            return v.name;
        }
    }

    HMODULE mod[1024];
    DWORD needed;
    HANDLE proc = GetCurrentProcess();

    InitRpmalloc();
    if( EnumProcessModules( proc, mod, sizeof( mod ), &needed ) != 0 )
    {
        const auto sz = needed / sizeof( HMODULE );
        for( size_t i=0; i<sz; i++ )
        {
            MODULEINFO info;
            if( GetModuleInformation( proc, mod[i], &info, sizeof( info ) ) != 0 )
            {
                const auto base = uint64_t( info.lpBaseOfDll );
                if( addr >= base && addr < base + info.SizeOfImage )
                {
                    char name[1024];
                    const auto res = GetModuleFileNameA( mod[i], name, 1021 );
                    if( res > 0 )
                    {
                        auto ptr = name + res;
                        while( ptr > name && *ptr != '\\' && *ptr != '/' ) ptr--;
                        if( ptr > name ) ptr++;
                        const auto namelen = name + res - ptr;
                        auto cache = s_modCache->push_next();
                        cache->start = base;
                        cache->end = base + info.SizeOfImage;
                        cache->name = (char*)tracy_malloc_fast( namelen+3 );
                        cache->name[0] = '[';
                        memcpy( cache->name+1, ptr, namelen );
                        cache->name[namelen+1] = ']';
                        cache->name[namelen+2] = '\0';
                        return cache->name;
                    }
                }
            }
        }
    }
#endif

    return "[unknown]";
}

CallstackSymbolData DecodeSymbolAddress( uint64_t ptr )
{
    CallstackSymbolData sym;
    IMAGEHLP_LINE64 line;
    DWORD displacement = 0;
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
#ifdef TRACY_DBGHELP_LOCK
    DBGHELP_LOCK;
#endif
    const auto res = SymGetLineFromAddr64( GetCurrentProcess(), ptr, &displacement, &line );
#ifdef TRACY_DBGHELP_LOCK
    DBGHELP_UNLOCK;
#endif
    if( res == 0 )
    {
        sym.file = "[unknown]";
        sym.line = 0;
    }
    else
    {
        sym.file = line.FileName;
        sym.line = line.LineNumber;
    }
    sym.needFree = false;
    return sym;
}

CallstackSymbolData DecodeCodeAddress( uint64_t ptr )
{
    CallstackSymbolData sym;
    const auto proc = GetCurrentProcess();
    bool done = false;

    IMAGEHLP_LINE64 line;
    DWORD displacement = 0;
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

#ifdef TRACY_DBGHELP_LOCK
    DBGHELP_LOCK;
#endif
#if !defined __CYGWIN__ && !defined TRACY_NO_CALLSTACK_INLINES
    DWORD inlineNum = SymAddrIncludeInlineTrace( proc, ptr );
    DWORD ctx = 0;
    DWORD idx;
    BOOL doInline = FALSE;
    if( inlineNum != 0 ) doInline = SymQueryInlineTrace( proc, ptr, 0, ptr, ptr, &ctx, &idx );
    if( doInline )
    {
        if( SymGetLineFromInlineContext( proc, ptr, ctx, 0, &displacement, &line ) != 0 )
        {
            sym.file = line.FileName;
            sym.line = line.LineNumber;
            done = true;
        }
    }
#endif
    if( !done )
    {
        if( SymGetLineFromAddr64( proc, ptr, &displacement, &line ) == 0 )
        {
            sym.file = "[unknown]";
            sym.line = 0;
        }
        else
        {
            sym.file = line.FileName;
            sym.line = line.LineNumber;
        }
    }
#ifdef TRACY_DBGHELP_LOCK
    DBGHELP_UNLOCK;
#endif
    sym.needFree = false;
    return sym;
}

CallstackEntryData DecodeCallstackPtr( uint64_t ptr )
{
    int write;
    const auto proc = GetCurrentProcess();
    InitRpmalloc();

#ifdef TRACY_DBGHELP_LOCK
    DBGHELP_LOCK;
#endif
#if !defined __CYGWIN__ && !defined TRACY_NO_CALLSTACK_INLINES
    DWORD inlineNum = SymAddrIncludeInlineTrace( proc, ptr );
    if( inlineNum > MaxCbTrace - 1 ) inlineNum = MaxCbTrace - 1;
    DWORD ctx = 0;
    DWORD idx;
    BOOL doInline = FALSE;
    if( inlineNum != 0 ) doInline = SymQueryInlineTrace( proc, ptr, 0, ptr, ptr, &ctx, &idx );
    if( doInline )
    {
        write = inlineNum;
        cb_num = 1 + inlineNum;
    }
    else
#endif
    {
        write = 0;
        cb_num = 1;
    }

    char buf[sizeof( SYMBOL_INFO ) + MaxNameSize];
    auto si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof( SYMBOL_INFO );
    si->MaxNameLen = MaxNameSize;

    const auto moduleName = GetModuleName( ptr );
    const auto symValid = SymFromAddr( proc, ptr, nullptr, si ) != 0;

    IMAGEHLP_LINE64 line;
    DWORD displacement = 0;
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    {
        const char* filename;
        if( SymGetLineFromAddr64( proc, ptr, &displacement, &line ) == 0 )
        {
            filename = "[unknown]";
            cb_data[write].line = 0;
        }
        else
        {
            filename = line.FileName;
            cb_data[write].line = line.LineNumber;
        }

        cb_data[write].name = symValid ? CopyStringFast( si->Name, si->NameLen ) : CopyStringFast( moduleName );
        cb_data[write].file = CopyStringFast( filename );
        if( symValid )
        {
            cb_data[write].symLen = si->Size;
            cb_data[write].symAddr = si->Address;
        }
        else
        {
            cb_data[write].symLen = 0;
            cb_data[write].symAddr = 0;
        }
    }

#if !defined __CYGWIN__ && !defined TRACY_NO_CALLSTACK_INLINES
    if( doInline )
    {
        for( DWORD i=0; i<inlineNum; i++ )
        {
            auto& cb = cb_data[i];
            const auto symInlineValid = SymFromInlineContext( proc, ptr, ctx, nullptr, si ) != 0;
            const char* filename;
            if( SymGetLineFromInlineContext( proc, ptr, ctx, 0, &displacement, &line ) == 0 )
            {
                filename = "[unknown]";
                cb.line = 0;
            }
            else
            {
                filename = line.FileName;
                cb.line = line.LineNumber;
            }

            cb.name = symInlineValid ? CopyStringFast( si->Name, si->NameLen ) : CopyStringFast( moduleName );
            cb.file = CopyStringFast( filename );
            if( symInlineValid )
            {
                cb.symLen = si->Size;
                cb.symAddr = si->Address;
            }
            else
            {
                cb.symLen = 0;
                cb.symAddr = 0;
            }

            ctx++;
        }
    }
#endif
#ifdef TRACY_DBGHELP_LOCK
    DBGHELP_UNLOCK;
#endif

    return { cb_data, uint8_t( cb_num ), moduleName };
}

#elif TRACY_HAS_CALLSTACK == 2 || TRACY_HAS_CALLSTACK == 3 || TRACY_HAS_CALLSTACK == 4 || TRACY_HAS_CALLSTACK == 6

enum { MaxCbTrace = 16 };

struct backtrace_state* cb_bts;
int cb_num;
CallstackEntry cb_data[MaxCbTrace];
int cb_fixup;

#ifdef __linux
struct KernelSymbol
{
    uint64_t addr;
    const char* name;
    const char* mod;
};

KernelSymbol* s_kernelSym = nullptr;
size_t s_kernelSymCnt;

static void InitKernelSymbols()
{
    FILE* f = fopen( "/proc/kallsyms", "rb" );
    if( !f ) return;
    tracy::FastVector<KernelSymbol> tmpSym( 1024 );
    size_t linelen = 16 * 1024;     // linelen must be big enough to prevent reallocs in getline()
    auto linebuf = (char*)tracy_malloc( linelen );
    ssize_t sz;
    while( ( sz = getline( &linebuf, &linelen, f ) ) != -1 )
    {
        auto ptr = linebuf;
        uint64_t addr = 0;
        while( *ptr != ' ' )
        {
            auto v = *ptr;
            if( v >= '0' && v <= '9' )
            {
                v -= '0';
            }
            else if( v >= 'a' && v <= 'f' )
            {
                v -= 'a';
                v += 10;
            }
            else if( v >= 'A' && v <= 'F' )
            {
                v -= 'A';
                v += 10;
            }
            else
            {
                assert( false );
            }
            assert( ( v & ~0xF ) == 0 );
            addr <<= 4;
            addr |= v;
            ptr++;
        }
        if( addr == 0 ) continue;
        ptr++;
        if( *ptr != 'T' && *ptr != 't' ) continue;
        ptr += 2;
        const auto namestart = ptr;
        while( *ptr != '\t' && *ptr != '\n' ) ptr++;
        const auto nameend = ptr;
        const char* modstart = nullptr;
        const char* modend;
        if( *ptr == '\t' )
        {
            ptr += 2;
            modstart = ptr;
            while( *ptr != ']' ) ptr++;
            modend = ptr;
        }

        auto strname = (char*)tracy_malloc_fast( nameend - namestart + 1 );
        memcpy( strname, namestart, nameend - namestart );
        strname[nameend-namestart] = '\0';

        char* strmod = nullptr;
        if( modstart )
        {
            strmod = (char*)tracy_malloc_fast( modend - modstart + 1 );
            memcpy( strmod, modstart, modend - modstart );
            strmod[modend-modstart] = '\0';
        }

        auto sym = tmpSym.push_next();
        sym->addr = addr;
        sym->name = strname;
        sym->mod = strmod;
    }
    tracy_free_fast( linebuf );
    fclose( f );
    if( tmpSym.empty() ) return;

    std::sort( tmpSym.begin(), tmpSym.end(), []( const KernelSymbol& lhs, const KernelSymbol& rhs ) { return lhs.addr > rhs.addr; } );
    s_kernelSymCnt = tmpSym.size();
    s_kernelSym = (KernelSymbol*)tracy_malloc_fast( sizeof( KernelSymbol ) * s_kernelSymCnt );
    memcpy( s_kernelSym, tmpSym.data(), sizeof( KernelSymbol ) * s_kernelSymCnt );
    TracyDebug( "Loaded %zu kernel symbols\n", s_kernelSymCnt );
}
#endif

void InitCallstack()
{
    cb_bts = backtrace_create_state( nullptr, 0, nullptr, nullptr );

#ifdef __linux
    InitKernelSymbols();
#endif
}

static int FastCallstackDataCb( void* data, uintptr_t pc, uintptr_t lowaddr, const char* fn, int lineno, const char* function )
{
    if( function )
    {
        strcpy( (char*)data, function );
    }
    else
    {
        const char* symname = nullptr;
        auto vptr = (void*)pc;
        Dl_info dlinfo;
        if( dladdr( vptr, &dlinfo ) )
        {
            symname = dlinfo.dli_sname;
        }
        if( symname )
        {
            strcpy( (char*)data, symname );
        }
        else
        {
            *(char*)data = '\0';
        }
    }
    return 1;
}

static void FastCallstackErrorCb( void* data, const char* /*msg*/, int /*errnum*/ )
{
    *(char*)data = '\0';
}

const char* DecodeCallstackPtrFast( uint64_t ptr )
{
    static char ret[1024];
    backtrace_pcinfo( cb_bts, ptr, FastCallstackDataCb, FastCallstackErrorCb, ret );
    return ret;
}

static int SymbolAddressDataCb( void* data, uintptr_t pc, uintptr_t lowaddr, const char* fn, int lineno, const char* function )
{
    auto& sym = *(CallstackSymbolData*)data;
    if( !fn )
    {
        sym.file = "[unknown]";
        sym.line = 0;
        sym.needFree = false;
    }
    else
    {
        sym.file = CopyString( fn );
        sym.line = lineno;
        sym.needFree = true;
    }

    return 1;
}

static void SymbolAddressErrorCb( void* data, const char* /*msg*/, int /*errnum*/ )
{
    auto& sym = *(CallstackSymbolData*)data;
    sym.file = "[unknown]";
    sym.line = 0;
    sym.needFree = false;
}

CallstackSymbolData DecodeSymbolAddress( uint64_t ptr )
{
    CallstackSymbolData sym;
    backtrace_pcinfo( cb_bts, ptr, SymbolAddressDataCb, SymbolAddressErrorCb, &sym );
    return sym;
}

static int CodeDataCb( void* data, uintptr_t pc, uintptr_t lowaddr, const char* fn, int lineno, const char* function )
{
    if( !fn ) return 1;

    const auto fnsz = strlen( fn );
    if( fnsz >= s_tracySkipSubframesMinLen )
    {
        auto ptr = s_tracySkipSubframes;
        do
        {
            if( fnsz >= ptr->len && memcmp( fn + fnsz - ptr->len, ptr->str, ptr->len ) == 0 ) return 0;
            ptr++;
        }
        while( ptr->str );
    }

    auto& sym = *(CallstackSymbolData*)data;
    sym.file = CopyString( fn );
    sym.line = lineno;
    sym.needFree = true;
    return 1;
}

static void CodeErrorCb( void* /*data*/, const char* /*msg*/, int /*errnum*/ )
{
}

CallstackSymbolData DecodeCodeAddress( uint64_t ptr )
{
    CallstackSymbolData sym = { "[unknown]", 0, false };
    backtrace_pcinfo( cb_bts, ptr, CodeDataCb, CodeErrorCb, &sym );
    return sym;
}

static int CallstackDataCb( void* /*data*/, uintptr_t pc, uintptr_t lowaddr, const char* fn, int lineno, const char* function )
{
    enum { DemangleBufLen = 64*1024 };
    char demangled[DemangleBufLen];

    cb_data[cb_num].symLen = 0;
    cb_data[cb_num].symAddr = (uint64_t)lowaddr;

    if( !fn && !function )
    {
        const char* symname = nullptr;
        auto vptr = (void*)pc;
        ptrdiff_t symoff = 0;

        Dl_info dlinfo;
        if( dladdr( vptr, &dlinfo ) )
        {
            symname = dlinfo.dli_sname;
            symoff = (char*)pc - (char*)dlinfo.dli_saddr;

            if( symname && symname[0] == '_' )
            {
                size_t len = DemangleBufLen;
                int status;
                abi::__cxa_demangle( symname, demangled, &len, &status );
                if( status == 0 )
                {
                    symname = demangled;
                }
            }
        }

        if( !symname ) symname = "[unknown]";

        if( symoff == 0 )
        {
            cb_data[cb_num].name = CopyStringFast( symname );
        }
        else
        {
            char buf[32];
            const auto offlen = sprintf( buf, " + %td", symoff );
            const auto namelen = strlen( symname );
            auto name = (char*)tracy_malloc_fast( namelen + offlen + 1 );
            memcpy( name, symname, namelen );
            memcpy( name + namelen, buf, offlen );
            name[namelen + offlen] = '\0';
            cb_data[cb_num].name = name;
        }

        cb_data[cb_num].file = CopyStringFast( "[unknown]" );
        cb_data[cb_num].line = 0;
    }
    else
    {
        if( !fn ) fn = "[unknown]";
        if( !function )
        {
            function = "[unknown]";
        }
        else
        {
            if( function[0] == '_' )
            {
                size_t len = DemangleBufLen;
                int status;
                abi::__cxa_demangle( function, demangled, &len, &status );
                if( status == 0 )
                {
                    function = demangled;
                }
            }
        }

        cb_data[cb_num].name = CopyStringFast( function );
        cb_data[cb_num].file = CopyStringFast( fn );
        cb_data[cb_num].line = lineno;
    }

    if( ++cb_num >= MaxCbTrace )
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

static void CallstackErrorCb( void* /*data*/, const char* /*msg*/, int /*errnum*/ )
{
    for( int i=0; i<cb_num; i++ )
    {
        tracy_free_fast( (void*)cb_data[i].name );
        tracy_free_fast( (void*)cb_data[i].file );
    }

    cb_data[0].name = CopyStringFast( "[error]" );
    cb_data[0].file = CopyStringFast( "[error]" );
    cb_data[0].line = 0;

    cb_num = 1;
}

void SymInfoCallback( void* /*data*/, uintptr_t pc, const char* symname, uintptr_t symval, uintptr_t symsize )
{
    cb_data[cb_num-1].symLen = (uint32_t)symsize;
    cb_data[cb_num-1].symAddr = (uint64_t)symval;
}

void SymInfoError( void* /*data*/, const char* /*msg*/, int /*errnum*/ )
{
    cb_data[cb_num-1].symLen = 0;
    cb_data[cb_num-1].symAddr = 0;
}

CallstackEntryData DecodeCallstackPtr( uint64_t ptr )
{
    InitRpmalloc();
    if( ptr >> 63 == 0 )
    {
        cb_num = 0;
        backtrace_pcinfo( cb_bts, ptr, CallstackDataCb, CallstackErrorCb, nullptr );
        assert( cb_num > 0 );

        backtrace_syminfo( cb_bts, ptr, SymInfoCallback, SymInfoError, nullptr );

        const char* symloc = nullptr;
        Dl_info dlinfo;
        if( dladdr( (void*)ptr, &dlinfo ) ) symloc = dlinfo.dli_fname;

        return { cb_data, uint8_t( cb_num ), symloc ? symloc : "[unknown]" };
    }
    else if( s_kernelSym )
    {
        auto it = std::lower_bound( s_kernelSym, s_kernelSym + s_kernelSymCnt, ptr, []( const KernelSymbol& lhs, const uint64_t& rhs ) { return lhs.addr > rhs; } );
        if( it != s_kernelSym + s_kernelSymCnt )
        {
            cb_data[0].name = CopyStringFast( it->name );
            cb_data[0].file = CopyStringFast( "<kernel>" );
            cb_data[0].line = 0;
            cb_data[0].symLen = 0;
            cb_data[0].symAddr = it->addr;
            return { cb_data, 1, it->mod ? it->mod : "<kernel>" };
        }
    }

    cb_data[0].name = CopyStringFast( "[unknown]" );
    cb_data[0].file = CopyStringFast( "<kernel>" );
    cb_data[0].line = 0;
    cb_data[0].symLen = 0;
    cb_data[0].symAddr = 0;
    return { cb_data, 1, "<kernel>" };
}

#elif TRACY_HAS_CALLSTACK == 5

void InitCallstack()
{
}

const char* DecodeCallstackPtrFast( uint64_t ptr )
{
    static char ret[1024];
    auto vptr = (void*)ptr;
    const char* symname = nullptr;
    Dl_info dlinfo;
    if( dladdr( vptr, &dlinfo ) && dlinfo.dli_sname )
    {
        symname = dlinfo.dli_sname;
    }
    if( symname )
    {
        strcpy( ret, symname );
    }
    else
    {
        *ret = '\0';
    }
    return ret;
}

CallstackSymbolData DecodeSymbolAddress( uint64_t ptr )
{
    const char* symloc = nullptr;
    Dl_info dlinfo;
    if( dladdr( (void*)ptr, &dlinfo ) ) symloc = dlinfo.dli_fname;
    if( !symloc ) symloc = "[unknown]";
    return CallstackSymbolData { symloc, 0, false };
}

CallstackSymbolData DecodeCodeAddress( uint64_t ptr )
{
    return DecodeSymbolAddress( ptr );
}

CallstackEntryData DecodeCallstackPtr( uint64_t ptr )
{
    static CallstackEntry cb;
    cb.line = 0;

    char* demangled = nullptr;
    const char* symname = nullptr;
    const char* symloc = nullptr;
    auto vptr = (void*)ptr;
    ptrdiff_t symoff = 0;
    void* symaddr = nullptr;

    Dl_info dlinfo;
    if( dladdr( vptr, &dlinfo ) )
    {
        symloc = dlinfo.dli_fname;
        symname = dlinfo.dli_sname;
        symoff = (char*)ptr - (char*)dlinfo.dli_saddr;
        symaddr = dlinfo.dli_saddr;

        if( symname && symname[0] == '_' )
        {
            size_t len = 0;
            int status;
            demangled = abi::__cxa_demangle( symname, nullptr, &len, &status );
            if( status == 0 )
            {
                symname = demangled;
            }
        }
    }

    if( !symname ) symname = "[unknown]";
    if( !symloc ) symloc = "[unknown]";

    if( symoff == 0 )
    {
        cb.name = CopyString( symname );
    }
    else
    {
        char buf[32];
        const auto offlen = sprintf( buf, " + %td", symoff );
        const auto namelen = strlen( symname );
        auto name = (char*)tracy_malloc( namelen + offlen + 1 );
        memcpy( name, symname, namelen );
        memcpy( name + namelen, buf, offlen );
        name[namelen + offlen] = '\0';
        cb.name = name;
    }

    cb.file = CopyString( "[unknown]" );
    cb.symLen = 0;
    cb.symAddr = (uint64_t)symaddr;

    if( demangled ) free( demangled );

    return { &cb, 1, symloc };
}

#endif

}

#endif
