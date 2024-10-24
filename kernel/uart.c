//
// low-level driver routines for 16550a UART.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// the UART control registers are memory-mapped
// at address UART0. this macro returns the
// address of one of the registers.
#define Reg(reg) ((volatile unsigned char *)(UART0 + reg))

// the UART control registers.
// some have different meanings for
// read vs write.
// see http://byterunner.com/16550.html
#define RHR 0                 // receive holding register (for input bytes)
#define THR 0                 // transmit holding register (for output bytes)
#define IER 1                 // interrupt enable register
#define IER_TX_ENABLE (1<<0)
#define IER_RX_ENABLE (1<<1)
#define FCR 2                 // FIFO control register
#define FCR_FIFO_ENABLE (1<<0)
#define FCR_FIFO_CLEAR (3<<1) // clear the content of the two FIFOs
#define ISR 2                 // interrupt status register
#define LCR 3                 // line control register
#define LCR_EIGHT_BITS (3<<0)
#define LCR_BAUD_LATCH (1<<7) // special mode to set baud rate
#define LSR 5                 // line status register
#define LSR_RX_READY (1<<0)   // input is waiting to be read from RHR
#define LSR_TX_IDLE (1<<5)    // THR can accept another character to send

#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// the transmit output buffer.
struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32
char uart_tx_buf[UART_TX_BUF_SIZE];
int uart_tx_w; // write next to uart_tx_buf[uart_tx_w++]
int uart_tx_r; // read next from uart_tx_buf[uar_tx_r++]

extern volatile int panicked; // from printf.c

void uartstart();

void
uartinit(void)
{
  // disable interrupts.
  WriteReg(IER, 0x00);

  // special mode to set baud rate.
  WriteReg(LCR, LCR_BAUD_LATCH);

  // LSB for baud rate of 38.4K.
  WriteReg(0, 0x03);

  // MSB for baud rate of 38.4K.
  WriteReg(1, 0x00);

  // leave set-baud mode,
  // and set word length to 8 bits, no parity.
  WriteReg(LCR, LCR_EIGHT_BITS);

  // reset and enable FIFOs.
  WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

  // enable transmit and receive interrupts.
  WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);

  initlock(&uart_tx_lock, "uart");
}

// 将一个字符添加到输出缓冲区，并通知UART开始发送字符（如果尚未发送）。
// 如果输出缓冲区已满，则会阻塞。
// 由于可能会阻塞，不能在中断中调用，仅适合在 write() 中使用。
void uartputc(int c) {
  acquire(&uart_tx_lock); // 获取UART发送缓冲区的锁，确保互斥访问

  if(panicked) { // 如果系统处于紧急状态（panic），进入无限循环，停止执行
    for(;;)
      ;
  }

  while(1) { // 无限循环，直到成功将字符添加到缓冲区
    // 检查输出缓冲区是否已满
    // 使用环形缓冲区的公式判断是否满
    // uart_tx_w 表示写指针，uart_tx_r 表示读指针
    if(((uart_tx_w + 1) % UART_TX_BUF_SIZE) == uart_tx_r) {
      sleep(&uart_tx_r, &uart_tx_lock); // 如果缓冲区已满，等待 uartstart() 释放缓冲区空间
    } else {
      uart_tx_buf[uart_tx_w] = c; // 如果缓冲区未满，将字符 c 添加到缓冲区
      uart_tx_w = (uart_tx_w + 1) % UART_TX_BUF_SIZE; // 更新写指针，使用环形缓冲区的方式
      uartstart(); // 调用 uartstart() 开始发送数据
      release(&uart_tx_lock); // 释放锁，允许其他进程访问缓冲区
      return; // 结束函数，成功写入字符
    }
  }
}


// alternate version of uartputc() that doesn't 
// use interrupts, for use by kernel printf() and
// to echo characters. it spins waiting for the uart's
// output register to be empty.
void
uartputc_sync(int c)
{
  push_off();

  if(panicked){
    for(;;)
      ;
  }

  // wait for Transmit Holding Empty to be set in LSR.
  while((ReadReg(LSR) & LSR_TX_IDLE) == 0)
    ;
  WriteReg(THR, c);

  pop_off();
}

// if the UART is idle, and a character is waiting
// in the transmit buffer, send it.
// caller must hold uart_tx_lock.
// called from both the top- and bottom-half.
void
uartstart()
{
  while(1){ // 检查传输缓冲区是否为空
    if(uart_tx_w == uart_tx_r){ // 如果缓冲区为空，则返回，结束发送操作
      return;
    }
    
    if((ReadReg(LSR) & LSR_TX_IDLE) == 0){ // 检查UART的传输保持寄存器是否已满
      // 如果UART寄存器未空闲，说明不能再发送新的字节
      // 当UART准备好接收新字节时会产生中断
      return;
    }
    
    int c = uart_tx_buf[uart_tx_r]; // 从缓冲区读取要发送的字符
    uart_tx_r = (uart_tx_r + 1) % UART_TX_BUF_SIZE; // 更新读取指针，使用环形缓冲区的方式
    
    wakeup(&uart_tx_r); // 唤醒可能在等待缓冲区有空间的 uartputc() 调用者
    
    WriteReg(THR, c); // 将字符写入UART的传输保持寄存器，开始发送
  }
}

// 从 UART 读取一个输入字符
// 如果没有字符等待，则返回 -1
int
uartgetc(void)
{
  if(ReadReg(LSR) & 0x01){ // 检查接收线状态寄存器 (LSR) 的最低位，判断是否有输入数据准备好
    return ReadReg(RHR); // 输入数据已准备好，读取接收保持寄存器 (RHR) 中的数据并返回
  } else {
    return -1; // 没有输入数据准备好，返回 -1
  }
}

// 处理 UART 中断，触发的原因可能是输入到达，
// 或者 UART 准备好更多输出，或者两者都有
// 此函数从 trap.c 被调用
void
uartintr(void)
{
  while(1){ // 循环读取并处理接收到的字符
    int c = uartgetc(); // 调用 uartgetc 函数获取一个字符
    if(c == -1) // 如果没有字符，退出循环
      break;
    consoleintr(c); // 将接收到的字符传递给控制台中断处理程序
  }

  // 发送缓冲区中的字符
  acquire(&uart_tx_lock); // 获取 UART 发送缓冲区的锁，以保护共享资源
  uartstart(); // 调用 uartstart 函数，开始发送缓冲区中的字符
  release(&uart_tx_lock); // 释放 UART 发送缓冲区的锁
}
