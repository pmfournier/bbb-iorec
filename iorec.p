/* Functions use r20 as argument and r20-r27 for their private
 * operations. r29 is used as stack. r28 as return value
 */

.setcallreg r29.w0
.origin 0 // offset of the start of the code in PRU memory
.entrypoint START // program entry point, used by debugger only

// To signal the host that we're done, we set bit 5 in our R31
// simultaneously with putting the number of the signal we want
// into R31 bits 0-3. See 5.2.2.2 in AM335x PRU-ICSS Reference Guide.
#define PRU0_R31_VEC_VALID (1<<5)
#define SIGNUM 3 // corresponds to PRU_EVTOUT_0

#define CLOCK 200000000 // PRU is always clocked at 200MHz
#define CLOCKS_PER_LOOP 2 // loop contains two instructions, one clock each
#define DELAY_US_MULTIPLIER (CLOCK / CLOCKS_PER_LOOP / 1000000)

#define SHARED_RAM           0x100
#define PRU0_CTRL            0x22000
#define PRU1_CTRL            0x24000
#define CTPPR0               0x28

#define GPIO1               0x4804c000 /* this is gpio1 (zero-based) */
#define GPIO_CLEARDATAOUT   0x190
#define GPIO_SETDATAOUT     0x194
#define GPIO_OE             0x134


#define PRU0_PRU1_INTERRUPT     17
#define PRU1_PRU0_INTERRUPT     18
#define PRU0_ARM_INTERRUPT      19
#define PRU1_ARM_INTERRUPT      20
#define ARM_PRU0_INTERRUPT      21
#define ARM_PRU1_INTERRUPT      22

#define CONST_PRUCFG	     C4
#define CONST_PRUDRAM        C24
#define CONST_PRUSHAREDRAM   C28
#define CONST_DDR            C31

// Address for the Constant table Block Index Register (CTBIR)
#define CTBIR          0x22020

// Address for the Constant table Programmable Pointer Register 0(CTPPR_0)
#define CTPPR_0         0x22028

// Address for the Constant table Programmable Pointer Register 1(CTPPR_1)
#define CTPPR_1         0x2202C


.macro  LD32
.mparam dst,src
    LBBO    dst,src,#0x00,4
.endm

.macro  LD16
.mparam dst,src
    LBBO    dst,src,#0x00,2
.endm

.macro  LD8
.mparam dst,src
    LBBO    dst,src,#0x00,1
.endm

.macro ST32
.mparam src,dst
    SBBO    src,dst,#0x00,4
.endm

.macro ST16
.mparam src,dst
    SBBO    src,dst,#0x00,2
.endm

.macro ST8
.mparam src,dst
    SBBO    src,dst,#0x00,1
.endm

#define BUF_SIZE_BITS 18

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

    //// Configure the programmable pointer register for PRU0 by setting c31_pointer[15:0]
    //// field to 0x0010.  This will make C31 point to 0x80001000 (DDR memory).
    //MOV       r0, 0x00100000
    //MOV       r1, CTPPR_1
    //ST32      r0, r1

    MOV r0, 0           // r0 = write counter; not moduloed
    LBCO r1, C28, 8, 4  // r1 = base address for the ddr memory, which we're reading from the private memory
    LBCO r2, C28, 12, 4  // r2 = size of designated region, must be a power of 2,
                        //      passed by C code, read from the private memory
    SUB r2, r2, 4       // remove 4 bytes for the output of the write counter
                        // r3 = temporary var for computations in the loop
    LBCO r4, C28, 16, 4  // r4 = delay amount, passed by C code
    MOV r5, 0           // r5 = the relative counter; says the offset in the ddr region we're writing to next

    // Loop: 6 instructions, 30 ns per loop, 289 loops per bit, in a 115200 baud transmission
loop1:

    SBCO r0, C28, 0, 4  // write the before_write counter; its value is unincremented so the offset
			// we are about to write is the actual value of this counter

#ifdef TEST_PATTERN
    SBBO r0, r1, r5, 4  // write to the designated memory; we write the actual value of the unmoduloed counter
#else
    SBBO r31, r1, r5, 4  // write the contents of the direct io register
#endif

    ADD r0, r0, 4       // increase the value of the unmoduloed offset counter
    ADD r5, r5, 4	// increase moduloed counter

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

    // Send notification to Host for program completion
    MOV       r31.b0, PRU0_ARM_INTERRUPT+16

    // Halt the processor
    HALT
