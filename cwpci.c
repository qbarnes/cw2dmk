/*
 * Catweasel -- Advanced Floppy Controller
 * PCI detection routines
 *
 * Copyright (C) 2002 by Timothy Mann
 * $Id: cwpci.c,v 1.6 2010/01/15 19:28:46 mann Exp $
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*#define DEBUG_PCI(stmt) stmt*/
#define DEBUG_PCI(stmt)

#include <stdio.h>
#include <stdint.h>
#include "cwpci.h"

#if __DJGPP__
#include <dpmi.h>
#include <string.h>

/* Translate PCI BIOS error code to string.
 * See http://www.delorie.com/djgpp/doc/rbinter/it/29/7.html
 */
const char *
pci_errstr(int code)
{
  switch (code) {
  case 0x00:
    return "successful";
  case 0x81:
    return "unsupported function";
  case 0x83:
    return "bad vendor id";
  case 0x86:
    return "device not found";
  case 0x87:
    return "bad register number";
  case -1:
    return "PCI BIOS not present";
  default:
    return "unknown error code";
  }
}


/* 1AB101
 * INT 1A - PCI BIOS v2.0c+ - INSTALLATION CHECK
 * See http://www.delorie.com/djgpp/doc/rbinter/id/81/23.html
 *
 * Check if PCI bus installed.  Returns 0 if OK, -1 if error
 * invoking the BIOS through DPMI or the BIOS is not present.
 */
int
pci_install_check(void)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));

  r.x.ax = 0xb101;
  r.d.edi = 0x0;

  if (__dpmi_int(0x1a, &r) != 0) {
    DEBUG_PCI( printf("pci_install_check: BIOS call failed\n"); )
    return -1;
  }
	
  if (r.h.ah != 0 || r.d.edx != 0x20494350) {
    DEBUG_PCI( printf("pci_install_check: PCI BIOS not found\n"); )
    return -1;
  }

  DEBUG_PCI( printf("pci_install_check: PCI BIOS found\n"); )
  return 0;
}


/* 1AB102
 * INT 1A - PCI BIOS v2.0c+ - FIND PCI DEVICE
 * http://www.delorie.com/djgpp/doc/rbinter/id/82/23.html
 *
 * Find the index'th PCI device with the given vendorID and deviceID.
 * Returns 0 if OK, -1 if error invoking the BIOS through DPMI, or a
 * PCI BIOS error code.  If OK, sets bus, device, and func to the
 * address of the device found.
 */
int
pci_find(int vendorID, int deviceID, int index,
	 int *bus, int *device, int *func)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));

  r.x.ax = 0xb102;
  r.x.cx = deviceID;
  r.x.dx = vendorID;
  r.x.si = index;

  if (__dpmi_int(0x1a, &r) != 0) {
    DEBUG_PCI( printf("pci_find: BIOS call failed\n"); )
    return -1;
  }
	
  if (r.h.ah == 0) {
    *bus = r.h.bh;
    *device = (r.h.bl >> 3) & 0x1f;
    *func = r.h.bl & 0x03;
    DEBUG_PCI( printf("pci_install_check(0x%x, 0x%x, %d) -> (%d, %d, %d)\n",
		      vendorID, deviceID, index, *bus, *device, *func); )
  } else {
    DEBUG_PCI( printf("pci_find(0x%x, 0x%x, %d): %s\n",
		      vendorID, deviceID, index, pci_errstr(r.h.ah)); )
  }

  return r.h.ah;
}


/* 1AB10A
 * INT 1A - PCI BIOS v2.0c+ - READ CONFIGURATION DWORD
 * http://www.delorie.com/djgpp/doc/rbinter/id/87/23.html
 *
 * Read a 32-bit value from PCI configuration space.  Returns 0 if OK,
 * -1 if error invoking the BIOS through DPMI, or a PCI BIOS error
 * code.  If OK, sets value.
 */
int
pci_read_config_dword(int bus, int device, int func,
		      int reg, uint32_t *value)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));

  r.x.ax = 0xb10a;
  r.h.bh = bus;
  r.h.bl = (device << 3) + func;
  r.x.di = reg;

  if (__dpmi_int(0x1a, &r) != 0) {
    DEBUG_PCI( printf("pci_read_config_dword: BIOS call failed\n"); )
    return -1;
  }
	
  if (r.h.ah == 0) {
    *value = r.d.ecx;
    DEBUG_PCI( printf("pci_read_config_dword(%d, %d, %d, %d) -> 0x%lx\n",
		      bus, device, func, reg, *value); )
  } else {
    DEBUG_PCI( printf("pci_read_config_dword(%d, %d, %d, %d): %s\n",
		      bus, device, func, reg, pci_errstr(r.h.ah)); )
  }

  return r.h.ah;
}


/*
 * Find the I/O space for the index'th PCI catweasel card in the
 * machine (0-origin).  Return port address if found, -1 if not found.
 */
int
pci_find_catweasel(int index, int *cw_mk)
{
  int i = 0, j = 0, res;
  int bus, device, func, mk = 0;
  uint32_t subsysID, baseAddr;

  if (pci_install_check() != 0) return -1;

  while (i <= index) {
    /* Find the next card that uses the Tiger Jet Networks Tiger320 PCI chip */
    res = pci_find(0xe159, 0x0001, j++, &bus, &device, &func);
    if (res != 0) return -1;
    /* Read the subsystem vendor ID + subsystem ID */
    res = pci_read_config_dword(bus, device, func, 0x2c, &subsysID);
    if (res != 0) continue;
    /* Check if they match the Catweasel */
    switch (subsysID) {
    case 0x00021212: 	/* Catweasel MK3 */
    case 0x00031212: 	/* Catweasel MK3 alternate */
      mk = 3;
      break;
    case 0x00025213: 	/* Catweasel MK4 */
    case 0x00035213: 	/* Catweasel MK4 alternate */
      mk = 4;
      break;
    default:
      DEBUG_PCI( printf("subsysID 0x%lx\n", subsysID); )
      continue;
    }
    i++;
  }

  for (i = 0x10; i <= 0x24; i += 4) {
    /* Read a base address */
    res = pci_read_config_dword(bus, device, func, i, &baseAddr);
    if (res != 0) return -1;
    /* Check for I/O space */
    if (baseAddr & 1) {
      *cw_mk = mk;
      return baseAddr & ~3;
    }
    DEBUG_PCI( printf("baseAddr 0x%lx not I/O space\n", baseAddr); )
  }

  return -1;
}


#else /* __DJGPP__ */
#ifdef __cplusplus
extern "C" {
#endif
#include <pci/pci.h>
#ifdef __cplusplus
}
#endif

/*
 * Find the I/O space for the index'th PCI catweasel card in the
 * machine (0-origin).  Return port address if found, -1 if not found.
 */
int
pci_find_catweasel(int index, int *cw_mk)
{
  struct pci_access *pa;
  struct pci_dev *pd;
  int i, mk;

  /* Initialize */
  pa = pci_alloc();
  pci_init(pa);
  
  /* Scan buses */
  pci_scan_bus(pa);
  
  /* Iterate through devices */
  for (pd = pa->devices; pd != NULL; pd = pd->next) {

    pci_fill_info(pd, PCI_FILL_IDENT);
    if (pd->vendor_id == 0xe159 &&  /* Tiger Jet Networks */
	pd->device_id == 0x0001) {  /* Tiger320 PCI chip */

      switch (pci_read_long(pd, 0x2c)) {
      case 0x00021212: 	/* Catweasel MK3 */
      case 0x00031212: 	/* Catweasel MK3 alternate */
	mk = 3;
	break;
      case 0x00025213: 	/* Catweasel MK4 */
      case 0x00035213: 	/* Catweasel MK4 alternate */
	mk = 4;
	break;
      default:
	continue;
      }

      /* Is this the index'th card? */
      if (index-- > 0) continue;

      /* Find the I/O space */
      pci_fill_info(pd, PCI_FILL_BASES);
      for (i = 0; i < 6; i++) {
	unsigned flg = pci_read_long(pd, PCI_BASE_ADDRESS_0 + 4 * i);
	if (flg != 0xffffffff && (flg & PCI_BASE_ADDRESS_SPACE_IO)) {
	  u16 sval;
	  sval = pci_read_word(pd, PCI_COMMAND);
	  sval |= PCI_COMMAND_MEMORY | PCI_COMMAND_IO;
	  pci_write_word(pd, PCI_COMMAND, sval);
	  *cw_mk = mk;
	  return pd->base_addr[i] & PCI_ADDR_IO_MASK;
	}
	DEBUG_PCI( printf("baseAddr %d 0x%llx not I/O space\n",
			  i, (long long)pd->base_addr[i]); )
      }
    }

  }
  return -1;
}


#endif /* __DJGPP__ */
