#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
/* unix */
#include <unistd.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>

// memory: 65536 slots * 2 bytes
uint16_t memory[UINT16_MAX];

// registers: 10 total registers with 16 bits each
enum
{
  R_R0 = 0,
  R_R1,
  R_R2,
  R_R3,
  R_R4,
  R_R5,
  R_R6,
  R_R7,
  R_PC,   // program counter
  R_COND, // condition of last operation
  R_COUNT
};
uint16_t reg[R_COUNT];

// instruction set
enum
{
  OP_BR = 0, /* branch */
  OP_ADD,    /* add  */
  OP_LD,     /* load */
  OP_ST,     /* store */
  OP_JSR,    /* jump register */
  OP_AND,    /* bitwise and */
  OP_LDR,    /* load register */
  OP_STR,    /* store register */
  OP_RTI,    /* unused */
  OP_NOT,    /* bitwise not */
  OP_LDI,    /* load indirect */
  OP_STI,    /* store indirect */
  OP_JMP,    /* jump */
  OP_RES,    /* reserved (unused) */
  OP_LEA,    /* load effective address */
  OP_TRAP    /* execute trap */
};

// condition flags which indicate the sign of the previous calculation
enum
{
  FL_POS = 1 << 0, // P
  FL_ZRO = 1 << 1, // Z
  FL_NEG = 1 << 2  // N
};

/**
 * LC-3 Assembly Examples:
 *
 * Hello World Assembly:
 *
 * .ORIG x3000                        ; this is the address in memory where the program will be loaded
 * LEA R0, HELLO_STR                  ; load the address of the HELLO_STR string into R0
 * PUTs                               ; output the string pointed to by R0 to the console
 * HALT                               ; halt the program
 * HELLO_STR .STRINGZ "Hello World!"  ; store this string here in the program
 * .END                               ; mark the end of the file
 *
 * Loop Assembly:
 *
 * AND R0, R0, 0                      ; clear R0
 * LOOP                               ; label at the top of our loop
 * ADD R0, R0, 1                      ; add 1 to R0 and store back in R0
 * ADD R1, R0, -10                    ; subtract 10 from R0 and store back in R1
 * BRn LOOP                           ; go back to LOOP if the result was negative
 * ...                                ; R0 is now 10!
 *
 * Procedures for processing LC-3 Assembly:
 * 1. Load one instruction from memory at the address of the PC register.
 * 2. Increment the PC register.
 * 3. Look at the opcode to determine which type of instruction it should perform.
 * 4. Perform the instruction using the parameters in the instruction.
 * 5. Go back to step 1.
 *
 */

void handle_interrupt(int signal);
// setting up UNIX terminal input
struct termios original_tio;
void disable_input_buffering();
void restore_input_buffering();

uint16_t sign_extend(uint16_t x, int bit_count);

void update_flags(uint16_t r);

int main(int argc, char const *argv[])
{
  // Load Arguments
  if (argc < 2)
  {
    /* show usage string */
    printf("lc3 [image-file1] ...\n");
    exit(2);
  }

  for (int j = 1; j < argc; ++j)
  {
    if (!read_image(argv[j]))
    {
      printf("failed to load image: %s\n", argv[j]);
      exit(1);
    }
  }

  signal(SIGINT, handle_interrupt);
  disable_input_buffering();

  reg[R_COND] = FL_ZRO; // since exactly one condition flag should be set at any given time, set the Z flag

  // set the PC to starting position
  // 0x3000 is the default
  enum
  {
    PC_START = 0x3000
  };
  reg[R_PC] = PC_START;

  int running = 1;
  while (running)
  {
    // fetch instrcution from memory
    uint16_t instr = mem_read(reg[R_PC]); // TODO: mem_read
    reg[R_PC] += 1;
    uint16_t opcode = instr >> 12;

    switch (opcode)
    {
    case OP_ADD:
    {
      uint16_t r0 = (instr >> 9) & 0x7;       // get bits[9:11]
      uint16_t r1 = (instr >> 6) & 0x7;       // get bits[6:8]
      uint16_t imm_flag = (instr >> 5) & 0x1; // get bit[5]
      if (imm_flag)
      {
        uint16_t imm5 = sign_extend(instr & 0x1F, 5);
        reg[r0] = reg[r1] + imm5;
      }
      else
      {
        uint16_t r2 = instr & 0x7;
        reg[r0] = reg[r1] + reg[r2];
      }
      update_flags(r0);
    }
    break;
    case OP_AND:
    {
      uint16_t r0 = (instr >> 9) & 0x7;
      uint16_t r1 = (instr >> 6) & 0x7;
      uint16_t imm_flag = (instr >> 5) & 0x1;

      if (imm_flag)
      {
        uint16_t imm5 = sign_extend(instr & 0x1F, 5);
        reg[r0] = reg[r1] & imm5;
      }
      else
      {
        uint16_t r2 = instr & 0x7;
        reg[r0] = reg[r1] & reg[r2];
      }
      update_flags(r0);
    }
    break;
    case OP_NOT:
    {
      uint16_t r0 = (instr >> 9) & 0x7;
      uint16_t r1 = (instr >> 6) & 0x7;

      reg[r0] = ~reg[r1];
      update_flags(r0);
    }
    break;
    case OP_BR:
    {
      uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
      uint16_t cond_flag = (instr >> 9) & 0x7;
      if (cond_flag & reg[R_COND])
      {
        reg[R_PC] += pc_offset;
      }
    }
    break;
    case OP_JMP:
    {
      /* Also handles RET */
      uint16_t r1 = (instr >> 6) & 0x7;
      reg[R_PC] = reg[r1];
    }
    break;
    case OP_JSR:
    {
      uint16_t long_flag = (instr >> 11) & 1;
      reg[R_R7] = reg[R_PC];
      if (long_flag)
      {
        uint16_t long_pc_offset = sign_extend(instr & 0x7FF, 11);
        reg[R_PC] += long_pc_offset; /* JSR */
      }
      else
      {
        uint16_t r1 = (instr >> 6) & 0x7;
        reg[R_PC] = reg[r1]; /* JSRR */
      }
      break;
    }
    break;
    case OP_LD:
    {
      uint16_t r0 = (instr >> 9) & 0x7;
      uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
      reg[r0] = mem_read(reg[R_PC] + pc_offset);
      update_flags(r0);
    }
    break;
    case OP_LDI: // LDI stands for "load indirect." This instruction is used to load a value from a location in memory into a register.
    {
      uint16_t r0 = (instr >> 9) & 0x7; // get destination register
      uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
      reg[r0] = mem_read(mem_read(reg[R_PC] + pc_offset));
      update_flags(r0);
    }
    break;
    case OP_LDR:
    {
      uint16_t r0 = (instr >> 9) & 0x7;
      uint16_t r1 = (instr >> 6) & 0x7;
      uint16_t offset = sign_extend(instr & 0x3F, 6);
      reg[r0] = mem_read(reg[r1] + offset);
      update_flags(r0);
    }
    break;
    case OP_LEA:
      // TODO: OP_LEA
      break;
    case OP_ST:
      // TODO: OP_ST
      break;
    case OP_STI:
      // TODO: OP_STI
      break;
    case OP_STR:
      // TODO: OP_STR
      break;
    case OP_TRAP:
      // TODO: OP_TRAP
      break;
    case OP_RES:
    case OP_RTI:
    default:
      abort();
      break;
    }
  }
  restore_input_buffering();
}

void handle_interrupt(int signal)
{
  restore_input_buffering();
  printf("\n");
  exit(-2);
}

void disable_input_buffering()
{
  tcgetattr(STDIN_FILENO, &original_tio);
  struct termios new_tio = original_tio;
  new_tio.c_lflag &= ~ICANON & ~ECHO;
  tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_input_buffering()
{
  tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}

uint16_t sign_extend(uint16_t x, int bit_count)
{
  if ((x >> (bit_count - 1)) & 1 == 1)
  {
    x |= (0xFFFF << bit_count);
  }
  return x;
}

void update_flags(uint16_t r)
{
  if (reg[r] == 0)
  {
    reg[R_COND] = FL_ZRO;
  }
  else if (reg[r] >> 15)
  {
    reg[R_COND] = FL_NEG;
  }
  else
  {
    reg[R_COND] = FL_POS;
  }
}