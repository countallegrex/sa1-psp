// Imports from ARK-4's SystemControl (library SystemCtrlForUser).
//
// Written here rather than linking ARK's or pspsdk's GPL'd systemctrl
// stubs.  The NIDs are the default export hashes ARK-4 uses: the first four
// bytes, little-endian, of SHA-1 of the function name.  Checked against
// ARK-4 libs/SystemCtrlForUser/SystemCtrlForUser.S and core/systemctrl/
// exports.exp (PSP-Archive/ARK-4 7c03c37), and against pspsdk's own
// libpspsystemctrl_user.a.  The library flags word is the one ARK's stub uses.
	.set noreorder

#include "pspimport.s"

	IMPORT_START "SystemCtrlForUser",0x40090000
	IMPORT_FUNC  "SystemCtrlForUser",0xABA7F1B0,sctrlKernelLoadExecVSHMs2
	IMPORT_FUNC  "SystemCtrlForUser",0xBC939DC1,sctrlSESetBootConfFileIndex
	IMPORT_FUNC  "SystemCtrlForUser",0x5A35C948,sctrlSESetUmdFile
