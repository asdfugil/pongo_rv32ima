// Copyright 2022 Charles Lohr, you may use this file or any portions herein under any of the BSD, MIT, or CC0 licenses.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pongo.h>
#include <task.h>

#include "default64mbdtc.h"

// Just default RAM amount is 64MB.
uint32_t ram_amt = 64*1024*1024;
int fail_on_all_faults = 0;

static int64_t SimpleReadNumberInt( const char * number, int64_t defaultNumber );
static uint64_t GetTimeMicroseconds();
static void ResetKeyboardInput();
static void CaptureKeyboardInput();
static uint32_t HandleException( uint32_t ir, uint32_t retval );
static uint32_t HandleControlStore( uint32_t addy, uint32_t val );
static uint32_t HandleControlLoad( uint32_t addy );
static void HandleOtherCSRWrite( uint8_t * image, uint16_t csrno, uint32_t value );
static int32_t HandleOtherCSRRead( uint8_t * image, uint16_t csrno );
static void MiniSleep();
static int IsKBHit();
static int ReadKBByte();

// This is the functionality we want to override in the emulator.
//  think of this as the way the emulator's processor is connected to the outside world.
#define MINIRV32WARN( x... ) printf( x );
#define MINIRV32_DECORATE  static
#define MINI_RV32_RAM_SIZE ram_amt
#define MINIRV32_IMPLEMENTATION
#define MINIRV32_POSTEXEC( pc, ir, retval ) { if( retval > 0 ) { if( fail_on_all_faults ) { printf( "FAULT\n" ); return 3; } else retval = HandleException( ir, retval ); } }
#define MINIRV32_HANDLE_MEM_STORE_CONTROL( addy, val ) if( HandleControlStore( addy, val ) ) return val;
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL( addy, rval ) rval = HandleControlLoad( addy );
#define MINIRV32_OTHERCSR_WRITE( csrno, value ) HandleOtherCSRWrite( image, csrno, value );
#define MINIRV32_OTHERCSR_READ( csrno, value ) value = HandleOtherCSRRead( image, csrno );

#include "mini-rv32ima.h"

uint8_t * ram_image = 0;
struct MiniRV32IMAState * core;

static void DumpState( struct MiniRV32IMAState * core, uint8_t * ram_image );

static void rv32ima_help(const char* cmd, char* args);
static void rv32ima_setram(const char* cmd, char* args);
static void rv32ima_cmdline(const char* cmd, char* args);
static void rv32ima_image(const char* cmd, char* args);
static void rv32ima_dtb(const char* cmd, char* args);
static void rv32ima_run(const char* cmd, char* args);

static void rv32ima_cmd(const char* cmd, char* args);

struct rv32ima_command {
    char* name;
    char* desc;
    void (*cb)(const char* cmd, char* args);
};

#define RV32IMA_COMMAND(_name, _desc, _cb) {.name = _name, .desc = _desc, .cb = _cb}

static struct rv32ima_command command_table[] = {
	RV32IMA_COMMAND("help", "print help", rv32ima_help),
	RV32IMA_COMMAND("setram", "sets the amount of memory", rv32ima_setram),
	RV32IMA_COMMAND("cmdline", "set cmdline", rv32ima_cmdline),
	RV32IMA_COMMAND("image", "load image", rv32ima_image),
	RV32IMA_COMMAND("dtb", "load dtb", rv32ima_dtb),
	RV32IMA_COMMAND("run", "start emulation", rv32ima_run),	
};

static void rv32ima_cmd(const char* cmd, char* args) {
    char* arguments = command_tokenize(args, 0x1ff - (args - cmd));
    struct rv32ima_command* fallback_cmd = NULL;
    if (arguments) {
        for (int i=0; i < sizeof(command_table) / sizeof(struct rv32ima_command); i++) {
            if (command_table[i].name && !strcmp("help", command_table[i].name)) {
                fallback_cmd = &command_table[i];
            }
            if (command_table[i].name && !strcmp(args, command_table[i].name)) {
                command_table[i].cb(args, arguments);
                return;
            }
        }
        if (*args)
            iprintf("rv32ima: invalid command %s\n", args);
        if (fallback_cmd) return fallback_cmd->cb(cmd, arguments);
    }
}

static void rv32ima_help(const char* cmd, char* args) {
    iprintf("rv32ima usage: rv32ima [subcommand] <subcommand options>\nsubcommands:\n");
    for (int i=0; i < sizeof(command_table) / sizeof(struct rv32ima_command); i++) {
        if (command_table[i].name) {
            iprintf("%16s | %s\n", command_table[i].name, command_table[i].desc ? command_table[i].desc : "no description");
        }
    }
}

static void rv32ima_setram(const char* cmd, char* args) {
    if (! *args) {
        iprintf("rv32ima setram usage: rv32ima setram [ram_amt]\n");
        return;
    }
	ram_amt = strtoul(args, NULL, 16);
}

static char kernel_command_line[4096] = {0};
static void rv32ima_cmdline(const char* cmd, char* args) {
	if (!*args) {
		printf("rv32ima cmdline usage: rv32ima cmdline [cmdline]");
		return;
	}

	snprintf(kernel_command_line, 4096, "%s",args);
}

static void* customDTB = NULL;
static uint32_t dtbLen = 0;
static void* rv32imaKernel = NULL;
static uint32_t rv32imaKernelLen = 0;

static void rv32ima_dtb(const char* cmd, char* args) {
    if (!loader_xfer_recv_count) {
        iprintf("please upload a dtb before issuing this command\n");
        return;
    }
	if (customDTB) free(customDTB);
	customDTB = malloc(loader_xfer_recv_count);
	memcpy(customDTB, loader_xfer_recv_data, loader_xfer_recv_count);
	dtbLen = loader_xfer_recv_count;
	loader_xfer_recv_count = 0;
}

static void rv32ima_image(const char* cmd, char* args) {
    if (!loader_xfer_recv_count) {
        iprintf("please upload a kernel image before issuing this command\n");
        return;
    }
	if (rv32imaKernel) free(rv32imaKernel);
	rv32imaKernel = malloc(loader_xfer_recv_count);
	rv32imaKernelLen = loader_xfer_recv_count;
	memcpy(rv32imaKernel, loader_xfer_recv_data, loader_xfer_recv_count);
	loader_xfer_recv_count = 0;
}

struct task* rv32ima_kbd_task = NULL;
bool rv32ima_task_should_exit = true;
static int emulator();
static void rv32ima_run(const char* cmd, char* args) {
	if (!rv32imaKernel) {
		iprintf("no kernel image uploaded\n");
		return;
	}
	emulator();
}

static void rv32ima_kbd();
static int  emulator() {
	int i;
	long long instct = -1;
	int show_help = 0;
	int time_divisor = 1;
	int fixed_update = 0;
	int do_sleep = 1;
	int single_step = 0;
	int dtb_ptr = 0;

	ram_image = malloc( ram_amt );
restart:
	{
		memset( ram_image, 0, ram_amt );
		if (rv32imaKernelLen > ram_amt) {
			fprintf(stderr, "cannot fit kernel inside memory\n");
			return -1;
		}

		memcpy(ram_image, rv32imaKernel, rv32imaKernelLen);

		if(customDTB)
		{
			int dtb_ptr = ram_amt - dtbLen - sizeof( struct MiniRV32IMAState );
			if ((dtbLen + rv32imaKernelLen + sizeof(struct MiniRV32IMAState)) > ram_amt) {
				fprintf(stderr, "cannot fit dtb inside memory\n");
				return -1;
			}
			memcpy(ram_image + dtb_ptr, customDTB, dtbLen);
		} else {
			// Load a default dtb.
			dtb_ptr = ram_amt - sizeof(default64mbdtb) - sizeof( struct MiniRV32IMAState );
			memcpy( ram_image + dtb_ptr, default64mbdtb, sizeof( default64mbdtb ) );
			if (kernel_command_line[0] != '\0')
			{
				strncpy( (char*)( ram_image + dtb_ptr + 0xc0 ), kernel_command_line, 54 );
			}
		}
	}

	CaptureKeyboardInput();

	// The core lives at the end of RAM.
	core = (struct MiniRV32IMAState *)(ram_image + ram_amt - sizeof( struct MiniRV32IMAState ));
	core->pc = MINIRV32_RAM_IMAGE_OFFSET;
	core->regs[10] = 0x00; //hart ID
	core->regs[11] = dtb_ptr?(dtb_ptr+MINIRV32_RAM_IMAGE_OFFSET):0; //dtb_pa (Must be valid pointer) (Should be pointer to dtb)
	core->extraflags |= 3; // Machine-mode.

	if(customDTB == NULL )
	{
		// Update system ram size in DTB (but if and only if we're using the default DTB)
		// Warning - this will need to be updated if the skeleton DTB is ever modified.
		uint32_t * dtb = (uint32_t*)(ram_image + dtb_ptr);
		if( dtb[0x13c/4] == 0x00c0ff03 )
		{
			uint32_t validram = dtb_ptr;
			dtb[0x13c/4] = (validram>>24) | ((( validram >> 16 ) & 0xff) << 8 ) | (((validram>>8) & 0xff ) << 16 ) | ( ( validram & 0xff) << 24 );
		}
	}

	if (!rv32ima_kbd_task) {
		rv32ima_task_should_exit = false;
		rv32ima_kbd_task = task_create("rv32ima_kbd", rv32ima_kbd);
	}
	rv32ima_kbd_task->flags |= TASK_RESTART_ON_EXIT;
	// Image is loaded.
	uint64_t rt;
	uint64_t lastTime = (fixed_update)?0:(GetTimeMicroseconds()/time_divisor);
	int instrs_per_flip = single_step?1:1024;
	for( rt = 0; rt < instct+1 || instct < 0; rt += instrs_per_flip )
	{
		uint64_t * this_ccount = ((uint64_t*)&core->cyclel);
		uint32_t elapsedUs = 0;
		if( fixed_update )
			elapsedUs = *this_ccount / time_divisor - lastTime;
		else
			elapsedUs = GetTimeMicroseconds()/time_divisor - lastTime;
		lastTime += elapsedUs;

		if( single_step )
			DumpState( core, ram_image);

		int ret = MiniRV32IMAStep( core, ram_image, 0, elapsedUs, instrs_per_flip ); // Execute upto 1024 cycles before breaking out.
		switch( ret )
		{
			case 0: break;
			case 1: if( do_sleep ) MiniSleep(); *this_ccount += instrs_per_flip; break;
			case 3: instct = 0; break;
			case 0x7777: goto restart;	//syscon code for restart
			case 0x5555: printf( "POWEROFF@0x%08x%08x\n", core->cycleh, core->cyclel ); goto end_emulation; //syscon code for power-off
			default: printf( "Unknown failure\n" ); goto end_emulation;
		}
	}
end_emulation:
	rv32ima_task_should_exit = true;
	DumpState( core, ram_image);
	return 0;
}


//////////////////////////////////////////////////////////////////////////
// Platform-specific functionality
//////////////////////////////////////////////////////////////////////////


// #include <termios.h>

static void CtrlC()
{
	DumpState( core, ram_image);
	exit( 0 );
}

// Override keyboard, so we can capture all keyboard input for the VM.
static void CaptureKeyboardInput()
{
	
}

static void ResetKeyboardInput()
{
	// PongoOS doesn't support any of that
}

static void MiniSleep()
{
	usleep(500);
}

static uint64_t GetTimeMicroseconds()
{
	return (get_ticks() / 24);
}

static int is_eofd;
static char KBbuf;
static bool hasKBBuf;

static void rv32ima_kbd() {
	KBbuf = getchar();
	if (rv32ima_task_should_exit) task_exit();
	if (feof(stdin)) hasKBBuf = false;
	else hasKBBuf = true;
	clearerr(stdin);
}

static int IsKBHit()
{
	if( is_eofd ) return -1;
	if (hasKBBuf) return 1;
	return 0;
}

static int ReadKBByte()
{
	if( is_eofd ) return 0xffffffff;
	if (!hasKBBuf) return -1;
	hasKBBuf = false;
	return KBbuf;
}


//////////////////////////////////////////////////////////////////////////
// Rest of functions functionality
//////////////////////////////////////////////////////////////////////////

static uint32_t HandleException( uint32_t ir, uint32_t code )
{
	// Weird opcode emitted by duktape on exit.
	if( code == 3 )
	{
		// Could handle other opcodes here.
	}
	return code;
}

static uint32_t HandleControlStore( uint32_t addy, uint32_t val )
{
	if( addy == 0x10000000 ) //UART 8250 / 16550 Data Buffer
	{
		printf( "%c", val );
		fflush( stdout );
	}
	return 0;
}


static uint32_t HandleControlLoad( uint32_t addy )
{
	// Emulating a 8250 / 16550 UART
	if( addy == 0x10000005 )
		return 0x60 | IsKBHit();
	else if( addy == 0x10000000 && IsKBHit() )
		return ReadKBByte();
	return 0;
}

static void HandleOtherCSRWrite( uint8_t * image, uint16_t csrno, uint32_t value )
{
	if( csrno == 0x136 )
	{
		printf( "%d", value ); fflush( stdout );
	}
	if( csrno == 0x137 )
	{
		printf( "%08x", value ); fflush( stdout );
	}
	else if( csrno == 0x138 )
	{
		//Print "string"
		uint32_t ptrstart = value - MINIRV32_RAM_IMAGE_OFFSET;
		uint32_t ptrend = ptrstart;
		if( ptrstart >= ram_amt )
			printf( "DEBUG PASSED INVALID PTR (%08x)\n", value );
		while( ptrend < ram_amt )
		{
			if( image[ptrend] == 0 ) break;
			ptrend++;
		}
		if( ptrend != ptrstart )
			fwrite( image + ptrstart, ptrend - ptrstart, 1, stdout );
	}
	else if( csrno == 0x139 )
	{
		putchar( value ); fflush( stdout );
	}
}

static int32_t HandleOtherCSRRead( uint8_t * image, uint16_t csrno )
{
	if( csrno == 0x140 )
	{
		if( !IsKBHit() ) return -1;
		return ReadKBByte();
	}
	return 0;
}

static int64_t SimpleReadNumberInt( const char * number, int64_t defaultNumber )
{
	if( !number || !number[0] ) return defaultNumber;
	int radix = 10;
	if( number[0] == '0' )
	{
		char nc = number[1];
		number+=2;
		if( nc == 0 ) return 0;
		else if( nc == 'x' ) radix = 16;
		else if( nc == 'b' ) radix = 2;
		else { number--; radix = 8; }
	}
	char * endptr;
	uint64_t ret = strtoll( number, &endptr, radix );
	if( endptr == number )
	{
		return defaultNumber;
	}
	else
	{
		return ret;
	}
}

static void DumpState( struct MiniRV32IMAState * core, uint8_t * ram_image )
{
	uint32_t pc = core->pc;
	uint32_t pc_offset = pc - MINIRV32_RAM_IMAGE_OFFSET;
	uint32_t ir = 0;

	printf( "PC: %08x ", pc );
	if( pc_offset >= 0 && pc_offset < ram_amt - 3 )
	{
		ir = *((uint32_t*)(&((uint8_t*)ram_image)[pc_offset]));
		printf( "[0x%08x] ", ir ); 
	}
	else
		printf( "[xxxxxxxxxx] " ); 
	uint32_t * regs = core->regs;
	printf( "Z:%08x ra:%08x sp:%08x gp:%08x tp:%08x t0:%08x t1:%08x t2:%08x s0:%08x s1:%08x a0:%08x a1:%08x a2:%08x a3:%08x a4:%08x a5:%08x ",
		regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7],
		regs[8], regs[9], regs[10], regs[11], regs[12], regs[13], regs[14], regs[15] );
	printf( "a6:%08x a7:%08x s2:%08x s3:%08x s4:%08x s5:%08x s6:%08x s7:%08x s8:%08x s9:%08x s10:%08x s11:%08x t3:%08x t4:%08x t5:%08x t6:%08x\n",
		regs[16], regs[17], regs[18], regs[19], regs[20], regs[21], regs[22], regs[23],
		regs[24], regs[25], regs[26], regs[27], regs[28], regs[29], regs[30], regs[31] );
}

char* module_name = "mini-rv32ima";

void module_entry() {
	command_register("rv32ima", "rv32ima emulator", rv32ima_cmd);
	return;
}

struct pongo_exports exported_symbols[] = {
	NULL
};
