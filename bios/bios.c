/*
 *  bios.c - C portion of BIOS initialization and front end
 *
 * Copyright (c) 2001 Lineo, Inc. and
 *
 * Authors:
 *  SCC     Steve C. Cavender
 *  KTB     Karl T. Braun (kral)
 *  JSL     Jason S. Loveman
 *  EWF     Eric W. Fleischman
 *  LTG     Louis T. Garavaglia
 *  MAD     Martin Doering
 *  LVL     Laurent Vogel
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */



#include "portab.h"
#include "bios.h"
#include "gemerror.h"
#include "config.h"
#include "kprint.h"
#include "tosvars.h"
#include "initinfo.h"


#include "ikbd.h"
#include "midi.h"
#include "mfp.h"
#include "floppy.h"
#include "sound.h"
#include "screen.h"
#include "vectors.h"
#include "asm.h"
#include "chardev.h"

/*==== Defines ============================================================*/

#define DBGBIOS 0               /* If you want debugging output */

/*==== Forward prototypes =================================================*/

void bufl_init(void);
void biosmain(void);


/*==== External declarations ==============================================*/

extern WORD os_dosdate;         /* Time in DOS format */
extern BYTE *biosts ;           /*  time stamp string */


extern LONG trap_1();           /* found in startup.s */
extern LONG drvbits;            /* found in startup.s */
extern MD b_mdx;                /* found in startup.s */


extern void clk_init(void);     /* found in clock.c */

extern LONG oscall();           /* This jumps to BDOS */
extern LONG osinit();

extern void linea_init(void);   /* found in linea.S */
extern void cartscan(WORD);     /* found in startup.S */

extern void emucon(void);       /* found in cli/coma.S - start of CLI */

/*==== Declarations =======================================================*/

/* is_ramtos = 1 if the TOS is running in RAM (detected by looking
 * at os_entry).
 */
int is_ramtos;

/* Drive specific declarations */
static WORD defdrv ;            /* default drive number (0 = a:, 2 = c:) */

BYTE env[256];                  /* environment string, enough bytes??? */


/*==== BOOT ===============================================================*/

/*
 * Taken from startup.s, and rewritten in C.
 * 
 */
 
void startup(void)
{
  WORD i;
  LONG a;

#if DBGBIOS
  kprintf("beginning of BIOS startup\n");
#endif
  
  snd_init();     /* Reset Soundchip, deselect floppies */
  screen_init();  /* detect monitor type, ... */
  
  /* detect if TOS in RAM */
  a = ((LONG) os_entry) & 0xffffff;
  if( a == 0xe00000L || a == 0xfc0000L ) {
    is_ramtos = 0;
  } else {
    is_ramtos = 1;
  }

#if DBGBIOS
  kprintf("_etext = 0x%08lx\n", (LONG)_etext);
  kprintf("_edata = 0x%08lx\n", (LONG)_edata);
  kprintf("end    = 0x%08lx\n", (LONG)end);
#endif
  if(is_ramtos) {
    /* patch TOS header */
    os_end = (LONG) _edata;
  }

  /* initialise some memory variables */
  end_os = os_end;
  membot = end_os;
//  exec_os = os_beg;
  exec_os = &emucon;            // set start of console program
  memtop = (LONG) v_bas_ad;

  m_start = os_end;
  m_length = memtop - m_start;
  themd = (LONG) &b_mdx;
  
  /* setup default exception vectors */
  init_exc_vec();
  init_user_vec();
  
  /* initialise some vectors */
  VEC_HBL = int_hbl;
  VEC_VBL = int_vbl;
  VEC_AES = dummyaes;
  VEC_BIOS = bios;
  VEC_XBIOS = xbios;
  VEC_LINEA = int_linea;
  
  init_acia_vecs();
  
  VEC_DIVNULL = just_rte;
  
  /* floppy related vectors */
  floppy_init();

  /* are these useful ?? */
  prt_stat = print_stat;
  prt_vec = print_vec;
  aux_stat = serial_stat;
  aux_vec = serial_vec;
  dump_vec = dump_scr;
  
  /* misc. variables */
#if DBGBIOS
  kprintf("diskbuf = %08lx\n", (LONG)diskbuf);
#endif
  dumpflg = -1;
  sysbase = (LONG) os_entry;
  
  savptr = (LONG) save_area;
  
  /* some more variables */
  etv_timer = just_rts;
  etv_critic = criter1; 
  etv_term = just_rts;
  
  /* setup VBL queue */
  nvbls = 8;
  vblqueue = vbl_list;
  for(i = 0 ; i < 8 ; i++) {
    vbl_list[i] = 0;
  }

  /* init linea */
  linea_init();

  vblsem = 1;

    /* initialize BIOS components */

    chardev_init();     /* Initialize character devices */
    mfp_init();         /* init MFP, timers, USART */
    kbd_init();         /* init keyboard, disable mouse and joystick */
    midi_init();        /* init MIDI acia so that kbd acia irq works */
    clk_init();         /* init clock (dummy for emulator) */
  
    /* initialize BDOS buffer list */

    bufl_init();

    /* initialize BDOS */

    osinit();
  
    set_sr(0x2300);
  
    VEC_BIOS = bios;
    (*(PFVOID*)0x7C) = brkpt;   /* ??? */
  
#if DBGBIOS
    kprintf("BIOS: Last test point reached ...\n");
#endif
    cartscan(3);

     /* main BIOS */
 
    biosmain();

    for(;;);

}

/*
 * bufl_init - BDOS buffer list initialization
 *
 * LVL - This should really go in BDOS...
 */

static BYTE secbuf[4][512];          /* sector buffers */

static BCB bcbx[4];    /* buffer control block array for each buffer */
extern BCB *bufl[];    /* buffer lists - two lists:  fat,dir / data */

void bufl_init(void)
{
    /* set up sector buffers */

    bcbx[0].b_link = &bcbx[1];
    bcbx[2].b_link = &bcbx[3];

    /* make BCBs invalid */

    bcbx[0].b_bufdrv = -1;
    bcbx[1].b_bufdrv = -1;
    bcbx[2].b_bufdrv = -1;
    bcbx[3].b_bufdrv = -1;

    /* initialize buffer pointers in BCBs */

    bcbx[0].b_bufr = &secbuf[0][0];
    bcbx[1].b_bufr = &secbuf[1][0];
    bcbx[2].b_bufr = &secbuf[2][0];
    bcbx[3].b_bufr = &secbuf[3][0];

    /* initialize the buffer list pointers */
    
    bufl[BI_FAT] = &bcbx[0];                    /* fat buffers */
    bufl[BI_DATA] = &bcbx[2];                   /* dir/data buffers */
}



/*
 * biosmain - c part of the bios init code
 *
 * Print some status messages
 * exec the shell command.prg
 */

void biosmain()
{
    VOID (*newexec_os)(VOID);

    trap_1( 0x30 );              /* initial test, if BDOS works */

    trap_1( 0x2b, os_dosdate);  /* set initial date in GEMDOS format */

    /* if TOS in RAM booted from an autoboot floppy, ask to remove the
     * floppy before going on.
     */
    if(is_ramtos) {
      if(os_magic == OS_MAGIC_EJECT) {
	cprintf("Please eject the floppy and hit RETURN");
	bconin2();
      }
    }

#if DBGBIOS
    kprintf("drvbits = %08lx\n", drvbits);
#endif
    do_hdv_boot();
#if DBGBIOS
    kprintf("drvbits = %08lx, bootdev = %d\n", drvbits, bootdev);
#endif
    defdrv = bootdev;
    trap_1( 0x0e , defdrv );    /* Set boot drive */

    /* execute Reset-resistent PRGs */

    /* show initial config information */
    initinfo();

    /* autoexec Prgs from AUTO folder */
    
    /* clear environment string */
    env[0]='\0';

    /* clear commandline */
    
    /* Jump to the EmuCON - for now not loaded via pexec (hack) */
    trap_1( 0x4b , 0, "COMMAND.PRG" , "", env);

//    newexec_os=(VOID*)trap_1( 0x4b , 5, "" , "", env)+8;
//    newexec_os=exec_os;

    trap_1( 0x4b , 4, "" , "", env);

    cprintf("[FAIL] HALT - should never be reached!\n\r");
    while(1) ;
}




/**
 * bios_0 - (getmpb) Load Memory parameter block
 *
 * Returns values of the initial memory parameter block, which contains the
 * start address and the length of the TPA.
 * Just executed one time, before GEMDOS is loaded.
 *
 * Arguments:
 *   mpb - first memory descriptor, filled from BIOS
 *
 */

void bios_0(MPB *mpb)
{
    mpb->mp_mfl = mpb->mp_rover = &b_mdx; /* free list/rover set to init MD */
    mpb->mp_mal = (MD *)0;                /* allocated list set to NULL */

#if DBGBIOS
    kprint("BIOS: getmpb m_start = ");
    kputp((LONG*) b_mdx.m_start);
    kprint("\n");
    kprint("BIOS: getmpb m_length = ");
    kputp((LONG*) b_mdx.m_length);
    kprint("\n");
#endif
}



/**
 * bios_1 - (bconstat) Status of input device
 *
 * Arguments:
 *   handle - device handle (0:PRT 1:AUX 2:CON)
 *
 *
 * Returns status in D0.L:
 *  -1  device is ready
 *   0  device is not ready
 */

LONG bios_1(WORD handle)        /* GEMBIOS character_input_status */
{
    return bconstat_vec[handle & 7]() ;
}



/**
 * bconin  - Get character from device
 *
 * Arguments:
 *   handle - device handle (0:PRT 1:AUX 2:CON)
 *
 * This function does not return until a character has been
 * input.  It returns the character value in D0.L, with the
 * high word set to zero.  For CON:, it returns the GSX 2.0
 * compatible scan code in the low byte of the high word, &
 * the ASCII character in the lower byte, or zero in the
 * lower byte if the character is non-ASCII.  For AUX:, it
 * returns the character in the low byte.
 */

LONG bios_2(WORD handle)
{
    return bconin_vec[handle & 7]() ;
}



/**
 * bconout  - Print character to output device
 */

void bios_3(WORD handle, BYTE what)
{
    bconout_vec[handle & 7](handle, what);
}



/**
 * rwabs  - Read or write sectors
 *
 * Returns a 2's complement error number in D0.L.  It
 * is the responsibility of the driver to check for
 * media change before any write to FAT sectors.  If
 * media has changed, no write should take place, just
 * return with error code.
 *
 * r_w   = 0:Read 1:Write
 * *adr  = where to get/put the data
 * numb  = # of sectors to get/put
 * first = 1st sector # to get/put = 1st record # tran
 * drive = drive #: 0 = A:, 1 = B:, etc
 */

LONG bios_4(WORD r_w, LONG adr, WORD numb, WORD first, WORD drive)
{
#if DBGBIOS
    kprintf("BIOS rwabs(rw = %d, addr = 0x%08lx, count = 0x%04x, "
            "sect = 0x%04x, dev = 0x%04x)\n",
            r_w, adr, numb, first, drive);
#endif
    return hdv_rw(r_w, adr, numb, first, drive);
}



/**
 * Setexec - set exception vector
 *
 */

LONG bios_5(WORD num, LONG vector)
{
    LONG oldvector;
    LONG *addr = (LONG *) (4L * num);
    oldvector = *addr;

#if DBGBIOS
    kprintf("Bios 5: Setexec(num = 0x%x, vector = 0x%08lx)\n", num, vector);
#endif

    if(vector != -1) {
        *addr = vector;
    }
    return oldvector;
}



/**
 * tickcal - Time between two systemtimer calls
 */

LONG bios_6()
{
    return(20L);        /* system timer is 50 Hz so 20 ms is the period */
}



/**
 * get_bpb - Get BIOS parameter block
 * Returns a pointer to the BIOS Parameter Block for
 * the specified drive in D0.L.  If necessary, it
 * should read boot header information from the media
 * in the drive to determine BPB values.
 *
 * Arguments:
 *  drive - drive  (0 = A:, 1 = B:, etc)
 */

LONG bios_7(WORD drive)
{
    return hdv_bpb(drive);
}



/**
 * bconstat - Read status of output device
 *
 * Returns status in D0.L:
 * -1   device is ready       
 * 0    device is not ready
 */

/* handle  = 0:PRT 1:AUX 2:CON 3:MID 4:KEYB */

LONG bios_8(WORD handle)        /* GEMBIOS character_output_status */
{
    if(handle>7)
        return 0;               /* Illegal handle */

    /* compensate for a known BIOS bug: MIDI and IKBD are switched */
    /*    if(handle==3)  handle=4; else if (handle==4)  handle=3;  */
    /* LVL: now done directly in the table */

    return bcostat_vec[handle]();
}



/**
 * bios_9 - (mediach) See, if floppy has changed
 *
 * Returns media change status for specified drive in D0.L:
 *   0  Media definitely has not changed
 *   1  Media may have changed
 *   2  Media definitely has changed
 * where "changed" = "removed"
 */

LONG bios_9(WORD drv)
{
    return hdv_mediach(drv);
}



/**
 * bios_a - (drvmap) Read drive bitmap
 *
 * Returns a long containing a bit map of logical drives on  the system,
 * with bit 0, the least significant bit, corresponding to drive A.
 * Note that if the BIOS supports logical drives A and B on a single
 * physical drive, it should return both bits set if a floppy is present.
 */

LONG bios_a()
{
    return(drvbits);
}



/*
 *  bios_b - (kbshift)  Shift Key mode get/set.
 *
 *  two descriptions:
 *      o       If 'mode' is non-negative, sets the keyboard shift bits
 *              accordingly and returns the old shift bits.  If 'mode' is
 *              less than zero, returns the IBM0PC compatible state of the
 *              shift keys on the keyboard, as a bit vector in the low byte
 *              of D0
 *      o       The flag parameter is used to control the operation of
 *              this function.  If flag is not -1, it is copied into the
 *              state variable(s) for the shift, control and alt keys,
 *              and the previous key states are returned in D0.L.  If
 *              flag is -1, then only the inquiry is done.
 */

LONG bios_b(WORD flag)
{
    return kbshift(flag);
}




