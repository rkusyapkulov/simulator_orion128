#ifndef CPU_8080_H
#define CPU_8080_H

#include <stdint.h>
#if defined(__cplusplus)
    /* в C++ bool встроен */
#elif defined(_MSC_VER) && _MSC_VER < 1800
    /* VS2010/2012 не имеют <stdbool.h> (появился в VS2013) */
    typedef unsigned char bool;
    #define true 1
    #define false 0
#else
    #include <stdbool.h>
#endif

#pragma pack(push, 1)

// --- СТРУКТУРЫ ТАЙМЕРА КР580ВИ53 ---
typedef struct {
    uint16_t count;         // текущее значение (в BCD-режиме хранится как обычное число 0..9999)
    uint16_t latch;         // защёлкнутое значение для чтения
    uint16_t reload;        // значение перезагрузки (режимы 2 и 3)
    uint8_t mode;           // режим 0..5
    uint8_t bcd;            // 1 - двоично-десятичный счёт
    uint8_t rw_mode;        // 1 - только LSB, 2 - только MSB, 3 - LSB затем MSB
    uint8_t access_phase;   // фаза записи счётчика (LSB/MSB)
    uint8_t read_phase;     // фаза чтения счётчика (LSB/MSB)
    uint8_t wr_lo;          // принятый, но ещё не применённый младший байт
    uint8_t state;          // служебное состояние (режим 4: 1 - строб выдан, 2 - завершён)
    bool latched;
    bool armed;             // счётчик загружен и считает
    bool out;               // выход OUT
    double phase;
} PITChannel;

// --- СТРУКТУРА ПРОЦЕССОРА INTEL 8080 (КР580ВМ80А) ---
typedef struct {
    uint8_t A, B, C, D, E, H, L;            // Основные регистры
    uint16_t PC, SP;                        // Регистр счетчика команд и указатель стека
    
    // Флаги процессора
    bool S, Z, AC, P, CY;                   // Sign, Zero, Auxiliary Carry, Parity, Carry
    bool EI;                                // Разрешение прерываний
    bool halted;                            // Состояние останова процессора (HLT)
    
    int cycles_until_interrupt;             // Счетчик тактов до генерации прерывания
    bool int_pending;                       // Флаг отложенного аппаратного прерывания
    
    // Порты КР580ВВ55 (ППА)
    uint8_t ppi0_pa, ppi0_pb, ppi0_pc, ppi0_ctrl; // 0xF400 (Клавиатура РК-86)
    uint8_t ppi1_pa, ppi1_pb, ppi1_pc, ppi1_ctrl; // 0xF500 (Интерфейс ROM-диска)
    uint8_t ppi2_pa, ppi2_pb, ppi2_pc, ppi2_ctrl; // 0xF600 (Пользовательский порт №2)
    
    // Состояние КР580ВИ53 (Таймер/Звук)
    PITChannel pit_channels[3];             // Три независимых канала таймера
    uint8_t pit_control;                    // Управляющее слово таймера
} CPU8080;
#pragma pack(pop)

#endif // CPU_8080_H
