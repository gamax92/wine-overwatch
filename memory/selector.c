/*
 * Selector manipulation functions
 *
 * Copyright 1995 Alexandre Julliard
 */

#include <string.h>
#include "winerror.h"
#include "wine/winbase16.h"
#include "ldt.h"
#include "miscemu.h"
#include "selectors.h"
#include "stackframe.h"
#include "process.h"
#include "server.h"
#include "debugtools.h"
#include "toolhelp.h"

DEFAULT_DEBUG_CHANNEL(selector);

#define LDT_SIZE 8192

/* get the number of selectors needed to cover up to the selector limit */
inline static WORD get_sel_count( WORD sel )
{
    return (wine_ldt_copy.limit[sel >> __AHSHIFT] >> 16) + 1;
}

/***********************************************************************
 *           SELECTOR_AllocArray
 *
 * Allocate a selector array without setting the LDT entries
 */
static WORD SELECTOR_AllocArray( WORD count )
{
    WORD i, sel, size = 0;

    if (!count) return 0;
    for (i = FIRST_LDT_ENTRY_TO_ALLOC; i < LDT_SIZE; i++)
    {
        if (wine_ldt_copy.flags[i] & WINE_LDT_FLAGS_ALLOCATED) size = 0;
        else if (++size >= count) break;
    }
    if (i == LDT_SIZE) return 0;
    sel = i - size + 1;

    /* mark selectors as allocated */
    for (i = 0; i < count; i++) wine_ldt_copy.flags[sel + i] |= WINE_LDT_FLAGS_ALLOCATED;

    return (sel << __AHSHIFT) | 7;
}


/***********************************************************************
 *           AllocSelectorArray   (KERNEL.206)
 */
WORD WINAPI AllocSelectorArray16( WORD count )
{
    WORD i, sel = SELECTOR_AllocArray( count );

    if (sel)
    {
        LDT_ENTRY entry;
        wine_ldt_set_base( &entry, 0 );
        wine_ldt_set_limit( &entry, 1 ); /* avoid 0 base and limit */
        wine_ldt_set_flags( &entry, WINE_LDT_FLAGS_DATA );
        for (i = 0; i < count; i++) wine_ldt_set_entry( sel + (i << __AHSHIFT), &entry );
    }
    return sel;
}


/***********************************************************************
 *           AllocSelector   (KERNEL.175)
 */
WORD WINAPI AllocSelector16( WORD sel )
{
    WORD newsel, count, i;

    count = sel ? get_sel_count(sel) : 1;
    newsel = SELECTOR_AllocArray( count );
    TRACE("(%04x): returning %04x\n", sel, newsel );
    if (!newsel) return 0;
    if (!sel) return newsel;  /* nothing to copy */
    for (i = 0; i < count; i++)
    {
        LDT_ENTRY entry;
        wine_ldt_get_entry( sel + (i << __AHSHIFT), &entry );
        wine_ldt_set_entry( newsel + (i << __AHSHIFT), &entry );
    }
    return newsel;
}


/***********************************************************************
 *           FreeSelector   (KERNEL.176)
 */
WORD WINAPI FreeSelector16( WORD sel )
{
    LDT_ENTRY entry;

    if (IS_SELECTOR_FREE(sel)) return sel;  /* error */

#ifdef __i386__
    /* Check if we are freeing current %fs or %gs selector */
    if (!((__get_fs() ^ sel) & ~7))
    {
        WARN("Freeing %%fs selector (%04x), not good.\n", __get_fs() );
        __set_fs( 0 );
    }
    if (!((__get_gs() ^ sel) & ~7)) __set_gs( 0 );
#endif  /* __i386__ */

    memset( &entry, 0, sizeof(entry) );  /* clear the LDT entries */
    wine_ldt_set_entry( sel, &entry );
    wine_ldt_copy.flags[sel >> __AHSHIFT] &= ~WINE_LDT_FLAGS_ALLOCATED;
    return 0;
}


/***********************************************************************
 *           SELECTOR_SetEntries
 *
 * Set the LDT entries for an array of selectors.
 */
static void SELECTOR_SetEntries( WORD sel, const void *base, DWORD size, unsigned char flags )
{
    LDT_ENTRY entry;
    WORD i, count;

    wine_ldt_set_base( &entry, base );
    wine_ldt_set_limit( &entry, size - 1 );
    wine_ldt_set_flags( &entry, flags );
    /* Make sure base and limit are not 0 together if the size is not 0 */
    if (!base && size == 1) wine_ldt_set_limit( &entry, 1 );
    count = (size + 0xffff) / 0x10000;
    for (i = 0; i < count; i++)
    {
        wine_ldt_set_entry( sel + (i << __AHSHIFT), &entry );
        wine_ldt_set_base( &entry, wine_ldt_get_base(&entry) + 0x10000 );
        wine_ldt_set_limit( &entry, wine_ldt_get_limit(&entry) - 0x10000 );
    }
}


/***********************************************************************
 *           SELECTOR_AllocBlock
 *
 * Allocate selectors for a block of linear memory.
 */
WORD SELECTOR_AllocBlock( const void *base, DWORD size, unsigned char flags )
{
    WORD sel, count;

    if (!size) return 0;
    count = (size + 0xffff) / 0x10000;
    sel = SELECTOR_AllocArray( count );
    if (sel) SELECTOR_SetEntries( sel, base, size, flags );
    return sel;
}


/***********************************************************************
 *           SELECTOR_FreeBlock
 *
 * Free a block of selectors.
 */
void SELECTOR_FreeBlock( WORD sel )
{
    WORD i, count = get_sel_count( sel );

    TRACE("(%04x,%d)\n", sel, count );
    for (i = 0; i < count; i++) FreeSelector16( sel + (i << __AHSHIFT) );
}


/***********************************************************************
 *           SELECTOR_ReallocBlock
 *
 * Change the size of a block of selectors.
 */
WORD SELECTOR_ReallocBlock( WORD sel, const void *base, DWORD size )
{
    LDT_ENTRY entry;
    WORD i, oldcount, newcount;

    if (!size) size = 1;
    oldcount = get_sel_count( sel );
    newcount = (size + 0xffff) >> 16;
    wine_ldt_get_entry( sel, &entry );

    if (oldcount < newcount)  /* We need to add selectors */
    {
        WORD index = sel >> __AHSHIFT;
          /* Check if the next selectors are free */
        if (index + newcount > LDT_SIZE) i = oldcount;
        else
            for (i = oldcount; i < newcount; i++)
                if (wine_ldt_copy.flags[index+i] & WINE_LDT_FLAGS_ALLOCATED) break;

        if (i < newcount)  /* they are not free */
        {
            SELECTOR_FreeBlock( sel );
            sel = SELECTOR_AllocArray( newcount );
        }
        else  /* mark the selectors as allocated */
        {
            for (i = oldcount; i < newcount; i++)
                wine_ldt_copy.flags[index+i] |= WINE_LDT_FLAGS_ALLOCATED;
        }
    }
    else if (oldcount > newcount) /* We need to remove selectors */
    {
        SELECTOR_FreeBlock( sel + (newcount << __AHSHIFT) );
    }
    if (sel) SELECTOR_SetEntries( sel, base, size, wine_ldt_get_flags(&entry) );
    return sel;
}


/***********************************************************************
 *           PrestoChangoSelector   (KERNEL.177)
 */
WORD WINAPI PrestoChangoSelector16( WORD selSrc, WORD selDst )
{
    LDT_ENTRY entry;
    wine_ldt_get_entry( selSrc, &entry );
    /* toggle the executable bit */
    entry.HighWord.Bits.Type ^= (WINE_LDT_FLAGS_CODE ^ WINE_LDT_FLAGS_DATA);
    wine_ldt_set_entry( selDst, &entry );
    return selDst;
}


/***********************************************************************
 *           AllocCStoDSAlias   (KERNEL.170)
 */
WORD WINAPI AllocCStoDSAlias16( WORD sel )
{
    WORD newsel;
    LDT_ENTRY entry;

    newsel = SELECTOR_AllocArray( 1 );
    TRACE("(%04x): returning %04x\n",
                      sel, newsel );
    if (!newsel) return 0;
    wine_ldt_get_entry( sel, &entry );
    entry.HighWord.Bits.Type = WINE_LDT_FLAGS_DATA;
    wine_ldt_set_entry( newsel, &entry );
    return newsel;
}


/***********************************************************************
 *           AllocDStoCSAlias   (KERNEL.171)
 */
WORD WINAPI AllocDStoCSAlias16( WORD sel )
{
    WORD newsel;
    LDT_ENTRY entry;

    newsel = SELECTOR_AllocArray( 1 );
    TRACE("(%04x): returning %04x\n",
                      sel, newsel );
    if (!newsel) return 0;
    wine_ldt_get_entry( sel, &entry );
    entry.HighWord.Bits.Type = WINE_LDT_FLAGS_CODE;
    wine_ldt_set_entry( newsel, &entry );
    return newsel;
}


/***********************************************************************
 *           LongPtrAdd   (KERNEL.180)
 */
void WINAPI LongPtrAdd16( DWORD ptr, DWORD add )
{
    LDT_ENTRY entry;
    wine_ldt_get_entry( SELECTOROF(ptr), &entry );
    wine_ldt_set_base( &entry, (char *)wine_ldt_get_base(&entry) + add );
    wine_ldt_set_entry( SELECTOROF(ptr), &entry );
}


/***********************************************************************
 *           GetSelectorBase   (KERNEL.186)
 */
DWORD WINAPI WIN16_GetSelectorBase( WORD sel )
{
    /*
     * Note: For Win32s processes, the whole linear address space is
     *       shifted by 0x10000 relative to the OS linear address space.
     *       See the comment in msdos/vxd.c.
     */

    DWORD base = GetSelectorBase( sel );
    return W32S_WINE2APP( base, W32S_APPLICATION() ? W32S_OFFSET : 0 );
}
DWORD WINAPI GetSelectorBase( WORD sel )
{
    void *base = wine_ldt_copy.base[sel >> __AHSHIFT];

    /* if base points into DOSMEM, assume we have to
     * return pointer into physical lower 1MB */

    return DOSMEM_MapLinearToDos( base );
}


/***********************************************************************
 *           SetSelectorBase   (KERNEL.187)
 */
DWORD WINAPI WIN16_SetSelectorBase( WORD sel, DWORD base )
{
    /*
     * Note: For Win32s processes, the whole linear address space is
     *       shifted by 0x10000 relative to the OS linear address space.
     *       See the comment in msdos/vxd.c.
     */

    SetSelectorBase( sel,
	W32S_APP2WINE( base, W32S_APPLICATION() ? W32S_OFFSET : 0 ) );
    return sel;
}
WORD WINAPI SetSelectorBase( WORD sel, DWORD base )
{
    LDT_ENTRY entry;
    wine_ldt_get_entry( sel, &entry );
    wine_ldt_set_base( &entry, DOSMEM_MapDosToLinear(base) );
    wine_ldt_set_entry( sel, &entry );
    return sel;
}


/***********************************************************************
 *           GetSelectorLimit   (KERNEL.188)
 */
DWORD WINAPI GetSelectorLimit16( WORD sel )
{
    return wine_ldt_copy.limit[sel >> __AHSHIFT];
}


/***********************************************************************
 *           SetSelectorLimit   (KERNEL.189)
 */
WORD WINAPI SetSelectorLimit16( WORD sel, DWORD limit )
{
    LDT_ENTRY entry;
    wine_ldt_get_entry( sel, &entry );
    wine_ldt_set_limit( &entry, limit );
    wine_ldt_set_entry( sel, &entry );
    return sel;
}


/***********************************************************************
 *           SelectorAccessRights   (KERNEL.196)
 */
WORD WINAPI SelectorAccessRights16( WORD sel, WORD op, WORD val )
{
    LDT_ENTRY entry;
    wine_ldt_get_entry( sel, &entry );

    if (op == 0)  /* get */
    {
        return entry.HighWord.Bytes.Flags1 | ((entry.HighWord.Bytes.Flags2 << 8) & 0xf0);
    }
    else  /* set */
    {
        entry.HighWord.Bytes.Flags1 = LOBYTE(val) | 0xf0;
        entry.HighWord.Bytes.Flags2 = (entry.HighWord.Bytes.Flags2 & 0x0f) | (HIBYTE(val) & 0xf0);
        wine_ldt_set_entry( sel, &entry );
        return 0;
    }
}


/***********************************************************************
 *           IsBadCodePtr16   (KERNEL.336)
 */
BOOL16 WINAPI IsBadCodePtr16( SEGPTR lpfn )
{
    WORD sel;
    LDT_ENTRY entry;

    sel = SELECTOROF(lpfn);
    if (!sel) return TRUE;
    if (IS_SELECTOR_FREE(sel)) return TRUE;
    wine_ldt_get_entry( sel, &entry );
    /* check for code segment, ignoring conforming, read-only and accessed bits */
    if ((entry.HighWord.Bits.Type ^ WINE_LDT_FLAGS_CODE) & 0x18) return TRUE;
    if (OFFSETOF(lpfn) > wine_ldt_get_limit(&entry)) return TRUE;
    return FALSE;
}


/***********************************************************************
 *           IsBadStringPtr16   (KERNEL.337)
 */
BOOL16 WINAPI IsBadStringPtr16( SEGPTR ptr, UINT16 size )
{
    WORD sel;
    LDT_ENTRY entry;

    sel = SELECTOROF(ptr);
    if (!sel) return TRUE;
    if (IS_SELECTOR_FREE(sel)) return TRUE;
    wine_ldt_get_entry( sel, &entry );
    /* check for data or readable code segment */
    if (!(entry.HighWord.Bits.Type & 0x10)) return TRUE;  /* system descriptor */
    if ((entry.HighWord.Bits.Type & 0x0a) == 0x08) return TRUE;  /* non-readable code segment */
    if (strlen(PTR_SEG_TO_LIN(ptr)) < size) size = strlen(PTR_SEG_TO_LIN(ptr)) + 1;
    if (size && (OFFSETOF(ptr) + size - 1 > wine_ldt_get_limit(&entry))) return TRUE;
    return FALSE;
}


/***********************************************************************
 *           IsBadHugeReadPtr16   (KERNEL.346)
 */
BOOL16 WINAPI IsBadHugeReadPtr16( SEGPTR ptr, DWORD size )
{
    WORD sel;
    LDT_ENTRY entry;

    sel = SELECTOROF(ptr);
    if (!sel) return TRUE;
    if (IS_SELECTOR_FREE(sel)) return TRUE;
    wine_ldt_get_entry( sel, &entry );
    /* check for data or readable code segment */
    if (!(entry.HighWord.Bits.Type & 0x10)) return TRUE;  /* system descriptor */
    if ((entry.HighWord.Bits.Type & 0x0a) == 0x08) return TRUE;  /* non-readable code segment */
    if (size && (OFFSETOF(ptr) + size - 1 > wine_ldt_get_limit( &entry ))) return TRUE;
    return FALSE;
}


/***********************************************************************
 *           IsBadHugeWritePtr16   (KERNEL.347)
 */
BOOL16 WINAPI IsBadHugeWritePtr16( SEGPTR ptr, DWORD size )
{
    WORD sel;
    LDT_ENTRY entry;

    sel = SELECTOROF(ptr);
    if (!sel) return TRUE;
    if (IS_SELECTOR_FREE(sel)) return TRUE;
    wine_ldt_get_entry( sel, &entry );
    /* check for writeable data segment, ignoring expand-down and accessed flags */
    if ((entry.HighWord.Bits.Type ^ WINE_LDT_FLAGS_DATA) & ~5) return TRUE;
    if (size && (OFFSETOF(ptr) + size - 1 > wine_ldt_get_limit( &entry ))) return TRUE;
    return FALSE;
}

/***********************************************************************
 *           IsBadReadPtr16   (KERNEL.334)
 */
BOOL16 WINAPI IsBadReadPtr16( SEGPTR ptr, UINT16 size )
{
    return IsBadHugeReadPtr16( ptr, size );
}


/***********************************************************************
 *           IsBadWritePtr16   (KERNEL.335)
 */
BOOL16 WINAPI IsBadWritePtr16( SEGPTR ptr, UINT16 size )
{
    return IsBadHugeWritePtr16( ptr, size );
}


/***********************************************************************
 *           IsBadFlatReadWritePtr16   (KERNEL.627)
 */
BOOL16 WINAPI IsBadFlatReadWritePtr16( SEGPTR ptr, DWORD size, BOOL16 bWrite )
{
    return bWrite? IsBadHugeWritePtr16( ptr, size )
                 : IsBadHugeReadPtr16( ptr, size );
}


/***********************************************************************
 *           MemoryRead   (TOOLHELP.78)
 */
DWORD WINAPI MemoryRead16( WORD sel, DWORD offset, void *buffer, DWORD count )
{
    WORD index = sel >> __AHSHIFT;

    if (!(wine_ldt_copy.flags[index] & WINE_LDT_FLAGS_ALLOCATED)) return 0;
    if (offset > wine_ldt_copy.limit[index]) return 0;
    if (offset + count > wine_ldt_copy.limit[index] + 1)
        count = wine_ldt_copy.limit[index] + 1 - offset;
    memcpy( buffer, (char *)wine_ldt_copy.base[index] + offset, count );
    return count;
}


/***********************************************************************
 *           MemoryWrite   (TOOLHELP.79)
 */
DWORD WINAPI MemoryWrite16( WORD sel, DWORD offset, void *buffer, DWORD count )
{
    WORD index = sel >> __AHSHIFT;

    if (!(wine_ldt_copy.flags[index] & WINE_LDT_FLAGS_ALLOCATED)) return 0;
    if (offset > wine_ldt_copy.limit[index]) return 0;
    if (offset + count > wine_ldt_copy.limit[index] + 1)
        count = wine_ldt_copy.limit[index] + 1 - offset;
    memcpy( (char *)wine_ldt_copy.base[index] + offset, buffer, count );
    return count;
}

/************************************* Win95 pointer mapping functions *
 *
 */

/***********************************************************************
 *           MapSL   (KERNEL32.523)
 *
 * Maps fixed segmented pointer to linear.
 */
LPVOID WINAPI MapSL( SEGPTR sptr )
{
    return (char *)wine_ldt_copy.base[SELECTOROF(sptr) >> __AHSHIFT] + OFFSETOF(sptr);
}

/***********************************************************************
 *           MapSLFix   (KERNEL32.524)
 *
 * FIXME: MapSLFix and UnMapSLFixArray should probably prevent
 * unexpected linear address change when GlobalCompact() shuffles
 * moveable blocks.
 */

LPVOID WINAPI MapSLFix( SEGPTR sptr )
{
    return (LPVOID)PTR_SEG_TO_LIN(sptr);
}

/***********************************************************************
 *           UnMapSLFixArray   (KERNEL32.701)
 */

void WINAPI UnMapSLFixArray( SEGPTR sptr[], INT length, CONTEXT86 *context )
{
    /* Must not change EAX, hence defined as 'register' function */
}

/***********************************************************************
 *           MapLS   (KERNEL32.522)
 *
 * Maps linear pointer to segmented.
 */
SEGPTR WINAPI MapLS( LPVOID ptr )
{
    if (!HIWORD(ptr))
        return (SEGPTR)ptr;
    else
    {
        WORD sel = SELECTOR_AllocBlock( ptr, 0x10000, WINE_LDT_FLAGS_DATA );
        return PTR_SEG_OFF_TO_SEGPTR( sel, 0 );
    }
}


/***********************************************************************
 *           UnMapLS   (KERNEL32.700)
 *
 * Free mapped selector.
 */
void WINAPI UnMapLS( SEGPTR sptr )
{
    if (SELECTOROF(sptr)) FreeSelector16( SELECTOROF(sptr) );
}

/***********************************************************************
 *           GetThreadSelectorEntry   (KERNEL32)
 */
BOOL WINAPI GetThreadSelectorEntry( HANDLE hthread, DWORD sel, LPLDT_ENTRY ldtent)
{
#ifdef __i386__
    BOOL ret;

    if (!(sel & 4))  /* GDT selector */
    {
        sel &= ~3;  /* ignore RPL */
        if (!sel)  /* null selector */
        {
            memset( ldtent, 0, sizeof(*ldtent) );
            return TRUE;
        }
        ldtent->BaseLow                   = 0;
        ldtent->HighWord.Bits.BaseMid     = 0;
        ldtent->HighWord.Bits.BaseHi      = 0;
        ldtent->LimitLow                  = 0xffff;
        ldtent->HighWord.Bits.LimitHi     = 0xf;
        ldtent->HighWord.Bits.Dpl         = 3;
        ldtent->HighWord.Bits.Sys         = 0;
        ldtent->HighWord.Bits.Pres        = 1;
        ldtent->HighWord.Bits.Granularity = 1;
        ldtent->HighWord.Bits.Default_Big = 1;
        ldtent->HighWord.Bits.Type        = 0x12;
        /* it has to be one of the system GDT selectors */
        if (sel == (__get_ds() & ~3)) return TRUE;
        if (sel == (__get_ss() & ~3)) return TRUE;
        if (sel == (__get_cs() & ~3))
        {
            ldtent->HighWord.Bits.Type |= 8;  /* code segment */
            return TRUE;
        }
        SetLastError( ERROR_NOACCESS );
        return FALSE;
    }

    SERVER_START_REQ
    {
        struct get_selector_entry_request *req = server_alloc_req( sizeof(*req), 0 );

        req->handle = hthread;
        req->entry = sel >> __AHSHIFT;
        if ((ret = !server_call( REQ_GET_SELECTOR_ENTRY )))
        {
            if (!(req->flags & WINE_LDT_FLAGS_ALLOCATED))
            {
                SetLastError( ERROR_MR_MID_NOT_FOUND );  /* sic */
                ret = FALSE;
            }
            else
            {
                wine_ldt_set_base( ldtent, (void *)req->base );
                wine_ldt_set_limit( ldtent, req->limit );
                wine_ldt_set_flags( ldtent, req->flags );
            }
        }
    }
    SERVER_END_REQ;
    return ret;
#else
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
#endif
}


/**********************************************************************
 * 		SMapLS*		(KERNEL32)
 * These functions map linear pointers at [EBP+xxx] to segmented pointers
 * and return them.
 * Win95 uses some kind of alias structs, which it stores in [EBP+x] to
 * unravel them at SUnMapLS. We just store the segmented pointer there.
 */
static void
x_SMapLS_IP_EBP_x(CONTEXT86 *context,int argoff) {
    DWORD	val,ptr; 

    val =*(DWORD*)(context->Ebp + argoff);
    if (val<0x10000) {
	ptr=val;
        *(DWORD*)(context->Ebp + argoff) = 0;
    } else {
    	ptr = MapLS((LPVOID)val);
        *(DWORD*)(context->Ebp + argoff) = ptr;
    }
    context->Eax = ptr;
}

/***********************************************************************
 *		SMapLS_IP_EBP_8 (KERNEL32.601)
 */
void WINAPI SMapLS_IP_EBP_8 (CONTEXT86 *context) {x_SMapLS_IP_EBP_x(context, 8);}

/***********************************************************************
 *		SMapLS_IP_EBP_12 (KERNEL32.593)
 */
void WINAPI SMapLS_IP_EBP_12(CONTEXT86 *context) {x_SMapLS_IP_EBP_x(context,12);}

/***********************************************************************
 *		SMapLS_IP_EBP_16 (KERNEL32.594)
 */
void WINAPI SMapLS_IP_EBP_16(CONTEXT86 *context) {x_SMapLS_IP_EBP_x(context,16);}

/***********************************************************************
 *		SMapLS_IP_EBP_20 (KERNEL32.595)
 */
void WINAPI SMapLS_IP_EBP_20(CONTEXT86 *context) {x_SMapLS_IP_EBP_x(context,20);}

/***********************************************************************
 *		SMapLS_IP_EBP_24 (KERNEL32.596)
 */
void WINAPI SMapLS_IP_EBP_24(CONTEXT86 *context) {x_SMapLS_IP_EBP_x(context,24);}

/***********************************************************************
 *		SMapLS_IP_EBP_28 (KERNEL32.597)
 */
void WINAPI SMapLS_IP_EBP_28(CONTEXT86 *context) {x_SMapLS_IP_EBP_x(context,28);}

/***********************************************************************
 *		SMapLS_IP_EBP_32 (KERNEL32.598)
 */
void WINAPI SMapLS_IP_EBP_32(CONTEXT86 *context) {x_SMapLS_IP_EBP_x(context,32);}

/***********************************************************************
 *		SMapLS_IP_EBP_36 (KERNEL32.599)
 */
void WINAPI SMapLS_IP_EBP_36(CONTEXT86 *context) {x_SMapLS_IP_EBP_x(context,36);}

/***********************************************************************
 *		SMapLS_IP_EBP_40 (KERNEL32.600)
 */
void WINAPI SMapLS_IP_EBP_40(CONTEXT86 *context) {x_SMapLS_IP_EBP_x(context,40);}

/***********************************************************************
 *		SMapLS (KERNEL32.592)
 */
void WINAPI SMapLS( CONTEXT86 *context )
{
    if (HIWORD(context->Eax))
    {
        context->Eax = MapLS( (LPVOID)context->Eax );
        context->Edx = context->Eax;
    } else {
        context->Edx = 0;
    }
}

/***********************************************************************
 *		SUnMapLS (KERNEL32.602)
 */

void WINAPI SUnMapLS( CONTEXT86 *context )
{
    if (HIWORD(context->Eax)) UnMapLS( (SEGPTR)context->Eax );
}

inline static void x_SUnMapLS_IP_EBP_x(CONTEXT86 *context,int argoff)
{
    SEGPTR *ptr = (SEGPTR *)(context->Ebp + argoff);
    if (*ptr)
    {
        UnMapLS( *ptr );
        *ptr = 0;
    }
}

/***********************************************************************
 *		SUnMapLS_IP_EBP_8 (KERNEL32.611)
 */
void WINAPI SUnMapLS_IP_EBP_8 (CONTEXT86 *context) { x_SUnMapLS_IP_EBP_x(context, 8); }

/***********************************************************************
 *		SUnMapLS_IP_EBP_12 (KERNEL32.603)
 */
void WINAPI SUnMapLS_IP_EBP_12(CONTEXT86 *context) { x_SUnMapLS_IP_EBP_x(context,12); }

/***********************************************************************
 *		SUnMapLS_IP_EBP_16 (KERNEL32.604)
 */
void WINAPI SUnMapLS_IP_EBP_16(CONTEXT86 *context) { x_SUnMapLS_IP_EBP_x(context,16); }

/***********************************************************************
 *		SUnMapLS_IP_EBP_20 (KERNEL32.605)
 */
void WINAPI SUnMapLS_IP_EBP_20(CONTEXT86 *context) { x_SUnMapLS_IP_EBP_x(context,20); }

/***********************************************************************
 *		SUnMapLS_IP_EBP_24 (KERNEL32.606)
 */
void WINAPI SUnMapLS_IP_EBP_24(CONTEXT86 *context) { x_SUnMapLS_IP_EBP_x(context,24); }

/***********************************************************************
 *		SUnMapLS_IP_EBP_28 (KERNEL32.607)
 */
void WINAPI SUnMapLS_IP_EBP_28(CONTEXT86 *context) { x_SUnMapLS_IP_EBP_x(context,28); }

/***********************************************************************
 *		SUnMapLS_IP_EBP_32 (KERNEL32.608)
 */
void WINAPI SUnMapLS_IP_EBP_32(CONTEXT86 *context) { x_SUnMapLS_IP_EBP_x(context,32); }

/***********************************************************************
 *		SUnMapLS_IP_EBP_36 (KERNEL32.609)
 */
void WINAPI SUnMapLS_IP_EBP_36(CONTEXT86 *context) { x_SUnMapLS_IP_EBP_x(context,36); }

/***********************************************************************
 *		SUnMapLS_IP_EBP_40 (KERNEL32.610)
 */
void WINAPI SUnMapLS_IP_EBP_40(CONTEXT86 *context) { x_SUnMapLS_IP_EBP_x(context,40); }

/**********************************************************************
 * 		AllocMappedBuffer	(KERNEL32.38)
 *
 * This is a undocumented KERNEL32 function that 
 * SMapLS's a GlobalAlloc'ed buffer.
 *
 * Input:   EDI register: size of buffer to allocate
 * Output:  EDI register: pointer to buffer
 *
 * Note: The buffer is preceeded by 8 bytes:
 *        ...
 *       edi+0   buffer
 *       edi-4   SEGPTR to buffer
 *       edi-8   some magic Win95 needs for SUnMapLS
 *               (we use it for the memory handle)
 *
 *       The SEGPTR is used by the caller!
 */

void WINAPI AllocMappedBuffer( CONTEXT86 *context )
{
    HGLOBAL handle = GlobalAlloc(0, context->Edi + 8);
    DWORD *buffer = (DWORD *)GlobalLock(handle);
    SEGPTR ptr = 0;

    if (buffer)
        if (!(ptr = MapLS(buffer + 2)))
        {
            GlobalUnlock(handle);
            GlobalFree(handle);
        }

    if (!ptr)
        context->Eax = context->Edi = 0;
    else
    {
        buffer[0] = handle;
        buffer[1] = ptr;

        context->Eax = (DWORD) ptr;
        context->Edi = (DWORD)(buffer + 2);
    }
}

/**********************************************************************
 * 		FreeMappedBuffer	(KERNEL32.39)
 *
 * Free a buffer allocated by AllocMappedBuffer
 *
 * Input: EDI register: pointer to buffer
 */

void WINAPI FreeMappedBuffer( CONTEXT86 *context )
{
    if (context->Edi)
    {
        DWORD *buffer = (DWORD *)context->Edi - 2;

        UnMapLS(buffer[1]);

        GlobalUnlock(buffer[0]);
        GlobalFree(buffer[0]);
    }
}


/***********************************************************************
 *           UTSelectorOffsetToLinear       (WIN32S16.48)
 *
 * rough guesswork, but seems to work (I had no "reasonable" docu)
 */
LPVOID WINAPI UTSelectorOffsetToLinear16(SEGPTR sptr)
{
        return PTR_SEG_TO_LIN(sptr);
}

/***********************************************************************
 *           UTLinearToSelectorOffset       (WIN32S16.49)
 *
 * FIXME: I don't know if that's the right way to do linear -> segmented
 */
SEGPTR WINAPI UTLinearToSelectorOffset16(LPVOID lptr)
{
    return (SEGPTR)lptr;
}

#ifdef __i386__
__ASM_GLOBAL_FUNC( __get_cs, "movw %cs,%ax\n\tret" )
__ASM_GLOBAL_FUNC( __get_ds, "movw %ds,%ax\n\tret" )
__ASM_GLOBAL_FUNC( __get_es, "movw %es,%ax\n\tret" )
__ASM_GLOBAL_FUNC( __get_fs, "movw %fs,%ax\n\tret" )
__ASM_GLOBAL_FUNC( __get_gs, "movw %gs,%ax\n\tret" )
__ASM_GLOBAL_FUNC( __get_ss, "movw %ss,%ax\n\tret" )
__ASM_GLOBAL_FUNC( __set_fs, "movl 4(%esp),%eax\n\tmovw %ax,%fs\n\tret" )
__ASM_GLOBAL_FUNC( __set_gs, "movl 4(%esp),%eax\n\tmovw %ax,%gs\n\tret" )
#endif
