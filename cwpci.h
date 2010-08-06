/*
 * Find the I/O space for the index'th PCI catweasel card in the
 * machine (0-origin).  Return port address if found, -1 if not found.
 *
 * The Catweasel version is returned in *cw_mk:
 *   4 = MK4
 *   3 = MK3
 *
 * If no card is found, *cw_mk is left unchanged.  There could still
 * be a MK1 card on the ISA bus, but that's for other code to determine.
 */
int pci_find_catweasel(int index, int* cw_mk);
