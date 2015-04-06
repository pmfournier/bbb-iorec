.origin 0 // offset of the start of the code in PRU memory
.entrypoint START // program entry point, used by debugger only

// Address for the Constant table Programmable Pointer Register 0(CTPPR_0)
#define CTPPR_0         0x22028

START:
    // Activate OCP port
    LBCO r0, C4, 4, 4
    CLR  r0, r0, 4
    SBCO r0, C4, 4, 4


    // C28 points to 0x000nnn00 (PRU shared RAM). nnn is set by writing to address CTPPR_0.
    // Writing zero means C28 will point to the beginning of PRU0's private memory.
    MOV       r0, 0x00000000
    MOV       r1, CTPPR_0
    ST32      r0, r1

    MOV r0, 0           // r0 = write counter; not moduloed
    LBCO r1, C28, 8, 4  // r1 = base address for the ddr memory, which we're reading from the private memory
    LBCO r2, C28, 12, 4 // r2 = size of designated region, must be a power of 2,
                        //      passed by C code, read from the private memory
                        // r3 = temporary var for computations in the loop
    LBCO r4, C28, 16, 4 // r4 = delay amount, passed by C code
    MOV r5, 0           // r5 = the relative counter; says the offset in the ddr region we're writing to next

loop1:

    SBCO r0, C28, 0, 4  // write the before_write counter; its value is unincremented so the offset
                        // we are about to write is the actual value of this counter

#ifdef TEST_PATTERN
    SBBO r0, r1, r5, 4  // write to the designated memory; we write the actual value of the unmoduloed counter
#else
    SBBO r31, r1, r5, 4 // write the contents of the direct io register
#endif

    ADD r0, r0, 4       // increase the value of the unmoduloed offset counter
    ADD r5, r5, 4       // increase moduloed counter

    // Reset moduloed counter if necessary
    QBNE skip_reset, r5, r2
    MOV r5, 0
skip_reset:
    SBCO r0, C28, 4, 4  // the cumulative write count at offset 8 of private memory

    // Prepare delay loop
    MOV r3, 0
delay:
    ADD r3, r3, 1
    QBNE delay, r3, r4

    // Jump back to the beginning
    JMP loop1
