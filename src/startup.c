/*
 * Minimal _start for Android ARM32 PIE binaries compiled with gnueabi-gcc.
 * Android's linker does NOT export __libc_start_main (unlike glibc).
 * This provides a bare-bones entry point that calls main() and exit().
 */

extern int main(int argc, char **argv);

/* Bionic exports exit() in libc.so — declare without pulling in glibc headers */
extern void exit(int status) __attribute__((noreturn));

__attribute__((naked, used, section(".text._start")))
void _start(void)
{
    __asm__ volatile(
        "mov    fp, #0          \n\t"  /* clear frame pointer */
        "mov    lr, #0          \n\t"  /* clear link register  */
        "ldr    r0, [sp]        \n\t"  /* r0 = argc            */
        "add    r1, sp, #4      \n\t"  /* r1 = argv            */
        "bl     main            \n\t"  /* call main()          */
        "bl     exit            \n\t"  /* exit(main's retval)  */
        /* exit() is noreturn, but add a loop in case something goes wrong */
        "1: b 1b               \n\t"
    );
}
