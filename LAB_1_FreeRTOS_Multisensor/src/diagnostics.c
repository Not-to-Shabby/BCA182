#include "diagnostics.h"
#include "stm32f1xx_hal.h"

void diag_early_init(void) {
    // Enable SHCSR fault handlers (MemManage, BusFault, UsageFault)
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk |
                  SCB_SHCSR_BUSFAULTENA_Msk |
                  SCB_SHCSR_USGFAULTENA_Msk;
}

void diag_putc(char c) {
    // Write directly to USART1 data register without interrupts or HAL state
    while (!(USART1->SR & USART_SR_TXE)) {
    }
    USART1->DR = (uint8_t)c;
}

void diag_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            diag_putc('\r');
        }
        diag_putc(*s++);
    }
}

void diag_put_hex(uint32_t val) {
    const char hex_chars[] = "0123456789ABCDEF";
    diag_puts("0x");
    for (int i = 7; i >= 0; i--) {
        diag_putc(hex_chars[(val >> (i * 4)) & 0x0F]);
    }
}

// Low-level fault trap with register dump
void diag_dump_fault(const char *fault_name, const uint32_t *stacked_regs) {
    diag_puts("\r\n================ [FAULT DETECTED] ================\r\n");
    diag_puts(fault_name);
    diag_puts("\r\nStacked Registers:\r\n");
    diag_puts(" R0:  "); diag_put_hex(stacked_regs[0]); diag_puts("\r\n");
    diag_puts(" R1:  "); diag_put_hex(stacked_regs[1]); diag_puts("\r\n");
    diag_puts(" R2:  "); diag_put_hex(stacked_regs[2]); diag_puts("\r\n");
    diag_puts(" R3:  "); diag_put_hex(stacked_regs[3]); diag_puts("\r\n");
    diag_puts(" R12: "); diag_put_hex(stacked_regs[4]); diag_puts("\r\n");
    diag_puts(" LR:  "); diag_put_hex(stacked_regs[5]); diag_puts("\r\n");
    diag_puts(" PC:  "); diag_put_hex(stacked_regs[6]); diag_puts("\r\n");
    diag_puts(" PSR: "); diag_put_hex(stacked_regs[7]); diag_puts("\r\n");

    diag_puts("SCB Fault Registers:\r\n");
    diag_puts(" CFSR:  "); diag_put_hex(SCB->CFSR); diag_puts("\r\n");
    diag_puts(" HFSR:  "); diag_put_hex(SCB->HFSR); diag_puts("\r\n");
    diag_puts(" MMFAR: "); diag_put_hex(SCB->MMFAR); diag_puts("\r\n");
    diag_puts(" BFAR:  "); diag_put_hex(SCB->BFAR); diag_puts("\r\n");
    diag_puts("==================================================\r\n");

    while (1) {
        // Fast-blink PC13 LED to indicate fault state
        GPIOC->ODR ^= GPIO_PIN_13;
        for (volatile int i = 0; i < 200000; i++) __NOP();
    }
}

// Naked assembly trap to preserve MSP/PSP
__attribute__((naked)) void HardFault_Handler(void) {
    __asm volatile(
        " tst lr, #4 \n"
        " ite eq \n"
        " mrseq r1, msp \n"
        " mrsne r1, psp \n"
        " ldr r0, =fault_msg \n"
        " b diag_dump_fault \n"
        " fault_msg: .asciz \"HardFault\"\n"
    );
}

__attribute__((naked)) void MemManage_Handler(void) {
    __asm volatile(
        " tst lr, #4 \n"
        " ite eq \n"
        " mrseq r1, msp \n"
        " mrsne r1, psp \n"
        " ldr r0, =mm_msg \n"
        " b diag_dump_fault \n"
        " mm_msg: .asciz \"MemManage_Fault\"\n"
    );
}

__attribute__((naked)) void BusFault_Handler(void) {
    __asm volatile(
        " tst lr, #4 \n"
        " ite eq \n"
        " mrseq r1, msp \n"
        " mrsne r1, psp \n"
        " ldr r0, =bf_msg \n"
        " b diag_dump_fault \n"
        " bf_msg: .asciz \"BusFault\"\n"
    );
}

__attribute__((naked)) void UsageFault_Handler(void) {
    __asm volatile(
        " tst lr, #4 \n"
        " ite eq \n"
        " mrseq r1, msp \n"
        " mrsne r1, psp \n"
        " ldr r0, =uf_msg \n"
        " b diag_dump_fault \n"
        " uf_msg: .asciz \"UsageFault\"\n"
    );
}
