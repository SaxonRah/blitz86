/* B86_PM_UNIFIED_V5: descriptor-backed 80386 protected system registers.
 * Shared 16/32-bit CPL0 decode; no real-mode/JIT changes or static buffers.
 * Uses the existing GDT/LDT descriptor validator and PM6 ModRM decoder.
 * Returns -1 for opcodes belonging to older PM stages; recognized-but-
 * unsupported machine operations return B86_UNSUPPORTED without mutation.
 * Deliberately NO paging, task switching, VM86, rings >0 or CR3 emulation.
 */
#ifndef B86_PM25_FLASH
#define B86_PM25_FLASH __attribute__((noinline,cold,section(".text.bb386_flash")))
#endif
#define B86_PM29_EXIT_MARKER 0xA6u /* old PM CS cache until far JMP */
static B86_PM25_FLASH int b86_pm25_step(B86Cpu *c)
{
    uint32_t start,ip,lin=0,val=0,base=0,limit=0;
    unsigned code32,op32,ad32,seg=B86_DS,segprefix=0,p66=0,p67=0;
    unsigned sub,kind=0,n=0,access=0,reg=0;
    B86PM6EA ea;
    B86PM2Fault fault;
    uint8_t op,ext,modrm;
    if(!c || !c->mem || c->model!=B86_CPU_386 || !(c->msw&1u) ||
       c->shutdown386 || c->pm386.seg[B86_CS].reserved==B86_PM3_TRANSITION_MARKER ||
       !c->pm386.seg[B86_CS].valid || !(c->pm386.seg[B86_CS].access&8u) ||
       (c->seg[B86_CS]&3u) || c->lz_kind!=LZ_NONE || c->lz_ikind!=LZ_NONE)
        return -1;
    code32=!!(c->pm386.seg[B86_CS].flags&4u);
    if(!code32 && c->ip>0xFFFFu)return -1;
    start=ip=c->ip;
    for(;;){
        if(!b86_pm4_byte(c,&ip,start,&op))return B86_UNSUPPORTED;
        if(op==0x66u){if(p66)return B86_UNSUPPORTED;p66=1;continue;}
        if(op==0x67u){if(p67)return B86_UNSUPPORTED;p67=1;continue;}
        if(op==0x26u||op==0x2Eu||op==0x36u||op==0x3Eu||op==0x64u||op==0x65u){
            if(segprefix)return B86_UNSUPPORTED;
            segprefix=1;
            seg=op==0x26u?B86_ES:op==0x2Eu?B86_CS:op==0x36u?B86_SS:
                op==0x3Eu?B86_DS:op==0x64u?B86_FS:B86_GS;
            continue;
        }
        if(op==0xF0u || op==0xF2u || op==0xF3u)return -1;
        break;
    }
    op32=code32^p66;ad32=code32^p67;
    if(op==0x63u){ /* ARPL r/m16,r16; ZF records whether RPL changed */
        uint32_t dstlin=0,old,newr;
        if(p66)return B86_UNSUPPORTED;
        if(!b86_pm6_ea(c,&ip,start,(int)ad32,&ea))return B86_UNSUPPORTED;
        if(segprefix && ea.is_reg)return B86_UNSUPPORTED;
        if(segprefix)ea.seg=seg;
        if(!ea.is_reg && b86_pm2_check_access(c,ea.seg,ea.off,2u,
                B86_PM1_READ|B86_PM1_WRITE,&dstlin,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        if(ea.is_reg)old=b86_pm6_reg(c,ea.rm,2u);
        else old=(uint32_t)*b86_host(c,dstlin)|
                 ((uint32_t)*b86_host(c,dstlin+1u)<<8u);
        newr=b86_pm6_reg(c,ea.reg,2u)&3u;
        if((old&3u)<newr){
            old=(old&~3u)|newr;
            if(ea.is_reg)b86_pm6_putreg(c,ea.rm,2u,old);
            else{
                *b86_host(c,dstlin)=(uint8_t)old;
                *b86_host(c,dstlin+1u)=(uint8_t)(old>>8u);
                smc(c,dstlin,2u);
            }
            c->flags|=B86_ZF;
        }else c->flags&=~(uint32_t)B86_ZF;
        c->ip=code32?ip:(ip&0xFFFFu);++c->icount;return B86_OK;
    }
    if(op!=0x0Fu)return -1;
    if(!b86_pm4_byte(c,&ip,start,&ext))return B86_UNSUPPORTED;
    if(ext==0x06u){ /* CLTS: CR0.TS is MSW bit 3 */
        if(p66||p67||segprefix)return B86_UNSUPPORTED;
        c->msw=(uint16_t)(c->msw&~8u);
        c->ip=code32?ip:(ip&0xFFFFu);++c->icount;return B86_OK;
    }
    if(ext!=0x00u && ext!=0x01u && ext!=0x20u && ext!=0x22u)return -1;
    if(ext==0x20u || ext==0x22u){
        if(!b86_pm4_byte(c,&ip,start,&modrm))return B86_UNSUPPORTED;
        /* MOV reg,CR0 / MOV CR0,reg: CR0.PG and all other unsupported
         * machine control fields are never guessed. 386 CR moves are 32b. */
        if(p66 || p67 || segprefix || (modrm&0xF8u)!=0xC0u)
            return B86_UNSUPPORTED;
        reg=modrm&7u;
        if(ext==0x20u)c->r[reg]=(uint32_t)c->msw;
        else{
            val=c->r[reg];
            /* B86_PM_UNIFIED_V9: allow CR0.PE clear only for a checked
             * far-JMP exit. Never invent paging or other CR0 bits. */
            if(val&~0xFu)return B86_UNSUPPORTED;
            if(!(val&1u)){
                if(c->pm386.seg[B86_CS].reserved ||
                   (c->flags&(0x20000u|0x4000u)))return B86_UNSUPPORTED;
                c->pm386.seg[B86_CS].reserved=B86_PM29_EXIT_MARKER;
            }
            c->msw=(uint16_t)val;
        }
        c->ip=code32?ip:(ip&0xFFFFu);++c->icount;return B86_OK;
    }
    if(!b86_pm4_byte(c,&ip,start,&modrm))return B86_UNSUPPORTED;
    sub=(modrm>>3u)&7u;
    /* Keep 0F00/0F01 illegal encodings fail-closed, never enter RM code. */
    if(ext==0x00u){
        if(sub!=0u && sub!=2u && sub!=4u && sub!=5u)return -1;
        kind=sub==0u?1u:sub==2u?2u:sub==4u?3u:4u;
        n=2u;
    }else{
        if(sub==0u||sub==1u||sub==2u||sub==3u)
            {kind=sub+5u;n=6u;}
        else if(sub==4u||sub==6u)
            {kind=sub==4u?9u:10u;n=2u;}
        else return -1;
    }
    /* ModRM helper expects ip positioned at the ModRM byte. */
    ip--;
    if(!b86_pm6_ea(c,&ip,start,(int)ad32,&ea))return B86_UNSUPPORTED;
    if(segprefix && ea.is_reg)return B86_UNSUPPORTED;
    if(segprefix)ea.seg=seg;
    if(ea.is_reg){
        if(n==6u)return B86_UNSUPPORTED;
        val=b86_pm6_reg(c,ea.rm,2u);
    }else{
        access=(kind==1u||kind==5u||kind==6u||kind==9u)?B86_PM1_WRITE:B86_PM1_READ;
        if(b86_pm2_check_access(c,ea.seg,ea.off,n,access,&lin,&fault)!=B86_PM2_OK)
            return B86_UNSUPPORTED;
        if(n==2u && access==B86_PM1_READ)
            val=(uint32_t)*b86_host(c,lin)|((uint32_t)*b86_host(c,lin+1u)<<8u);
    }
    if(kind==1u){ /* SLDT r/m16 */
        val=c->pm386.ldtr_valid?c->pm386.ldtr_selector:0u;
    }else if(kind==2u){ /* LLDT r/m16 */
        if(b86_pm1_prepare_ldtr(c,(uint16_t)val)!=B86_PM1_OK)return B86_UNSUPPORTED;
    }else if(kind==3u || kind==4u){ /* VERR / VERW: ZF only */
        B86PM0Desc d; int ok=0;
        if(b86_pm1_descriptor(c,(uint16_t)val,&d)==B86_PM1_OK && d.code_data &&
           d.present && d.dpl>=((uint16_t)val&3u)){
            ok=kind==3u?(!d.executable || d.read_write):(!d.executable && d.read_write);
        }
        c->flags=(c->flags&~(uint32_t)B86_ZF)|(ok?B86_ZF:0u);
    }else if(kind==5u||kind==6u){ /* SGDT / SIDT m16&32 */
        limit=kind==5u?c->gdt_limit:c->idt_limit;
        base=kind==5u?c->gdt_base:c->idt_base;
        *b86_host(c,lin)=(uint8_t)limit;
        *b86_host(c,lin+1u)=(uint8_t)(limit>>8u);
        for(unsigned i=0;i<4u;++i)
            *b86_host(c,lin+2u+i)=(uint8_t)(base>>(8u*i));
        smc(c,lin,6u);
    }else if(kind==7u||kind==8u){ /* LGDT / LIDT m16&32 */
        limit=(uint32_t)*b86_host(c,lin)|((uint32_t)*b86_host(c,lin+1u)<<8u);
        for(unsigned i=0;i<4u;++i)
            base|=(uint32_t)*b86_host(c,lin+2u+i)<<(8u*i);
        if(!op32)base&=0xFFFFFFu; /* 16-bit operand size on a 386 */
        if((uint64_t)base+(uint64_t)limit>=B86_RAM_BYTES)
            return B86_UNSUPPORTED;
        if(kind==7u){c->gdt_base=base;c->gdt_limit=(uint16_t)limit;}
        else{c->idt_base=base;c->idt_limit=(uint16_t)limit;}
    }else if(kind==9u){ /* SMSW r/m16 */
        val=c->msw;
    }else if(kind==10u){ /* LMSW r/m16 (PE cannot clear once set) */
        c->msw=(uint16_t)((c->msw&~0xEu)|(val&0xEu));
    }
    if(kind==1u||kind==9u){
        if(ea.is_reg){
            if(op32)c->r[ea.rm]=val; /* 386 r32 destination zero-extends */
            else b86_pm6_putreg(c,ea.rm,2u,val);
        }
        else{
            *b86_host(c,lin)=(uint8_t)val;
            *b86_host(c,lin+1u)=(uint8_t)(val>>8u);
            smc(c,lin,2u);
        }
    }
    c->ip=code32?ip:(ip&0xFFFFu);
    ++c->icount;
    return B86_OK;
}
