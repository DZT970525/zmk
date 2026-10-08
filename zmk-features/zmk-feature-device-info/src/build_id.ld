/*
 * Place the GNU build-id note into the ROMABLE (flash) region so it is part of
 * the flashed image and directly memory-mapped for runtime reading.
 *
 * Zephyr disables build-id by default (`--build-id=none` in the linker `base`
 * property). CMakeLists.txt re-enables it with `--build-id=sha1`; this snippet
 * gives the resulting `.note.gnu.build-id` a fixed home and boundary symbols
 * so the firmware can read it. Without an explicit placement the note becomes
 * an orphan section and may be assigned address 0.
 */

#include <zephyr/linker/linker-defs.h>

SECTION_PROLOGUE(.note.gnu.build-id,,)
{
	__build_id_note_start = .;
	KEEP(*(.note.gnu.build-id))
	__build_id_note_end = .;
} GROUP_LINK_IN(ROMABLE_REGION)
