        .code16
        .text
start:  mov $0x80, %bx                  # shrink our block (ES = PSP) so the host can allocate
        mov $0x4a, %ah
        int $0x21
        push %cs
        pop %ds
        mov %cs, %ax
        mov %ax, real_cs
        mov $0x1687, %ax
        int $0x2f
        test %ax, %ax
        jnz fail1
        mov %di, entry_off
        mov %es, entry_seg
        mov %si, %bx
        mov $0x48, %ah
        int $0x21
        jc fail2
        mov %ax, %es
        xor %ax, %ax                    # 16-bit client
        lcall *entry_off
        jc fail3
        push %ds
        pop %es
        mov %ds, %bx                    # keep the data selector
        push %cs
        pop %ds                         # DS:SI = code selector : procedure
        xor %esi, %esi
        xor %edi, %edi
        mov $cbproc, %si
        mov $rmstruct, %di
        mov $0x0303, %ax
        int $0x31
        mov %bx, %ds
        jc fail4
        mov %cx, cb_seg
        mov %dx, cb_off
        movw $rmproc, rmstruct+42       # CS:IP of the real-mode routine
        mov real_cs, %ax
        mov %ax, rmstruct+44
        mov %ax, rmstruct+36            # DS for the real-mode routine
        movw $0, rmstruct+46
        movw $0, rmstruct+48
        xor %edi, %edi
        mov $rmstruct, %di
        xor %cx, %cx
        mov $0x0301, %ax
        int $0x31
        jc fail5
        cmpw $0x1234, rmstruct+28       # marker from the callback procedure
        jne fail6
        mov $0x4c00, %ax
        int $0x21
fail1:  mov $0x4c01, %ax
        int $0x21
fail2:  mov $0x4c02, %ax
        int $0x21
fail3:  mov $0x4c03, %ax
        int $0x21
fail4:  mov $0x4c04, %ax
        int $0x21
fail5:  mov $0x4c05, %ax
        int $0x21
fail6:  mov $0x4c06, %ax
        int $0x21
rmproc: lcall *cb_off                   # real mode: call the callback stub
        lret
cbproc: mov (%si), %ax                  # protected mode: DS:SI = real-mode stack, ES:DI = structure
        mov %ax, %es:42(%di)
        mov 2(%si), %ax
        mov %ax, %es:44(%di)
        addw $4, %es:46(%di)
        movw $0x1234, %es:28(%di)
        iret
        .data
real_cs:   .word 0
entry_off: .word 0
entry_seg: .word 0
cb_off:    .word 0
cb_seg:    .word 0
rmstruct:  .space 50
