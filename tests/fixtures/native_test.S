// The contract test builds a tiny ELF64 around these instructions, without an NDK dependency.
// A real Android NDK toolchain can assemble the same guest functions independently.
.text
.global sample_add
sample_add:
    add x0, x0, x1
    ret

constructor:
    stp x29, x30, [sp, #-16]!
    ldr x16, host_ping_slot
    blr x16
    ldp x29, x30, [sp], #16
    ret

host_ping_slot:
    .quad host_ping
