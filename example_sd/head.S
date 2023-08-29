// See LICENSE.Sifive for license details.
#include <platform.h>
#include <smp.h>
#include "common.h"

  .section .text.init
  .option norvc
  .globl _prog_start
_prog_start:
  li a2, (PAYLOAD_DEST + 0xfffe000)
  sd a1, 0(a2)
  smp_pause(s1, s2)
  li sp, (PAYLOAD_DEST + 0xffff000)
  call main
  smp_resume(s1, s2)
  csrr a0, mhartid // hartid for next level bootloader
  //la a1, dtb // dtb address for next level bootloader
  li a2, (PAYLOAD_DEST + 0xfffe000)
  ld a1, 0(a2)
  li s1, PAYLOAD_DEST
  jr s1

  .section .dtb
  .align 3
dtb:
