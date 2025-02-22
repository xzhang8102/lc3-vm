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
 * Trap Code
 *
 * Note:
 * This is why programs start at address 0x3000 instead of 0x0.
 * The lower addresses are left empty to leave space for the trap routine code.
 * (In the official LC-3 simulator, trap routines are written in assembly.)
 */
enum
{
  TRAP_GETC = 0x20,  /* get character from keyboard, not echoed onto the terminal */
  TRAP_OUT = 0x21,   /* output a character */
  TRAP_PUTS = 0x22,  /* output a word string */
  TRAP_IN = 0x23,    /* get character from keyboard, echoed onto the terminal */
  TRAP_PUTSP = 0x24, /* output a byte string */
  TRAP_HALT = 0x25   /* halt the program */
};

/**
 * memory mapped registers
 *
 */
enum
{
  MR_KBSR = 0xFE00, /* keyboard status */
  MR_KBDR = 0xFE02  /* keyboard data */
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

int read_image(const char *image_path);

void mem_write(uint16_t address, uint16_t val);
uint16_t mem_read(uint16_t address);

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
    uint16_t instr = mem_read(reg[R_PC]);
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
    {
      uint16_t r0 = (instr >> 9) & 0x7;
      uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
      reg[r0] = reg[R_PC] + pc_offset;
      update_flags(r0);
    }
    break;
    case OP_ST:
    {
      uint16_t r0 = (instr >> 9) & 0x7;
      uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
      mem_write(reg[R_PC] + pc_offset, reg[r0]);
    }
    break;
    case OP_STI:
    {
      uint16_t r0 = (instr >> 9) & 0x7;
      uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
      mem_write(mem_read(reg[R_PC] + pc_offset), reg[r0]);
    }
    break;
    case OP_STR:
    {
      uint16_t r0 = (instr >> 9) & 0x7;
      uint16_t r1 = (instr >> 6) & 0x7;
      uint16_t offset = sign_extend(instr & 0x3F, 6);
      mem_write(reg[r1] + offset, reg[r0]);
    }
    break;
    case OP_TRAP:
      switch (instr & 0xFF)
      {
      case TRAP_GETC:
      {
        reg[R_R0] = (uint16_t)getchar();
        update_flags(R_R0);
      }
      break;
      case TRAP_OUT:
      {
        putc((char)reg[R_R0], stdout);
        fflush(stdout);
      }
      break;
      case TRAP_PUTS:
      {
        uint16_t *c = memory + reg[R_R0];
        while (*c)
        {
          putc((char)*c, stdout);
          ++c;
        }
        fflush(stdout);
      }
      break;
      case TRAP_IN:
      {
        printf("Enter a charater: ");
        char c = getchar();
        putc(c, stdout);
        fflush(stdout);
        reg[R_R0] = (uint16_t)c;
        update_flags(R_R0);
      }
      break;
      case TRAP_PUTSP:
      {
        /**
         * one char per byte (two bytes per word)
         * here we need to swap back to big endian format
         */
        uint16_t *c = memory + reg[R_R0];
        while (*c)
        {
          char char1 = (*c) & 0xFF;
          putc(char1, stdout);
          char char2 = (*c) >> 8;
          if (char2)
            putc(char2, stdout);
          ++c;
        }
        fflush(stdout);
      }
      break;
      case TRAP_HALT:
      {
        puts("HALT");
        fflush(stdout);
        running = 0;
      }
      break;
      }
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
  if (((x >> (bit_count - 1)) & 1) == 1)
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

uint16_t swap16(uint16_t x)
{
  return (x << 8) | (x >> 8);
}

void read_image_file(FILE *file)
{
  /* the origin tells us where in memory to place the image */
  uint16_t origin;
  fread(&origin, sizeof(origin), 1, file);
  origin = swap16(origin);

  uint16_t max_read = UINT16_MAX - origin;
  uint16_t *p = memory + origin;
  size_t read = fread(p, sizeof(uint16_t), max_read, file);

  while (read-- > 0)
  {
    *p = swap16(*p);
    ++p;
  }
}

int read_image(const char *image_path)
{
  FILE *file = fopen(image_path, "rb");
  if (!file)
  {
    return 0;
  };
  read_image_file(file);
  fclose(file);
  return 1;
}

uint16_t check_key()
{
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);

  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 0;
  return select(1, &readfds, NULL, NULL, &timeout) != 0;
}

void mem_write(uint16_t address, uint16_t val)
{
  memory[address] = val;
}

uint16_t mem_read(uint16_t address)
{
  if (address == MR_KBSR)
  {
    if (check_key())
    {
      memory[MR_KBSR] = (1 << 15);
      memory[MR_KBDR] = getchar();
    }
    else
    {
      memory[MR_KBSR] = 0;
    }
  }
  return memory[address];
}