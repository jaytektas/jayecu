#pragma once
#include "../../Storage/SdArbitrator.h"

// THE ONE MOUNT OF "0:". Every SD user — config store, learned store, datalogger, FETCH_FILE,
// WRITE_FILE — calls sd_volume_ensure() before touching a file, and nobody unmounts.
//
// Each of them used to own a FATFS and end its work with f_mount(nullptr, "0:", 0), then mount its
// own at the start. The mount is global, so one user's unmount pulled the volume from under another's
// open file: a FETCH_FILE while logging killed the log (its FIL pointed at a volume that was gone),
// and a key-off totem flush could be cut the same way.
//
// The one time a mount really must be dropped is after the PC has had the card over USB — it may have
// changed anything. The arbitrator counts those hand-backs (ecu_epoch) and this remounts on a change.
bool sd_volume_ensure(SdArbitrator& arb);
