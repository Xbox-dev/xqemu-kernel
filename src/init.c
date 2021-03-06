/*
 * Copyright (C) 2018 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "boot.h"
#include "BootEEPROM.h"
#include "video.h"
#include "memory_layout.h"
#include "xdecl.h"
#include "lib/xbe/xbe.h"

volatile CURRENT_VIDEO_MODE_DETAILS vmode;
extern char HEAP_BASE[], HEAP_SIZE[];
extern volatile int nInteruptable;
extern void (*video_interrupt_hook)(void);

void init(void)
{
	nInteruptable = 0;

	serial_init();
	printk("XQEMU HLE Kernel\n");

	m_import_addrs[156] = (void*)&BIOS_TICK_COUNT; // KeTickCount

	MemoryManagementInitialization(HEAP_BASE, (u32)HEAP_SIZE);
	BootInterruptsWriteIdt();
	BootPciPeripheralInitialization();
	BootAGPBUSInitialization();
	BootDetectMemorySize();
	BootEepromReadEntireEEPROM();
	memset((void *)FB_START,0x00,0x400000);
	BootVgaInitializationKernelNG((CURRENT_VIDEO_MODE_DETAILS *)&vmode);

	I2CTransmitWord(0x10, 0x1a01);
	I2CTransmitWord(0x10, 0x1901); // no reset on eject

	/* Here, the interrupts are Switched on now */
	BootPciInterruptEnable();
	nInteruptable = 1;

	// FIXME: Move this into a NIC init function
	// set Ethernet MAC address from EEPROM
    {
		volatile u8 * pb=(u8 *)0xfef000a8;  // Ethernet MMIO base + MAC register offset (<--thanks to Anders Gustafsson)
		int n;
		for(n=5;n>=0;n--) { *pb++=	eeprom.MACAddress[n]; } // send it in backwards, its reversed by the driver
    }

	// FIXME: Move this into a HLE init function
	// Setup the HLE device
	#define KHLE_BAR 0xfed0c000
	PciWriteDword(0, 0x11,    0, 0x04, 7);
	PciWriteDword(0, 0x11,    0, 0x10, KHLE_BAR);

	//
	// FIXME: Until we have proper file i/o and virtual memory allocation,
	// just punt to XQEMU HLE to DMA the XBE right into memory.
	//
	// The right way is to implement the necessary kernel functions,
	// then call them to find and load the XBE.
	//

	// Get XBE size
	size_t xbe_size = *((volatile uint32_t *)(KHLE_BAR + 0x10));
	printk("XBE Size: %x\n", xbe_size);

	// Allocate some memory for the XBE
	void *xbe_load_addr = MmAllocateContiguousMemoryEx(align_up(xbe_size, 0x1000), 0, 0x3FFFFFF, 0, 0);
	// FIXME: Leak

	printk("XBE Load Address: %x\n", xbe_load_addr);
	assert(xbe_load_addr != NULL);

	// Poke the KHLE device to load an XBE into RAM for us
	*((volatile uint32_t *)(KHLE_BAR + 0x10)) = (uint32_t)xbe_load_addr & ~0x80000000;

	XBE_HEADER *hdr = load_xbe(xbe_load_addr);

    // Get unscrambled XBE entry point
    void (*xbe_entry)(void) = get_xbe_entry(hdr);
    printk("Jumping to XBE entry point %p\n", xbe_entry);
    xbe_entry();

	while(1);
}

//-----------------------------------------------------------------------------

#define PCI_SLOT(device, function) (((function) << 5) | (device))
#define PCI_DEV(slot) ((slot) & 0x1f)
#define PCI_FUNC(slot) (((slot) >> 5) & 0x7)

/**
 * AvSendTVEncoderOption
 */
XBAPI VOID NTAPI AvSendTVEncoderOption(
	PVOID  RegisterBase,
	ULONG  Option,
	ULONG  Param,
	PULONG Result
	)
{
	printf("AvSendTVEncoderOption(%x, %x, %x, %x)\n", RegisterBase, Option, Param, Result);
	if (Result != NULL) {
		*Result = 0x00400101;
	}
}

/**
 * AvSetDisplayMode
 */
XBAPI ULONG NTAPI AvSetDisplayMode(
	PVOID RegisterBase,
	ULONG Step,
	ULONG DisplayMode,
	ULONG SourceColorFormat,
	ULONG Pitch,
	ULONG FrameBuffer
	)
{
	return 0;
}

/**
 * HalGetInterruptVector
 */
XBAPI ULONG NTAPI HalGetInterruptVector(
	ULONG  BusInterruptLevel,
	PKIRQL Irql
	)
{
	if (BusInterruptLevel == 3) {
		*Irql = 3;
	}

	return 0;
}

/**
 * HalReadWritePCISpace
 */
XBAPI VOID NTAPI HalReadWritePCISpace(
	ULONG   BusNumber,
	ULONG   SlotNumber,
	ULONG   RegisterNumber,
	PVOID   Buffer,
	ULONG   Length,
	BOOLEAN WritePCISpace
	)
{
	if (WritePCISpace == FALSE) {
		if (Length == 4) {
			*(volatile DWORD *)Buffer = PciReadDword(BusNumber, PCI_DEV(SlotNumber), PCI_FUNC(SlotNumber), RegisterNumber);
			return;
		}
	}

	assert(0);
}

/**
 * KeConnectInterrupt
 */
XBAPI BOOLEAN NTAPI
KeConnectInterrupt(
	PKINTERRUPT Interrupt
	)
{
	return TRUE;
}

/**
 * KeDelayExecutionThread
 */
XBAPI NTSTATUS NTAPI
KeDelayExecutionThread(
	KPROCESSOR_MODE WaitMode,
	BOOLEAN         Alertable,
	PLARGE_INTEGER  Interval
	)
{
	wait_ms(100);
	return STATUS_SUCCESS;
}

/**
 * KeInitializeDpc
 */
XBAPI VOID NTAPI
KeInitializeDpc(
	PKDPC              Dpc,
	PKDEFERRED_ROUTINE DeferredRoutine,
	PVOID              DeferredContext
	)
{
	Dpc->DeferredRoutine = DeferredRoutine;
	Dpc->DeferredContext = DeferredContext;
}

/**
 * KeInitializeInterrupt
 */
XBAPI VOID NTAPI
KeInitializeInterrupt(
	PKINTERRUPT       Interrupt,
	PKSERVICE_ROUTINE ServiceRoutine,
	PVOID             ServiceContext,
	ULONG             Vector,
	KIRQL             Irql,
	KINTERRUPT_MODE   InterruptMode,
	BOOLEAN           ShareVector
	)
{
	if (Irql == 3) {
		video_interrupt_hook = (void*)ServiceRoutine;
	}
}

/**
 * KeInsertQueueDpc
 */
XBAPI BOOLEAN NTAPI
KeInsertQueueDpc(
	PRKDPC Dpc,
	PVOID  SystemArgument1,
	PVOID  SystemArgument2
	)
{
	assert(Dpc != NULL);

	// Just call the DPC for now
    void (*dpc_func)(void) = Dpc->DeferredRoutine;
    dpc_func();
    return TRUE;
}

/**
 * MmClaimGpuInstanceMemory
 */
XBAPI PVOID NTAPI
MmClaimGpuInstanceMemory(
	SIZE_T  NumberOfBytes,
	PSIZE_T NumberOfPaddingBytes
	)
{
	if (NumberOfPaddingBytes != NULL) {
		*NumberOfPaddingBytes = 0x10000;
	}

	return (PVOID)0x83FF0000;
}

/**
 * NtCreateEvent
 */
XBAPI NTSTATUS NTAPI
NtCreateEvent(
	PHANDLE            EventHandle,
	POBJECT_ATTRIBUTES ObjectAttributes,
	EVENT_TYPE         EventType,
	BOOLEAN            InitialState
	)
{
	return STATUS_SUCCESS;
}

/**
 * NtPulseEvent
 */
XBAPI NTSTATUS NTAPI
NtPulseEvent(
	HANDLE EventHandle,
	PLONG  PreviousState
	)
{
	return STATUS_SUCCESS;
}

/**
 * NtWaitForSingleObject
 */
XBAPI NTSTATUS NTAPI
NtWaitForSingleObject(
	HANDLE         Handle,
	BOOLEAN        Alertable,
	PLARGE_INTEGER Timeout
	)
{
	return STATUS_SUCCESS;
}

/**
 * HalRegisterShutdownNotification
 */
XBAPI VOID NTAPI
HalRegisterShutdownNotification(
	PHAL_SHUTDOWN_REGISTRATION ShutdownRegistration,
	BOOLEAN                    Register
	)
{
}

////////////////////////////////////////////////////////////////////////////////
// dlmalloc support

int errno;

void abort()
{
	printk("abort()!\n");
	while (1);
}

int time()
{
	// FIXME: Should be since EPOCH
	return BIOS_TICK_COUNT;
}
