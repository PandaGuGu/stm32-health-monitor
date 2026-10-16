/**
  ******************************************************************************
  * @file      startup_stm32f10x_md_gcc.s
  * @brief     GNU as (arm-none-eabi-gcc) 版 STM32F103 中容量启动文件
  *
  *  从 Keil/ARMCC 版 CORE/startup_stm32f10x_md.s 翻译而来，向量表顺序与
  *  stm32f10x_it.c 中实现的处理函数名严格一致。
  *
  *  与 Keil 版的差异：DCD -> .word, EXPORT -> .global, IMPORT -> 外部符号引用,
  *  [WEAK] -> .weak + .thumb_set 别名到 Default_Handler。
  ******************************************************************************
  */

  .syntax unified
  .cpu cortex-m3
  .fpu softvfp
  .thumb

.global g_pfnVectors
.global Default_Handler

/* 链接脚本中定义的 .data/.bss 边界符号 */
.word _sidata   /* .data 段初值在 Flash 中的起始地址 */
.word _sdata    /* .data 段在 RAM 中的起始地址 */
.word _edata    /* .data 段在 RAM 中的结束地址 */
.word _sbss     /* .bss 段起始地址 */
.word _ebss     /* .bss 段结束地址 */

/**
 * @brief  Reset_Handler 是上电后第一条执行的指令，负责拷贝初值、清零 bss、
 *         调用 __libc_init_array（C 运行时初始化）后跳入 main。
 */
  .section .text.Reset_Handler
  .weak Reset_Handler
  .type Reset_Handler, %function
Reset_Handler:
  ldr   r0, =_estack
  mov   sp, r0                    /* 设置栈指针（双保险，向量表首字已给出） */

/* 把 .data 段初值从 Flash 拷贝到 RAM */
  ldr   r0, =_sdata
  ldr   r1, =_edata
  ldr   r2, =_sidata
  movs  r3, #0
  b     LoopCopyDataInit

CopyDataInit:
  ldr   r4, [r2, r3]
  str   r4, [r0, r3]
  adds  r3, r3, #4

LoopCopyDataInit:
  adds  r4, r0, r3
  cmp   r4, r1
  bcc   CopyDataInit

/* 把 .bss 段清零 */
  ldr   r2, =_sbss
  ldr   r4, =_ebss
  movs  r3, #0
  b     LoopFillZerobss

FillZerobss:
  str   r3, [r2]
  adds  r2, r2, #4

LoopFillZerobss:
  cmp   r2, r4
  bcc   FillZerobss

/* 调用系统初始化（时钟树配置）与 C 运行时初始化，然后进入 main */
  bl    SystemInit
  bl    __libc_init_array
  bl    main

LoopForever:
  b     LoopForever

  .size Reset_Handler, .-Reset_Handler

/**
 * @brief  newlib 的 __libc_init_array() 会引用 _init / _fini。
 *         裸机 + -nostartfiles 时 crt0/crti 未参与链接，需自行提供空实现，
 *         否则报 "undefined reference to `_init'"。C 工程无需实际内容。
 */
  .section .text._init,"ax",%progbits
  .weak _init
  .type _init, %function
_init:
  bx    lr
  .size _init, .-_init

  .section .text._fini,"ax",%progbits
  .weak _fini
  .type _fini, %function
_fini:
  bx    lr
  .size _fini, .-_fini

/**
 * @brief  未实现的中断默认死循环，同时用于设置栈保护的故障处理。
 */
  .section .text.Default_Handler,"ax",%progbits
Default_Handler:
Infinite_Loop:
  b     Infinite_Loop
  .size Default_Handler, .-Default_Handler

/******************************************************************************
* 中断向量表（STM32F103 中容量，放置于 Flash 起始处）
******************************************************************************/
  .section .isr_vector,"a",%progbits
  .type g_pfnVectors, %object
g_pfnVectors:

  .word _estack                       /* 栈顶地址 */
  .word Reset_Handler                 /* 复位 */
  .word NMI_Handler                   /* NMI */
  .word HardFault_Handler             /* 硬件错误 */
  .word MemManage_Handler             /* MPU 错误 */
  .word BusFault_Handler              /* 总线错误 */
  .word UsageFault_Handler            /* 用法错误 */
  .word 0                             /* 保留 */
  .word 0                             /* 保留 */
  .word 0                             /* 保留 */
  .word 0                             /* 保留 */
  .word SVC_Handler                   /* SVCall */
  .word DebugMon_Handler              /* 调试监控 */
  .word 0                             /* 保留 */
  .word PendSV_Handler                /* PendSV */
  .word SysTick_Handler               /* 系统滴答定时器 */

  /* 外部中断（与 stm32f103 中容量中断向量号一一对应） */
  .word WWDG_IRQHandler               /* 窗口看门狗 */
  .word PVD_IRQHandler                /* 电源电压检测 */
  .word TAMPER_IRQHandler             /* 侵入检测 */
  .word RTC_IRQHandler                /* 实时时钟 */
  .word FLASH_IRQHandler              /* Flash 全局 */
  .word RCC_IRQHandler                /* 复位与时钟控制 */
  .word EXTI0_IRQHandler              /* 外部中断线 0 */
  .word EXTI1_IRQHandler              /* 外部中断线 1 */
  .word EXTI2_IRQHandler              /* 外部中断线 2 */
  .word EXTI3_IRQHandler              /* 外部中断线 3 */
  .word EXTI4_IRQHandler              /* 外部中断线 4 */
  .word DMA1_Channel1_IRQHandler      /* DMA1 通道 1 */
  .word DMA1_Channel2_IRQHandler      /* DMA1 通道 2 */
  .word DMA1_Channel3_IRQHandler      /* DMA1 通道 3 */
  .word DMA1_Channel4_IRQHandler      /* DMA1 通道 4 */
  .word DMA1_Channel5_IRQHandler      /* DMA1 通道 5 */
  .word DMA1_Channel6_IRQHandler      /* DMA1 通道 6 */
  .word DMA1_Channel7_IRQHandler      /* DMA1 通道 7 */
  .word ADC1_2_IRQHandler             /* ADC1 与 ADC2 */
  .word USB_HP_CAN1_TX_IRQHandler     /* USB 高优先级 / CAN1 发送 */
  .word USB_LP_CAN1_RX0_IRQHandler    /* USB 低优先级 / CAN1 接收 0 */
  .word CAN1_RX1_IRQHandler           /* CAN1 接收 1 */
  .word CAN1_SCE_IRQHandler           /* CAN1 状态改变 */
  .word EXTI9_5_IRQHandler            /* 外部中断线 9..5 */
  .word TIM1_BRK_IRQHandler           /* TIM1 制动 */
  .word TIM1_UP_IRQHandler            /* TIM1 更新 */
  .word TIM1_TRG_COM_IRQHandler       /* TIM1 触发与换相 */
  .word TIM1_CC_IRQHandler            /* TIM1 捕获比较 */
  .word TIM2_IRQHandler               /* TIM2 */
  .word TIM3_IRQHandler               /* TIM3 */
  .word TIM4_IRQHandler               /* TIM4 */
  .word I2C1_EV_IRQHandler            /* I2C1 事件 */
  .word I2C1_ER_IRQHandler            /* I2C1 错误 */
  .word I2C2_EV_IRQHandler            /* I2C2 事件 */
  .word I2C2_ER_IRQHandler            /* I2C2 错误 */
  .word SPI1_IRQHandler               /* SPI1 */
  .word SPI2_IRQHandler               /* SPI2 */
  .word USART1_IRQHandler             /* USART1 */
  .word USART2_IRQHandler             /* USART2 */
  .word USART3_IRQHandler             /* USART3 */
  .word EXTI15_10_IRQHandler          /* 外部中断线 15..10 */
  .word RTCAlarm_IRQHandler           /* RTC 闹钟 */
  .word USBWakeUp_IRQHandler          /* USB 唤醒 */

  .size g_pfnVectors, .-g_pfnVectors

/******************************************************************************
* 弱定义的中断处理函数：全部指向 Default_Handler，
* 用户在 stm32f10x_it.c 中实现同名函数即可覆盖。
******************************************************************************/
  .weak NMI_Handler
  .thumb_set NMI_Handler,Default_Handler
  .weak HardFault_Handler
  .thumb_set HardFault_Handler,Default_Handler
  .weak MemManage_Handler
  .thumb_set MemManage_Handler,Default_Handler
  .weak BusFault_Handler
  .thumb_set BusFault_Handler,Default_Handler
  .weak UsageFault_Handler
  .thumb_set UsageFault_Handler,Default_Handler
  .weak SVC_Handler
  .thumb_set SVC_Handler,Default_Handler
  .weak DebugMon_Handler
  .thumb_set DebugMon_Handler,Default_Handler
  .weak PendSV_Handler
  .thumb_set PendSV_Handler,Default_Handler
  .weak SysTick_Handler
  .thumb_set SysTick_Handler,Default_Handler

  .weak WWDG_IRQHandler
  .thumb_set WWDG_IRQHandler,Default_Handler
  .weak PVD_IRQHandler
  .thumb_set PVD_IRQHandler,Default_Handler
  .weak TAMPER_IRQHandler
  .thumb_set TAMPER_IRQHandler,Default_Handler
  .weak RTC_IRQHandler
  .thumb_set RTC_IRQHandler,Default_Handler
  .weak FLASH_IRQHandler
  .thumb_set FLASH_IRQHandler,Default_Handler
  .weak RCC_IRQHandler
  .thumb_set RCC_IRQHandler,Default_Handler
  .weak EXTI0_IRQHandler
  .thumb_set EXTI0_IRQHandler,Default_Handler
  .weak EXTI1_IRQHandler
  .thumb_set EXTI1_IRQHandler,Default_Handler
  .weak EXTI2_IRQHandler
  .thumb_set EXTI2_IRQHandler,Default_Handler
  .weak EXTI3_IRQHandler
  .thumb_set EXTI3_IRQHandler,Default_Handler
  .weak EXTI4_IRQHandler
  .thumb_set EXTI4_IRQHandler,Default_Handler
  .weak DMA1_Channel1_IRQHandler
  .thumb_set DMA1_Channel1_IRQHandler,Default_Handler
  .weak DMA1_Channel2_IRQHandler
  .thumb_set DMA1_Channel2_IRQHandler,Default_Handler
  .weak DMA1_Channel3_IRQHandler
  .thumb_set DMA1_Channel3_IRQHandler,Default_Handler
  .weak DMA1_Channel4_IRQHandler
  .thumb_set DMA1_Channel4_IRQHandler,Default_Handler
  .weak DMA1_Channel5_IRQHandler
  .thumb_set DMA1_Channel5_IRQHandler,Default_Handler
  .weak DMA1_Channel6_IRQHandler
  .thumb_set DMA1_Channel6_IRQHandler,Default_Handler
  .weak DMA1_Channel7_IRQHandler
  .thumb_set DMA1_Channel7_IRQHandler,Default_Handler
  .weak ADC1_2_IRQHandler
  .thumb_set ADC1_2_IRQHandler,Default_Handler
  .weak USB_HP_CAN1_TX_IRQHandler
  .thumb_set USB_HP_CAN1_TX_IRQHandler,Default_Handler
  .weak USB_LP_CAN1_RX0_IRQHandler
  .thumb_set USB_LP_CAN1_RX0_IRQHandler,Default_Handler
  .weak CAN1_RX1_IRQHandler
  .thumb_set CAN1_RX1_IRQHandler,Default_Handler
  .weak CAN1_SCE_IRQHandler
  .thumb_set CAN1_SCE_IRQHandler,Default_Handler
  .weak EXTI9_5_IRQHandler
  .thumb_set EXTI9_5_IRQHandler,Default_Handler
  .weak TIM1_BRK_IRQHandler
  .thumb_set TIM1_BRK_IRQHandler,Default_Handler
  .weak TIM1_UP_IRQHandler
  .thumb_set TIM1_UP_IRQHandler,Default_Handler
  .weak TIM1_TRG_COM_IRQHandler
  .thumb_set TIM1_TRG_COM_IRQHandler,Default_Handler
  .weak TIM1_CC_IRQHandler
  .thumb_set TIM1_CC_IRQHandler,Default_Handler
  .weak TIM2_IRQHandler
  .thumb_set TIM2_IRQHandler,Default_Handler
  .weak TIM3_IRQHandler
  .thumb_set TIM3_IRQHandler,Default_Handler
  .weak TIM4_IRQHandler
  .thumb_set TIM4_IRQHandler,Default_Handler
  .weak I2C1_EV_IRQHandler
  .thumb_set I2C1_EV_IRQHandler,Default_Handler
  .weak I2C1_ER_IRQHandler
  .thumb_set I2C1_ER_IRQHandler,Default_Handler
  .weak I2C2_EV_IRQHandler
  .thumb_set I2C2_EV_IRQHandler,Default_Handler
  .weak I2C2_ER_IRQHandler
  .thumb_set I2C2_ER_IRQHandler,Default_Handler
  .weak SPI1_IRQHandler
  .thumb_set SPI1_IRQHandler,Default_Handler
  .weak SPI2_IRQHandler
  .thumb_set SPI2_IRQHandler,Default_Handler
  .weak USART1_IRQHandler
  .thumb_set USART1_IRQHandler,Default_Handler
  .weak USART2_IRQHandler
  .thumb_set USART2_IRQHandler,Default_Handler
  .weak USART3_IRQHandler
  .thumb_set USART3_IRQHandler,Default_Handler
  .weak EXTI15_10_IRQHandler
  .thumb_set EXTI15_10_IRQHandler,Default_Handler
  .weak RTCAlarm_IRQHandler
  .thumb_set RTCAlarm_IRQHandler,Default_Handler
  .weak USBWakeUp_IRQHandler
  .thumb_set USBWakeUp_IRQHandler,Default_Handler
