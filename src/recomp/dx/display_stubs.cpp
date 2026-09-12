// display_stubs.cpp - a definition for every display interface Task 1
// declares, so the tasks that come after it link before it is implemented.
//
// EVERY ONE OF THESE SAYS "NOT IMPLEMENTED", and says it the way its own
// signature can: a status returns the failure, a pointer returns null, a count
// returns zero. None of them pretends to succeed, because a stub that answered
// plausibly would let a caller be written against behaviour that does not
// exist yet and look correct until the day it is replaced.
//
// A later task deletes the stubs it implements from this file. When the file
// is empty the interface is done.
#include "host_api.h"

#include <stddef.h>
#include <string.h>

extern "C" {

// Everything else this file once held - palettes, revisions, frames and draws
// - is implemented in ddraw.cpp as of DISP-T2, and was deleted from here when
// it was. That is how this file is meant to shrink: a task takes a stub away
// by implementing it, and when the file holds nothing but this comment the
// interface is done.

} // extern "C"
