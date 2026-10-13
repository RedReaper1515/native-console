/*
 * A program that does nothing, for as long as it is asked to.
 *
 * The frontend's own tests - does the menu open, does the game stop while it
 * is open, does it start again - need a guest that is still there to be
 * stopped. Every other program in this directory reports its verdicts and
 * exits, which for a test of the *frontend* is the one thing that must not
 * happen: a machine whose guest has finished stops advancing its field
 * counter, and "the game runs again" then fails for a reason that has
 * nothing to do with the menu.
 *
 * So this sets the video interface going and waits on the retrace for ever.
 * It prints no verdicts, which is why tools/gc/run_gc_tests.py knows not to
 * ask it for any.
 */

#include <gccore.h>

static void *framebuffer;

int main(void) {
  GXRModeObj *mode;

  VIDEO_Init();
  mode = VIDEO_GetPreferredMode(NULL);
  framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
  VIDEO_Configure(mode);
  VIDEO_SetNextFramebuffer(framebuffer);
  VIDEO_SetBlack(FALSE);
  VIDEO_Flush();
  VIDEO_WaitVSync();
  if (mode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

  /* Waiting on the retrace rather than spinning, so that the machine is
   * doing what a game does between frames: sitting in an interrupt handler's
   * wake-up rather than burning the processor. */
  for (;;) VIDEO_WaitVSync();
  return 0;
}
