/*
 * STM32F103C8 (中容量, 64KB Flash / 20KB RAM) GNU ld 链接脚本
 * 替代 Keil 的 .sct 分散加载文件。
 */

ENTRY(Reset_Handler)

/* 栈顶 = RAM 末尾（向上生长的满递减栈） */
_estack = ORIGIN(RAM) + LENGTH(RAM);

/* 可在 make 命令行覆盖：make HEAP_SIZE=0x400 STACK_SIZE=0x800 */
_Min_Heap_Size  = DEFINED(_Min_Heap_Size)  ? _Min_Heap_Size  : 0x200;  /* 512 B  */
_Min_Stack_Size = DEFINED(_Min_Stack_Size) ? _Min_Stack_Size : 0x400;  /* 1024 B */

MEMORY
{
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 20K
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
}

SECTIONS
{
  /* 中断向量表必须位于 Flash 首地址，且不被 --gc-sections 回收 */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  /* 代码段 */
  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  /* 只读数据 */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH

  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* .data 段的初值存放在 Flash 中，运行时由启动代码拷贝到 RAM */
  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  /* 未初始化数据 */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* 检查堆栈空间是否足够（不足则链接期报错） */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* 说明：不在此丢弃 libc/libm/libgcc 中的目标文件，
     未使用的函数由 -Wl,--gc-sections 自动回收，避免误删被引用的符号。 */

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
