#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <mmsystem.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cwchar>
#include <cmath>
#include <commdlg.h>

#pragma comment(lib, "winmm.lib")

// --- ПАРАМЕТРЫ ЭКРАНА ОРИОН-128 ---
const int ORION_SCREEN_WIDTH = 384;
const int ORION_SCREEN_HEIGHT = 256;

#define IDM_FILE_OPEN 0x0010
#define IDM_FILE_EXIT 0x0020

// --- ГЛОБАЛЬНЫЕ БУФЕРЫ ПАМЯТИ ---
// 256 Кб ОЗУ в виде 4-х банков по 64 Кб
BYTE orion_ram[4][65536];
// Состояние шин клавиатуры Радио-86РК (Индекс = Столбец PA, Биты = Строки PB)
BYTE key_matrix_state[8];
bool emulator_running = true;

// --- ПОДДЕРЖКА ЗВУКОВОГО КЛИКЕРА ОРИОНА ---
bool last_speaker_bit_state = false;

// --- ВИРТУАЛЬНЫЙ РОМ-ДИСК 64 КБ ---
BYTE rom_disk_storage[65536]; // Буфер для хранения файла "rom.bin"
static DWORD pixel_buffer[384 * 256];

// --- СИСТЕМНЫЕ ПОРТЫ ОРИОН-128 ---
BYTE orion_page = 0;         // Номер текущего банка памяти (Порт 0xF900)
BYTE orion_sys_port1 = 0;    // Управление графическим/цветным режимом (Порт 0xF800)
BYTE orion_sys_port3 = 0;    // Переключение экранной области памяти (Порт 0xFA00)

// Состояния модификаторов клавиатуры РК
bool rk_cc_pressed = false;       // CC (Shift) -> PC5
bool rk_us_pressed = false;       // УС (Ctrl) -> PC6
bool rk_rus_lat_pressed = false;  // РУС/ЛАТ -> PC7

// --- ЭМУЛЯЦИЯ БИПЕРА INTE ---
bool rk_inte_speaker_state = false; // Текущее состояние динамика от INTE

HWND hRegListBox = NULL;

// --- ПАРАМЕТРЫ ЗВУКОВОГО ПОТОКА КР580ВИ53 ---
const int AUDIO_SAMPLE_RATE = 22050;
HWAVEOUT hWaveOut = NULL;
WAVEHDR waveHeader[2];
short* audioBuffers[2] = { NULL, NULL };
const int AUDIO_BUF_SIZE = 1024;

// --- Кольцевой буфер синхронизации INTE ---
const int INTE_RING_BUF_SIZE = 16384;
short inte_ring_buffer[INTE_RING_BUF_SIZE] = { 0 };
volatile int inte_ring_write_ptr = 0;
volatile int inte_ring_read_ptr = 0;

#pragma pack(push, 1)
struct CPU8080 {
	BYTE A, B, C, D, E, H, L;
	WORD PC, SP;
	bool S, Z, AC, P, CY;
	bool EI;
	bool halted; // <-- ДОБАВИТЬ: флаг останова процессора
	int cycles_until_interrupt = 50000; // 2500000 Гц / 50 Гц = 50000 тактов на кадр
	bool int_pending = false;           // Флаг того, что прерывание пришло от дисплея

	// Три порта КР580ВВ55 (ППА)
	BYTE ppi0_pa, ppi0_pb, ppi0_pc, ppi0_ctrl; // 0F400H (Клавиатура РК)
	BYTE ppi1_pa, ppi1_pb, ppi1_pc, ppi1_ctrl; // 0F500H (Пользователь №1)
	BYTE ppi2_pa, ppi2_pb, ppi2_pc, ppi2_ctrl; // 0F600H (Пользователь №2)

	// Состояние КР580ВИ53 (Таймер/Звук по адресам 0F730H-0F733H)
	struct PITChannel {
		WORD count;
		WORD latch;
		BYTE mode;
		BYTE bcd;
		BYTE access_phase;
		bool latched;
		double phase;
	} pit_channels[3];
	BYTE pit_control;

	void Reset() {
		PC = 0xF800; // Старт из ПЗУ МОНИТОР (находится на верхней странице)
		SP = 0x0000;
		A = B = C = D = E = H = L = 0;
		S = Z = AC = P = CY = false;
		EI = false;
		halted = false; // <-- ДОБАВИТЬ: сброс флага при перезагрузке
		ppi0_pa = ppi0_pb = ppi0_pc = 0xFF; ppi0_ctrl = 0x9B;
		ppi1_pa = ppi1_pb = ppi1_pc = 0xFF; ppi1_ctrl = 0x9B;
		ppi2_pa = ppi2_pb = ppi2_pc = 0xFF; ppi2_ctrl = 0x9B;
		orion_page = 0;
		orion_sys_port1 = 0;
		orion_sys_port3 = 0;
		rk_inte_speaker_state = false;
		pit_control = 0x00;
		for (int i = 0; i < 3; i++) {
			pit_channels[i].count = 0; pit_channels[i].latch = 0;
			pit_channels[i].mode = 0; pit_channels[i].bcd = 0;
			pit_channels[i].access_phase = 0;
			pit_channels[i].latched = false;
			pit_channels[i].phase = 0.0;
		}
	}

	void SetFlagsZSP(BYTE res) {
		Z = (res == 0);
		S = ((res & 0x80) != 0);
		BYTE Ty = res;
		Ty ^= Ty >> 4; Ty ^= Ty >> 2; Ty ^= Ty >> 1;
		P = !(Ty & 1);
	}

	void SetFlagsINR(BYTE old_val, BYTE new_val) {
		AC = ((old_val & 0x0F) + 1 > 0x0F);
		SetFlagsZSP(new_val);
	}

	void SetFlagsDCR(BYTE old_val, BYTE new_val) {
		AC = ((old_val & 0x0F) == 0);
		SetFlagsZSP(new_val);
	}

	void SetFlagsAdd(BYTE r1, BYTE r2, int res) {
		CY = (res > 0xFF);
		AC = (((r1 & 0xF) + (r2 & 0xF)) > 0xF);
		SetFlagsZSP((BYTE)res);
	}

	void SetFlagsSub(BYTE r1, BYTE r2, int res) {
		CY = (res < 0);
		AC = (((r1 & 0xF) - (r2 & 0xF)) < 0);
		SetFlagsZSP((BYTE)res);
	}

	WORD GetHL() const { return (H << 8) | L; }
	void SetHL(WORD val) { H = val >> 8; L = val & 0xFF; }
	WORD GetBC() const { return (B << 8) | C; }
	void SetBC(WORD val) { B = val >> 8; C = val & 0xFF; }
	WORD GetDE() const { return (D << 8) | E; }
	void SetDE(WORD val) { D = val >> 8; E = val & 0xFF; }

	BYTE GetReg(int r) {
		switch (r) {
		case 0: return B; case 1: return C; case 2: return D; case 3: return E;
		case 4: return H; case 5: return L; case 6: return Read(GetHL()); default: return A;
		}
	}

	void SetReg(int r, BYTE val) {
		switch (r) {
		case 0: B = val; break; case 1: C = val; break; case 2: D = val; break; case 3: E = val; break;
		case 4: H = val; break; case 5: L = val; break; case 6: Write(GetHL(), val); break; default: A = val; break;
		}
	}

	// =========================================================================
	// ИСПРАВЛЕННЫЙ АППАРАТНЫЙ ДИСПЕТЧЕР ЧТЕНИЯ ПАМЯТИ И ПОРТОВ (ОРИОН-128)
	// =========================================================================
	BYTE Read(WORD addr) {
		// 1. Зона системного Монитора / BIOS (0xF800 - 0xFFFF)
		// Код Монитора должен исполняться ИЗ БАНКА 0 ВСЕГДА, из любого банка ОЗУ!
		if (addr >= 0xF800) {
			return orion_ram[0][addr];
		}

		// 2. Дешифрация аппаратных портов ввода-вывода (0xF400 - 0xF7FF)
		// Доступны только если мы не в режиме "чистого сплошного ОЗУ" (по умолчанию в Банках 0 и 1 они видны)
		int current_bank = orion_page & 3;
		if ((current_bank == 0 || current_bank == 1) && addr >= 0xF400 && addr <= 0xF7FF) {
			// К580ВВ55 №1: Порт клавиатуры (0xF400 - 0xF403)
			if (addr <= 0xF403) {
				WORD reg = addr & 3;
				if (reg == 0) return ppi0_pa;
				if (reg == 1) {
					BYTE result_pb = 0xFF;
					for (int col = 0; col < 8; col++) {
						if ((ppi0_pa & (1 << col)) == 0) {
							result_pb &= key_matrix_state[col];
						}
					}
					return result_pb;
				}
				if (reg == 2) {
					BYTE result_pc = ppi0_pc | 0xE0;
					if (rk_cc_pressed) result_pc &= ~(1 << 5);
					if (rk_us_pressed) result_pc &= ~(1 << 6);
					if (rk_rus_lat_pressed) result_pc &= ~(1 << 7);
					return result_pc;
				}
				return ppi0_ctrl;
			}
			// К580ВВ55 №2: Интерфейс ROM-диска (0xF500 - 0xF503)
			if (addr >= 0xF500 && addr <= 0xF503) {
				WORD reg = addr & 3;
				if (reg == 0) {
					WORD rom_address = (ppi1_pc << 8) | ppi1_pb;
					return rom_disk_storage[rom_address];
				}
				if (reg == 1) return ppi1_pb;
				if (reg == 2) return ppi1_pc;
				return ppi1_ctrl;
			}
			// Пользовательский порт №3 (0xF600 - 0xF603)
			if (addr >= 0xF600 && addr <= 0xF603) {
				WORD reg = addr & 3;
				if (reg == 0) return ppi2_pa;
				if (reg == 1) return ppi2_pb;
				if (reg == 2) return ppi2_pc;
				return ppi2_ctrl;
			}
			// Системный трехканальный таймер КР580ВИ53 (0xF730 - 0xF733)
			if (addr >= 0xF730 && addr <= 0xF733) {
				WORD reg = addr & 3;
				if (reg < 3) {
					PITChannel& ch = pit_channels[reg];
					BYTE val = 0;
					WORD src = ch.latched ? ch.latch : ch.count;
					if (ch.access_phase == 0) {
						val = src & 0xFF;
						ch.access_phase = 1;
					}
					else {
						val = (src >> 8) & 0xFF;
						ch.access_phase = 0;
						ch.latched = false;
					}
					return val;
				}
				return 0xFF;
			}
			return 0xFF;
		}

		// 3. Служебная область системных переменных Монитора (0xF000 - 0xF3FF)
		// Переменные и СТЕК Монитора ВСЕГДА должны читаться из Банка 0 в системных режимах!
		if ((current_bank == 0 || current_bank == 1) && addr >= 0xF000 && addr <= 0xF3FF) {
			return orion_ram[0][addr];
		}

		// 4. Чтение обычной памяти из выбранного текущего банка процессора (0 - 3)
		return orion_ram[current_bank][addr];
	}


	// =========================================================================
	// ИСПРАВЛЕННЫЙ АППАРАТНЫЙ ДИСПЕТЧЕР ЗАПИСИ В ПАМЯТЬ И ПОРТОВ (ОРИОН-128)
	// =========================================================================
	void Write(WORD addr, BYTE val) {

		// 1. ПЗУ Монитора защищено от записи аппаратно!
		if (addr >= 0xF800) {
			// Исключение: Регистры конфигурации ОРИОНА (Порты F800, F900, FA00) отображены поверх ПЗУ
			WORD port_type = addr & 0xFF00;
			if (port_type == 0xF800) { orion_sys_port1 = val; return; }
			if (port_type == 0xF900) { orion_page = val & 3; return; }
			if (port_type == 0xFA00) { orion_sys_port3 = val & 3; return; }
			return; // Запись в само ПЗУ игнорируется
		}

		int current_bank = orion_page & 3;

		// 2. Запись в периферийные БИС К580ВВ55 и КР580ВИ53 (0xF400 - 0xF7FF)
		if ((current_bank == 0 || current_bank == 1) && addr >= 0xF400 && addr <= 0xF7FF) {
			// Микросхема PPI 0 (Клавиатура и динамик)
			if (addr <= 0xF403) {
				WORD reg = addr & 3;
				if (reg == 0) ppi0_pa = val;
				else if (reg == 1) ppi0_pb = val;
				else if (reg == 2) {
					ppi0_pc = val;
					bool current_bit = (val & 0x10) != 0;
					if (current_bit != last_speaker_bit_state) {
						last_speaker_bit_state = current_bit;
						MessageBeep(MB_OK);
					}
				}
				else {
					ppi0_ctrl = val;
					if ((val & 0x80) == 0) {
						int bit_to_change = (val >> 1) & 7;
						if (val & 1) ppi0_pc |= (1 << bit_to_change);
						else ppi0_pc &= ~(1 << bit_to_change);
					}
				}
				return;
			}
			// Микросхема PPI 1 (Вторая плата / ROM-диск)
			if (addr >= 0xF500 && addr <= 0xF503) {
				WORD reg = addr & 3;
				if (reg == 0) ppi1_pa = val;
				else if (reg == 1) ppi1_pb = val;
				else if (reg == 2) ppi1_pc = val;
				else ppi1_ctrl = val;
				return;
			}
			// Порт №2 (0xF600 - 0xF603)
			if (addr >= 0xF600 && addr <= 0xF603) {
				WORD reg = addr & 3;
				if (reg == 0) { ppi2_pa = val; }
				else if (reg == 1) { ppi2_pb = val; }
				else if (reg == 2) { ppi2_pc = val; }
				else ppi2_ctrl = val;
				return;
			}
			// Системный таймер КР580ВИ53 (PIT)
			if (addr >= 0xF730 && addr <= 0xF733) {
				WORD reg = addr & 3;
				if (reg < 3) {
					PITChannel& ch = pit_channels[reg];
					if (ch.access_phase == 0) {
						ch.count = (ch.count & 0xFF00) | val;
						ch.access_phase = 1;
					}
					else {
						ch.count = (ch.count & 0x00FF) | (val << 8);
						ch.access_phase = 0;
					}
				}
				else {
					pit_control = val;
					int sel_ch = (val >> 6) & 3;
					if (sel_ch < 3) {
						pit_channels[sel_ch].mode = (val >> 1) & 7;
						pit_channels[sel_ch].bcd = val & 1;
						pit_channels[sel_ch].access_phase = 0;
						int rw_mode = (val >> 4) & 3;
						if (rw_mode == 0) {
							pit_channels[sel_ch].latch = pit_channels[sel_ch].count;
							pit_channels[sel_ch].latched = true;
						}
					}
				}
				return;
			}
			return;
		}

		// 3. Служебная зона ОЗУ Монитора (0xF000 - 0xF3FF) - всегда строго в Банк 0
		if ((current_bank == 0 || current_bank == 1) && addr >= 0xF000 && addr <= 0xF3FF) {
			orion_ram[0][addr] = val;
			return;
		}

		// 4. Обычная линейная запись в память текущего банка процессора (0, 1, 2, 3)
		orion_ram[current_bank][addr] = val;
	}

	int Step() {
		// Таблица базовых тактов для всех 256 инструкций Intel 8080
		static const int lut_cycles[256] = {
			4, 10, 7,  5,  5,  5,  7,  4,  4, 10, 7,  5,  5,  5,  7,  4,  // 00-0F
			4, 10, 7,  5,  5,  5,  7,  4,  4, 10, 7,  5,  5,  5,  7,  4,  // 10-1F
			4, 10, 16, 5,  5,  5,  7,  4,  4, 10, 16, 5,  5,  5,  7,  4,  // 20-2F
			4, 10, 13, 5,  10, 10, 10, 4,  4, 10, 13, 5,  5,  5,  7,  4,  // 30-3F
			5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7,  5,  // 40-4F
			5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7,  5,  // 50-5F
			5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7,  5,  // 60-6F
			7, 7,  7,  7,  7,  7,  7,  7,  5, 5,  5,  5,  5,  5,  7,  5,  // 70-7F
			4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7,  4,  // 80-8F
			4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7,  4,  // 90-9F
			4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7,  4,  // A0-AF
			4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7,  4,  // B0-BF
			5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7,  11, // C0-CF
			5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7,  11, // D0-DF
			5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7,  11, // E0-EF
			5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7,  11  // F0-FF
		};

		// 1. Проверяем аппаратное прерывание перед декодированием инструкции
		if (int_pending && EI) {
			int_pending = false;

			BYTE opcode = orion_ram[0][0x0038];
			// УМНАЯ ЗАГЛУШКА: Если на векторе пусто или там RET, просто гасим триггер прерываний
			if (opcode == 0x00 || opcode == 0xC9) {
				EI = false; // КРИТИЧЕСКИ ВАЖНО: прерывания должны запрещаться при обработке!
				halted = false; // Прерывание выводит процессор из HLT даже если там заглушка
				return 4;   // Возвращаем базовые 4 такта (как на холостой цикл), а не 0!
			}

			// Честное аппаратное прерывание по стандарту Intel 8080 (эквивалент команды RST 7)
			EI = false;
			halted = false;
			Write(--SP, PC >> 8);
			Write(--SP, PC & 0xFF);
			PC = 0x0038;
			return 11; // RST 7 занимает ровно 11 тактов
		}

		// 2. Если процессор в состоянии HLT
		if (halted) {
			int_pending = false; // Если прерывания запрещены, HLT вечный, но кадровый счетчик сбрасываем
			return 4; // Будем возвращать по 4 такта
		}

		BYTE op = Read(PC++);
		int cycles = lut_cycles[op];

		// Группа команд MOV r1, r2 (исключая MOV M, M, которая является HLT 0x76)
		if ((op & 0xC0) == 0x40 && op != 0x76) {
			SetReg((op >> 3) & 7, GetReg(op & 7));
			return cycles;
		}

		switch (op) {
				 // NOP инструкции
		case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28:
		case 0x30: case 0x38: return cycles;

				 // Останавливаем процессор до аппаратного прерывания
		case 0x76: halted = true; return cycles;

				 // MVI r, imm
		case 0x06: B = Read(PC++); return cycles;
		case 0x0E: C = Read(PC++); return cycles;
		case 0x16: D = Read(PC++); return cycles;
		case 0x1E: E = Read(PC++); return cycles;
		case 0x26: H = Read(PC++); return cycles;
		case 0x2E: L = Read(PC++); return cycles;
		case 0x36: Write(GetHL(), Read(PC++)); return cycles;
		case 0x3E: A = Read(PC++); return cycles;

				 // LXI rp, imm
		case 0x01: { WORD l = Read(PC++); WORD h = Read(PC++); SetBC((h << 8) | l); return cycles; }
		case 0x11: { WORD l = Read(PC++); WORD h = Read(PC++); SetDE((h << 8) | l); return cycles; }
		case 0x21: { WORD l = Read(PC++); WORD h = Read(PC++); SetHL((h << 8) | l); return cycles; }
		case 0x31: { WORD l = Read(PC++); WORD h = Read(PC++); SP = (h << 8) | l; return cycles; }

				 // ADD r
		case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
		case 0x86: case 0x87: { BYTE src = GetReg(op & 7); int r = A + src; SetFlagsAdd(A, src, r); A = (BYTE)r; return cycles; }

				 // ADC r
		case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D:
		case 0x8E: case 0x8F: { BYTE src = GetReg(op & 7); int r = A + src + (CY ? 1 : 0); SetFlagsAdd(A, src, r); A = (BYTE)r; return cycles; }

				 // SUB r
		case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
		case 0x96: case 0x97: { BYTE src = GetReg(op & 7); int r = A - src; SetFlagsSub(A, src, r); A = (BYTE)r; return cycles; }

				 // SBB r
		case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D:
		case 0x9E: case 0x9F: { BYTE src = GetReg(op & 7); int r = A - src - (CY ? 1 : 0); SetFlagsSub(A, src, r); A = (BYTE)r; return cycles; }

				 // ANA r
		case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:
		case 0xA6: case 0xA7: { A &= GetReg(op & 7); CY = false; AC = true; SetFlagsZSP(A); return cycles; }

				 // XRA r
		case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD:
		case 0xAE: case 0xAF: { A ^= GetReg(op & 7); CY = false; AC = false; SetFlagsZSP(A); return cycles; }

				 // ORA r
		case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5:
		case 0xB6: case 0xB7: { A |= GetReg(op & 7); CY = false; AC = false; SetFlagsZSP(A); return cycles; }

				 // CMP r
		case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD:
		case 0xBE: case 0xBF: { BYTE src = GetReg(op & 7); int r = A - src; SetFlagsSub(A, src, r); return cycles; }

				 // Операции с непосредственным значением (Иммедиат)
		case 0xC6: { BYTE imm = Read(PC++); int r = A + imm; SetFlagsAdd(A, imm, r); A = (BYTE)r; return cycles; }
		case 0xCE: { BYTE imm = Read(PC++); int r = A + imm + (CY ? 1 : 0); SetFlagsAdd(A, imm, r); A = (BYTE)r; return cycles; }
		case 0xD6: { BYTE imm = Read(PC++); int r = A - imm; SetFlagsSub(A, imm, r); A = (BYTE)r; return cycles; }
		case 0xDE: { BYTE imm = Read(PC++); int r = A - imm - (CY ? 1 : 0); SetFlagsSub(A, imm, r); A = (BYTE)r; return cycles; }
		case 0xE6: { A &= Read(PC++); CY = false; AC = true; SetFlagsZSP(A); return cycles; }
		case 0xEE: { A ^= Read(PC++); CY = false; AC = false; SetFlagsZSP(A); return cycles; }
		case 0xF6: { A |= Read(PC++); CY = false; AC = false; SetFlagsZSP(A); return cycles; }
		case 0xFE: { BYTE imm = Read(PC++); int r = A - imm; SetFlagsSub(A, imm, r); return cycles; }

				 // Безусловные JMP / CALL / RET
		case 0xC3: { WORD l = Read(PC++); WORD h = Read(PC++); PC = (h << 8) | l; return cycles; }
		case 0xCD: { WORD l = Read(PC++); WORD h = Read(PC++); Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; return cycles; }
		case 0xC9: { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; return cycles; }

				 // Возвраты по условию (При переходе тратится 11 тактов вместо 5 базовых)
		case 0xC0: if (!Z) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xC8: if (Z) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xD0: if (!CY) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xD8: if (CY) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xE0: if (!P) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xE8: if (P) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xF0: if (!S) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xF8: if (S) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;

				 // Переходы по условию (Всегда занимают 10 тактов в 8080)
		case 0xC2: { WORD l = Read(PC++); WORD h = Read(PC++); if (!Z) { PC = (h << 8) | l; } return cycles; }
		case 0xCA: { WORD l = Read(PC++); WORD h = Read(PC++); if (Z) { PC = (h << 8) | l; } return cycles; }
		case 0xD2: { WORD l = Read(PC++); WORD h = Read(PC++); if (!CY) { PC = (h << 8) | l; } return cycles; }
		case 0xDA: { WORD l = Read(PC++); WORD h = Read(PC++); if (CY) { PC = (h << 8) | l; } return cycles; }
		case 0xE2: { WORD l = Read(PC++); WORD h = Read(PC++); if (!P) { PC = (h << 8) | l; } return cycles; }
		case 0xEA: { WORD l = Read(PC++); WORD h = Read(PC++); if (P) { PC = (h << 8) | l; } return cycles; }
		case 0xF2: { WORD l = Read(PC++); WORD h = Read(PC++); if (!S) { PC = (h << 8) | l; } return cycles; }
		case 0xFA: { WORD l = Read(PC++); WORD h = Read(PC++); if (S) { PC = (h << 8) | l; } return cycles; }

				 // Вызовы по условию (При вызове тратится 17 тактов вместо 11 базовых)
		case 0xC4: { WORD l = Read(PC++); WORD h = Read(PC++); if (!Z) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xCC: { WORD l = Read(PC++); WORD h = Read(PC++); if (Z) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xD4: { WORD l = Read(PC++); WORD h = Read(PC++); if (!CY) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xDC: { WORD l = Read(PC++); WORD h = Read(PC++); if (CY) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xE4: { WORD l = Read(PC++); WORD h = Read(PC++); if (!P) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xEC: { WORD l = Read(PC++); WORD h = Read(PC++); if (P) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xF4: { WORD l = Read(PC++); WORD h = Read(PC++); if (!S) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xFC: { WORD l = Read(PC++); WORD h = Read(PC++); if (S) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }

				 // INX / DCX
		case 0x03: SetBC(GetBC() + 1); return cycles;
		case 0x13: SetDE(GetDE() + 1); return cycles;
		case 0x23: SetHL(GetHL() + 1); return cycles;
		case 0x33: SP++; return cycles;
		case 0x0B: SetBC(GetBC() - 1); return cycles;
		case 0x1B: SetDE(GetDE() - 1); return cycles;
		case 0x2B: SetHL(GetHL() - 1); return cycles;
		case 0x3B: SP--; return cycles;

				 // INR / DCR
		case 0x04: { BYTE old = B; B++; SetFlagsINR(old, B); return cycles; }
		case 0x05: { BYTE old = B; B--; SetFlagsDCR(old, B); return cycles; }
		case 0x0C: { BYTE old = C; C++; SetFlagsINR(old, C); return cycles; }
		case 0x0D: { BYTE old = C; C--; SetFlagsDCR(old, C); return cycles; }
		case 0x14: { BYTE old = D; D++; SetFlagsINR(old, D); return cycles; }
		case 0x15: { BYTE old = D; D--; SetFlagsDCR(old, D); return cycles; }
		case 0x1C: { BYTE old = E; E++; SetFlagsINR(old, E); return cycles; }
		case 0x1D: { BYTE old = E; E--; SetFlagsDCR(old, E); return cycles; }
		case 0x24: { BYTE old = H; H++; SetFlagsINR(old, H); return cycles; }
		case 0x25: { BYTE old = H; H--; SetFlagsDCR(old, H); return cycles; }
		case 0x2C: { BYTE old = L; L++; SetFlagsINR(old, L); return cycles; }
		case 0x2D: { BYTE old = L; L--; SetFlagsDCR(old, L); return cycles; }
		case 0x34: { BYTE old = Read(GetHL()); BYTE v = old + 1; Write(GetHL(), v); SetFlagsINR(old, v); return cycles; }
		case 0x35: { BYTE old = Read(GetHL()); BYTE v = old - 1; Write(GetHL(), v); SetFlagsDCR(old, v); return cycles; }
		case 0x3C: { BYTE old = A; A++; SetFlagsINR(old, A); return cycles; }
		case 0x3D: { BYTE old = A; A--; SetFlagsDCR(old, A); return cycles; }

				 // DAD rp
		case 0x09: { unsigned int r = (unsigned int)GetHL() + (unsigned int)GetBC(); CY = (r > 0xFFFF); SetHL((WORD)(r & 0xFFFF)); return cycles; }
		case 0x19: { unsigned int r = (unsigned int)GetHL() + (unsigned int)GetDE(); CY = (r > 0xFFFF); SetHL((WORD)(r & 0xFFFF)); return cycles; }
		case 0x29: { unsigned int r = (unsigned int)GetHL() + (unsigned int)GetHL(); CY = (r > 0xFFFF); SetHL((WORD)(r & 0xFFFF)); return cycles; }
		case 0x39: { unsigned int r = (unsigned int)GetHL() + (unsigned int)SP;    CY = (r > 0xFFFF); SetHL((WORD)(r & 0xFFFF)); return cycles; }

				 // PUSH / POP
		case 0xC5: Write(--SP, B); Write(--SP, C); return cycles;
		case 0xD5: Write(--SP, D); Write(--SP, E); return cycles;
		case 0xE5: Write(--SP, H); Write(--SP, L); return cycles;
		case 0xF5: { BYTE psw = (S << 7) | (Z << 6) | (0 << 5) | (AC << 4) | (0 << 3) | (P << 2) | (2) | (CY ? 1 : 0); Write(--SP, A); Write(--SP, psw); return cycles; }
		case 0xC1: C = Read(SP++); B = Read(SP++); return cycles;
		case 0xD1: E = Read(SP++); D = Read(SP++); return cycles;
		case 0xE1: L = Read(SP++); H = Read(SP++); return cycles;
		case 0xF1: { BYTE psw = Read(SP++); A = Read(SP++); S = (psw & 0x80) != 0; Z = (psw & 0x40) != 0; AC = (psw & 0x10) != 0; P = (psw & 0x04) != 0; CY = (psw & 0x01) != 0; return cycles; }

				 // Прямая адресация (STA, LDA, SHLD, LHLD)
		case 0x32: { WORD l = Read(PC++); WORD h = Read(PC++); Write((h << 8) | l, A); return cycles; }
		case 0x3A: { WORD l = Read(PC++); WORD h = Read(PC++); A = Read((h << 8) | l); return cycles; }
		case 0x22: { WORD l = Read(PC++); WORD h = Read(PC++); WORD a = (h << 8) | l; Write(a, L); Write(a + 1, H); return cycles; }
		case 0x2A: { WORD l = Read(PC++); WORD h = Read(PC++); WORD a = (h << 8) | l; L = Read(a); H = Read(a + 1); return cycles; }

				 // Косвенная адресация
		case 0x02: Write(GetBC(), A); return cycles;
		case 0x12: Write(GetDE(), A); return cycles;
		case 0x0A: A = Read(GetBC()); return cycles;
		case 0x1A: A = Read(GetDE()); return cycles;

				 // Циклические сдвиги аккумулятора
		case 0x07: { CY = (A & 0x80) != 0; A = (BYTE)(((A << 1) | (CY ? 1 : 0)) & 0xFF); return cycles; }
		case 0x0F: { CY = (A & 1) != 0; A = (BYTE)(((A >> 1) | (CY ? 0x80 : 0)) & 0xFF); return cycles; }
		case 0x17: { bool old_cy = CY; CY = (A & 0x80) != 0; A = (BYTE)(((A << 1) | (old_cy ? 1 : 0)) & 0xFF); return cycles; }
		case 0x1F: { bool old_cy = CY; CY = (A & 1) != 0; A = (BYTE)(((A >> 1) | (old_cy ? 0x80 : 0)) & 0xFF); return cycles; }

				 // RST 0 - RST 7
		case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
			Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (op & 0x38); return cycles;

				 // OUT port
		case 0xD3: {
			BYTE port = Read(PC++);
			WORD target_addr = (port << 8) | port; // Аппаратное дублирование байта порта
			Write(target_addr, A);                 // Перенаправляем запись в память (в область портов)
			return cycles;
		}

				 // IN port
		case 0xDB: {
			BYTE port = Read(PC++);
			WORD target_addr = (port << 8) | port; // Аппаратное дублирование байта порта
			A = Read(target_addr);                 // Читаем из памяти как из порта
			return cycles;
		}

				 // Десятичная коррекция аккумулятора (DAA)
		case 0x27: {
			BYTE corr = 0; bool new_cy = CY;
			if ((A & 0x0F) > 9 || AC) corr |= 0x06;
			if (A > 0x99 || CY) { corr |= 0x60; new_cy = true; }
			int res = A + corr; AC = ((A & 0x0F) + (corr & 0x0F)) > 0x0F;
			A = (BYTE)res; CY = new_cy; SetFlagsZSP(A); return cycles;
		}

				 // Специальные команды
		case 0x2F: A = (BYTE)(~A & 0xFF); return cycles;
		case 0x37: CY = true; return cycles;
		case 0x3F: CY = !CY; return cycles;
		case 0xEB: { BYTE t = D; D = H; H = t; t = E; E = L; L = t; return cycles; }
		case 0xE3: { BYTE l = Read(SP); BYTE h = Read(SP + 1); Write(SP, L); Write(SP + 1, H); L = l; H = h; return cycles; }
		case 0xF9: SP = GetHL(); return cycles;
		case 0xE9: PC = GetHL(); return cycles;
		case 0xFB: EI = true; rk_inte_speaker_state = true; return cycles;  // EI включает INTE
		case 0xF3: EI = false; rk_inte_speaker_state = false; return cycles; // DI выключает INTE
		}
		return cycles;
	}
};
#pragma pack(pop)

CPU8080 cpu;

// --- ЗАГРУЗКА И КОРРЕКТИРОВКА РОМОВ ---
bool LoadRawBinaryFile(const wchar_t* filepath, BYTE* target_dest, size_t max_bytes) {
	std::ifstream file(filepath, std::ios::binary);
	if (!file.is_open()) return false;
	file.read(reinterpret_cast<char*>(target_dest), max_bytes);
	file.close();
	return true;
}

// --- ИСПРАВЛЕННАЯ ЗАГРУЗКА ПЗУ ---
void LoadRoms() {
	cpu.Reset();
	// ИСПРАВЛЕНО: Загружаем Монитор точно по его физическому адресу 0xF800 в Банке 0
	if (!LoadRawBinaryFile(L"monitor.bin", &orion_ram[0][0xF800], 2048)) {
		MessageBoxW(NULL, L"Не удалось открыть файл системного монитора monitor.bin!", L"Предупреждение", MB_OK | MB_ICONWARNING);
	}

	// ДОБАВЛЕНО: Загрузка РОМ-диска 64 Кб для Порта Пользователя №1
	memset(rom_disk_storage, 0xFF, sizeof(rom_disk_storage)); // Инициализация чистой памяти
	if (!LoadRawBinaryFile(L"romdisk.bin", rom_disk_storage, 65536)) {
		MessageBoxW(NULL, L"Не удалось открыть файл РОМ-диска romdisk.bin!\nБудет смонтирован пустой диск.", L"Информация", MB_OK | MB_ICONINFORMATION);
	}

	for (int i = 0; i < 8; i++) key_matrix_state[i] = 0xFF;
}

void UpdateRegisterDisplay() {
	SendMessageW(hRegListBox, LB_RESETCONTENT, 0, 0);
	wchar_t buf[64];
	swprintf(buf, 64, L" PC: %04Xh  SP: %04Xh", cpu.PC, cpu.SP);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);

	// Показываем текущий банк процессора и физический банк видеовыхода
	swprintf(buf, 64, L" REG A: %02Xh  Банк ОЗУ: %d", cpu.A, orion_page);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);

	swprintf(buf, 64, L" REG B: %02Xh  REG C: %02Xh", cpu.B, cpu.C);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
	swprintf(buf, 64, L" REG D: %02Xh  REG E: %02Xh", cpu.D, cpu.E);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
	swprintf(buf, 64, L" REG H: %02Xh  REG L: %02Xh", cpu.H, cpu.L);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);

	swprintf(buf, 64, L" Флаги: S:%d Z:%d AC:%d P:%d CY:%d", cpu.S, cpu.Z, cpu.AC, cpu.P, cpu.CY);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);

	// Расширенная видео-отладка
	WORD screen_addr = 0xC000;
	switch (orion_sys_port3 & 3) {
	case 0: screen_addr = 0xC000; break;
	case 1: screen_addr = 0x8000; break;
	case 2: screen_addr = 0x4000; break;
	case 3: screen_addr = 0x0000; break;
	}

	swprintf(buf, 64, L" Экран: №%d (Адрес: %04Xh)", orion_sys_port3 + 1, screen_addr);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);

	// Битовая расшифровка Порта 1
	bool mode_c = (orion_sys_port1 & 0x04) != 0;
	bool mode_2 = (orion_sys_port1 & 0x02) != 0;
	swprintf(buf, 64, L" Порт1: %02Xh (%s, %s)",
		orion_sys_port1,
		mode_c ? L"Цветной" : L"Монохром",
		mode_2 ? L"4-цв (D1=1)" : L"16-цв (D1=0)");
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
}

// =========================================================================
// ПОЛНАЯ ИСПРАВЛЕННАЯ ФУНКЦИЯ ПОПИКСЕЛЬНОГО ВЫВОДА ЭКРАНА ОРИОН-128 (384х256)
// =========================================================================
void RenderScreen(HDC hdc, int xOffset, int yOffset) {
	// Эталонная 16-цветная палитра Ориона-128 (В формате BGR для StretchDIBits)
	static const DWORD palette16[] = {
		0x00000000, 0x000000AA, 0x0000AA00, 0x0000AAAA, 0x00AA0000, 0x00AA00AA,
		0x00AA5500, 0x00AAAAAA, 0x00555555, 0x005555FF, 0x0055FF55, 0x0055FFFF,
		0x00FF5555, 0x00FF55FF, 0x00FFFF55, 0x00FFFFFF
	};

	// Определение стартового адреса текущего экрана на основе Системного порта №3
	WORD screen_base = 0xC000;
	switch (orion_sys_port3 & 3) {
	case 0: screen_base = 0xC000; break;
	case 1: screen_base = 0x8000; break;
	case 2: screen_base = 0x4000; break;
	case 3: screen_base = 0x0000; break;
	}

	// Дешифрация режима цветности строго по спецификации вашего проекта
	bool color_mode = (orion_sys_port1 & 0x04) != 0;    // Бит D2: 1 - Цвет, 0 - Монохром

	// СТРОГО ПО СПЕЦИФИКАЦИИ: 
	// Если Бит D1 == 0 (как в Порт1 = 24h) -> это 4-цветный игровой режим.
	// Если Бит D1 == 1 (как в Порт1 = 07h) -> это 16-цветный режим ОРДОС.
	bool mode_4_color = (orion_sys_port1 & 0x02) == 0;

	BYTE palette_idx = orion_sys_port1 & 0x01;          // Бит D0: Палитра

	// ДИНАМИЧЕСКИЙ ВЫБОР СТРАНИЦ ВИДЕОПАМЯТИ ДЛЯ РЕНДЕРИНГА (СТРОГО С ВАШЕЙ СТРАНИЦЫ 14)
	int bank_mask = 0;
	int bank_color = 1;
	// Экранные страницы переключаются битом D3 порта 0xF800 (orion_sys_port1)
	if ((orion_sys_port1 & 0x08) != 0) {
		bank_mask = 2;  // Графическая маска перемещается в Банк 2
		bank_color = 3; // Атрибуты цвета перемещаются в Банк 3
	}

	// 1. МОНОХРОМНЫЙ РЕЖИМ РАБОТЫ ЭКРАНА (Бит D2 = 0)
	if (!color_mode) {
		DWORD color_on = (palette_idx == 0) ? 0x0000FF00 : 0x0000FFFF; // Зеленый или Желтый текст
		DWORD color_off = 0x00000000; // Черный фон
		for (int x_byte = 0; x_byte < 48; x_byte++) {
			for (int y = 0; y < 256; y++) {
				WORD addr = screen_base + (x_byte * 256) + y;
				BYTE pixels = orion_ram[bank_mask][addr];
				int pixel_y = (256 - 1) - y;
				for (int bit = 0; bit < 8; bit++) {
					int dot = 7 - bit;
					int pixel_x = (x_byte * 8) + bit;
					bool dot_state = (pixels & (1 << dot)) != 0;
					pixel_buffer[pixel_y * 384 + pixel_x] = dot_state ? color_on : color_off;
				}
			}
		}
	}
	// 2. ЦВЕТНЫЕ РЕЖИМЫ РАБОТЫ (Бит D2 = 1)
	else {
		for (int x_byte = 0; x_byte < 48; x_byte++) {
			for (int y = 0; y < 256; y++) {
				WORD addr = screen_base + (x_byte * 256) + y;

				BYTE layer0 = orion_ram[bank_mask][addr];  // Слой маски (Банк 0 или 2)
				BYTE layer1 = orion_ram[bank_color][addr]; // Слой цвета/атрибутов (Банк 1 или 3)
				int pixel_y = (256 - 1) - y;

				if (mode_4_color) {
					// ИГРОВОЙ 4-ЦВЕТНЫЙ РЕЖИМ (Побитная склейка двух слоев, Порт1 = 24h)
					for (int bit = 0; bit < 8; bit++) {
						int dot = 7 - bit;
						int bit0 = (layer0 & (1 << dot)) ? 1 : 0;
						int bit1 = (layer1 & (1 << dot)) ? 2 : 0;
						int color_code = bit0 | bit1;

						// Маппинг 4 цветов на палитру palette16
						BYTE final_idx = 0;
						if (palette_idx == 0) {
							static const BYTE pal1[] = { 0, 4, 2, 1 }; // черный, красный, зеленый, синий
							final_idx = pal1[color_code];
						}
						else {
							static const BYTE pal2[] = { 0, 5, 3, 15 };
							final_idx = pal2[color_code];
						}

						DWORD pixel_color = palette16[final_idx & 0x0F];
						int pixel_x = x_byte * 8 + bit;
						if (pixel_x >= 0 && pixel_x < 384 && pixel_y >= 0 && pixel_y < 256) {
							pixel_buffer[pixel_y * 384 + pixel_x] = pixel_color;
						}
					}
				}
				else {
					// СИСТЕМНЫЙ 16-ЦВЕТНЫЙ РЕЖИМ ОРДОС (Групповое кодирование тетрадами, Порт1 = 07h)
					BYTE bg_idx = (layer1 >> 4) & 0x0F; // Старшая тетрада — цвет фона
					BYTE fg_idx = layer1 & 0x0F;        // Младшая тетрада — цвет текста

					for (int bit = 0; bit < 8; bit++) {
						int dot = 7 - bit;
						bool bit0 = (layer0 & (1 << dot)) != 0;
						BYTE final_idx = bit0 ? fg_idx : bg_idx;

						DWORD pixel_color = palette16[final_idx];
						int pixel_x = x_byte * 8 + bit;
						if (pixel_x >= 0 && pixel_x < 384 && pixel_y >= 0 && pixel_y < 256) {
							pixel_buffer[pixel_y * 384 + pixel_x] = pixel_color;
						}
					}
				}
			}
		}
	}

	// Инициализация структуры метаданных кадра для Windows API
	BITMAPINFO bmi = { 0 };
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = 384;
	bmi.bmiHeader.biHeight = 256;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	// Вывод кадра в окно Windows с масштабированием x2
	StretchDIBits(hdc, xOffset, yOffset, 384 * 2, 256 * 2, 0, 0, 384, 256, pixel_buffer, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

#ifndef BS_NOFOCUS
#define BS_NOFOCUS 0x00008000L
#endif

// =========================================================================
// ФУНКЦИЯ ДИНАМИЧЕСКОЙ ГЕНЕРАЦИИ ИНТЕРФЕЙСА КЛАВИАТУРЫ (РАДИО-86РК)
// =========================================================================
void CreateKeyboardUI(HWND hwndParent, HINSTANCE hInst) {
	const int BTN_W = 40; const int BTN_H = 40; const int START_X = 20;
	const int START_Y = 560; const int GAP = 4;

	static const wchar_t* main_labels[] = {
		L";\n+", L"1\n!", L"2\n\"", L"3\n#", L"4\n$", L"5\n%", L"6\n&", L"7\n'",
		L"8\n(", L"9\n)", L"0", L"-\n=", L"ТАБ", L"ПС", L"",
		L"Й\nJ", L"Ц\nC", L"У\nU", L"К\nK", L"Е\nE", L"Н\nN", L"Г\nG", L"Ш\n[",
		L"Щ\n]", L"З\nZ", L"Х\nH", L":\n*", L"ВК", L"", L"",
		L"УС", L"Ф\nF", L"Ы\nY", L"В\nW", L"А\nA", L"П\nP", L"Р\nR", L"О\nO",
		L"Л\nL", L"Д\nD", L"Ж\n\\V", L"Э\n\\", L".\n>", L"ЗБ", L"",
		L"СС", L"Я\nQ", L"Ч\n^", L"С\nS", L"М\nM", L"И\nI", L"Т\nT", L"Ь\nX",
		L"Б\nB", L"Ю\n@", L",\n<", L"/\n?", L"РУС\nЛАТ", L"[ _ ]", L"[ _ ]"
	};

	static const wchar_t* side_labels[] = { L" ", L"F1", L"СТР", L"←", L"↑", L"→", L"F2", L"F3", L"F4", L"↓", L"AP2", L"" };

	for (int idx = 0; idx < 60; idx++) {
		int r = idx / 15; int c = idx % 15; int current_id = 1000 + idx;
		if (idx == 14) continue; // Пропуск отсутствующей кнопки
		if ((r == 1 && c >= 13) || (r == 2 && c == 14)) continue;

		int x = START_X + c * (BTN_W + GAP); int y = START_Y + r * (BTN_H + GAP);

		if (r == 3 && c == 13) { // Пробел
			CreateWindowExW(0, L"BUTTON", main_labels[idx],
				WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_MULTILINE | BS_FLAT | BS_NOFOCUS,
				x, y, (BTN_W * 2) + GAP, BTN_H, hwndParent, (HMENU)(INT_PTR)current_id, hInst, NULL);
			continue;
		}
		if (r == 3 && c == 14) continue;

		CreateWindowExW(0, L"BUTTON", main_labels[idx],
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_MULTILINE | BS_FLAT | BS_NOFOCUS,
			x, y, BTN_W, BTN_H, hwndParent, (HMENU)(INT_PTR)current_id, hInst, NULL);
	}

	int SIDE_START_X = START_X + 15 * (BTN_W + GAP) + 15;
	for (int side_idx = 0; side_idx < 12; side_idx++) {
		int r = side_idx / 3; int c = side_idx % 3; int current_id = 1060 + side_idx;
		if (r == 3 && c == 2) continue;

		int x = SIDE_START_X + c * (BTN_W + GAP); int y = START_Y + r * (BTN_H + GAP);

		CreateWindowExW(0, L"BUTTON", side_labels[side_idx],
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_MULTILINE | BS_FLAT | BS_NOFOCUS,
			x, y, BTN_W, BTN_H, hwndParent, (HMENU)(INT_PTR)current_id, hInst, NULL);
	}
}

static const int rk_hardware_map[] = {
	3 * 8 + 3, 2 * 8 + 1, 2 * 8 + 2, 2 * 8 + 3, 2 * 8 + 4, 2 * 8 + 5, 2 * 8 + 6, 2 * 8 + 7, 3 * 8 + 0, 3 * 8 + 1, 2 * 8 + 0, 3 * 8 + 5, 1 * 8 + 0, 1 * 8 + 1, 0,
	5 * 8 + 2, 4 * 8 + 3, 6 * 8 + 5, 5 * 8 + 3, 4 * 8 + 5, 5 * 8 + 6, 4 * 8 + 7, 7 * 8 + 3, 7 * 8 + 5, 7 * 8 + 2, 5 * 8 + 0, 3 * 8 + 2, 1 * 8 + 2, 0, 0,
	0, 4 * 8 + 6, 7 * 8 + 1, 6 * 8 + 7, 4 * 8 + 1, 6 * 8 + 0, 6 * 8 + 2, 5 * 8 + 7, 5 * 8 + 4, 4 * 8 + 4, 6 * 8 + 6, 7 * 8 + 4, 3 * 8 + 6, 1 * 8 + 3, 0,
	0, 6 * 8 + 1, 7 * 8 + 6, 6 * 8 + 3, 5 * 8 + 5, 5 * 8 + 1, 6 * 8 + 4, 7 * 8 + 0, 4 * 8 + 2, 4 * 8 + 0, 3 * 8 + 4, 3 * 8 + 7, 0, 7 * 8 + 7, 7 * 8 + 7,
	0 * 8 + 0, 0 * 8 + 3, 0 * 8 + 1, 1 * 8 + 4, 1 * 8 + 5, 1 * 8 + 6, 0 * 8 + 4, 0 * 8 + 5, 0 * 8 + 6, 1 * 8 + 7, 0 * 8 + 2, 0
};

bool MapVirtualKeyToRK(UINT vkCode, int& out_row, int& out_col) {
	int target_idx = -1;
	switch (vkCode) {
	case 0xC0: target_idx = 0; break;
	case '1': target_idx = 1; break; case '2': target_idx = 2; break; case '3': target_idx = 3; break;
	case '4': target_idx = 4; break; case '5': target_idx = 5; break; case '6': target_idx = 6; break;
	case '7': target_idx = 7; break; case '8': target_idx = 8; break; case '9': target_idx = 9; break;
	case '0': target_idx = 10; break; case 0xBD: target_idx = 11; break;
	case 'Q': target_idx = 46; break; case 'W': target_idx = 33; break; case 'E': target_idx = 19; break;
	case 'R': target_idx = 36; break; case 'T': target_idx = 51; break; case 'Y': target_idx = 32; break;
	case 'U': target_idx = 17; break; case 'I': target_idx = 50; break; case 'O': target_idx = 37; break;
	case 'P': target_idx = 35; break; case 'A': target_idx = 34; break; case 'S': target_idx = 48; break;
	case 'D': target_idx = 39; break; case 'F': target_idx = 31; break; case 'G': target_idx = 21; break;
	case 'H': target_idx = 25; break; case 'J': target_idx = 15; break; case 'K': target_idx = 18; break;
	case 'L': target_idx = 38; break; case 'Z': target_idx = 24; break; case 'X': target_idx = 52; break;
	case 'C': target_idx = 16; break; case 'V': target_idx = 40; break; case 'B': target_idx = 53; break;
	case 'N': target_idx = 20; break; case 'M': target_idx = 49; break;
	case 0xDB: target_idx = 22; break; case 0xDD: target_idx = 23; break; case 0xBA: target_idx = 26; break;
	case 0xDE: target_idx = 41; break; case 0xBC: target_idx = 55; break; case 0xBE: target_idx = 42; break;
	case 0xBF: target_idx = 56; break;
	case VK_RETURN: target_idx = 27; break; // ВК
	case VK_BACK:   target_idx = 43; break; // ЗБ
	case VK_SPACE:  target_idx = 58; break; // Пробел
	case VK_F1:     target_idx = 61; break; // Кнопка F1
	case VK_F2:     target_idx = 66; break; // Кнопка F2
	case VK_F3:     target_idx = 67; break; // Кнопка F3
	case VK_F4:     target_idx = 68; break; // Кнопка F4
	case VK_LEFT:   target_idx = 63; break; // ←
	case VK_UP:     target_idx = 64; break; // ↑
	case VK_RIGHT:  target_idx = 65; break; // →
	case VK_DOWN:   target_idx = 69; break; // ↓
	case VK_HOME:   target_idx = 60; break; // СТР (Домой)
	}
	if (target_idx != -1) {
		int hw_code = rk_hardware_map[target_idx];
		out_row = hw_code / 8; out_col = hw_code % 8;
		return true;
	}
	return false;
}

// Загрузка бинарных дампов Ориона (.ORD / .BIN)
void LoadOrionFile(HWND hwnd) {
	OPENFILENAMEW ofn;
	wchar_t szFile[260] = { 0 };
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hwnd;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile);
	ofn.lpstrFilter = L"Файлы Орион-128 (.ord;.bin)\0*.ord;.bin\0Все файлы (*.*)\0*.*\0";
	ofn.nFilterIndex = 1;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = NULL;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

	if (GetOpenFileNameW(&ofn) == TRUE) {
		std::ifstream file(ofn.lpstrFile, std::ios::binary);
		if (!file.is_open()) {
			MessageBoxW(hwnd, L"Не удалось открыть файл!", L"Ошибка", MB_OK | MB_ICONERROR);
			return;
		}

		// Вычисляем точный физический размер файла на диске
		file.seekg(0, std::ios::end);
		size_t file_size = file.tellg();
		file.seekg(0, std::ios::beg);

		WORD load_address = 0x0000;
		WORD file_length = 0;
		bool has_header = false;

		// Проверяем наличие 16-байтного заголовка ОС ORDOS
		if (file_size >= 16) {
			BYTE header[16];
			file.read(reinterpret_cast<char*>(header), 16);

			WORD test_load = header[8] | (header[9] << 8);
			WORD test_len = header[10] | (header[11] << 8);

			// Критерий корректности заголовка: длина кода + 16 байт заголовка == размер файла
			if (test_len + 16 == file_size) {
				load_address = test_load;
				file_length = test_len;
				has_header = true;
			}
		}

		// Если файл без заголовка (чистый бинарник RAW)
		if (!has_header) {
			file.seekg(0, std::ios::beg); // Сбрасываем указатель на начало файла
			load_address = 0x0000;        // Шьем строго с нулевого адреса
			file_length = (file_size > 48 * 1024) ? 48 * 1024 : (WORD)file_size;
		}

		// Читаем исполняемый код программы строго в БАНК 0 по вычисленному смещению
		file.read(reinterpret_cast<char*>(&orion_ram[0][load_address]), file_length);
		file.close();

		// =================================================================
		// АППАРАТНЫЙ СБРОС И НАСТРОЙКА ОКРУЖЕНИЯ ДЛЯ СТАРТА ИГРЫ:
		// =================================================================
		orion_page = 0;        // Принудительно включаем Банк ОЗУ №0
		orion_sys_port1 = 0x7; // Форсируем цветной 4-цветный игровой режим (Бит D5=1, D2=1)
		orion_sys_port3 = 0;   // Видеовыход строго на стандартный адрес 0xC000

		cpu.EI = false;        // Выключаем прерывания, чтобы тики 50Гц не ломали вектор 0x0038
		cpu.PC = load_address; // Устанавливаем точку старта процессора (0x0000)
		cpu.SP = 0xEFFF;       // Выделяем стек в безопасной верхней зоне ОЗУ

		// Системная интеграция: заполняем ОЗУ под нужды Монитора/ORDOS базовыми заглушками
		// Чтобы при вызове системных подпрограмм процессор мгновенно возвращался в игру
		orion_ram[0][0xBE03] = 0xC9; // Заглушка под ORDOS (команда RET)
		for (int o_addr = 0xF000; o_addr < 0xF800; o_addr++) { // ИСПРАВЛЕНО: Добавлен тип int
			orion_ram[0][o_addr] = 0x00; // Очищаем буфер системных переменных (NOP)
		}

		// Сброс физической матрицы клавиатуры в исходное ненажатое состояние
		for (int i = 0; i < 8; i++) key_matrix_state[i] = 0xFF;

		// Синхронизируем интерфейс отладки и перерисовываем окно эмулятора
		UpdateRegisterDisplay();
		InvalidateRect(hwnd, NULL, FALSE);

		wchar_t msg_buf[256];
		swprintf(msg_buf, 256, L"Файл успешно загружен!\nТип: %s\nАдрес старта: %04Xh\nРазмер кода: %d байт",
			has_header ? L"ORDOS (.ORD с заголовком)" : L"Чистый дамп (.BIN RAW)",
			load_address, file_length);
		MessageBoxW(hwnd, msg_buf, L"Успех", MB_OK | MB_ICONINFORMATION);
	}
}

int static_virtual_key_duration = 0; // Наш счетчик автоотжатия кнопок мыши
const int HARDWARE_MAP_SIZE = sizeof(rk_hardware_map) / sizeof(rk_hardware_map[0]);

// =========================================================================
// ОКОННАЯ ПРОЦЕДУРА WINDOWS ДЛЯ СИМУЛЯТОРА
// =========================================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	static HINSTANCE hInst; switch (msg) {
	case WM_CREATE: {
		LPCREATESTRUCTW pcs = (LPCREATESTRUCTW)lp;
		hInst = pcs->hInstance;
		HMENU hSysMenu = GetSystemMenu(hwnd, FALSE);
		if (hSysMenu) {
			AppendMenuW(hSysMenu, MF_SEPARATOR, 0, NULL);
			AppendMenuW(hSysMenu, MF_STRING, IDM_FILE_OPEN, L"Открыть файл Орион...");
		}
		CreateKeyboardUI(hwnd, hInst);
		int RIGHT_PANEL_X = 840;
		CreateWindowExW(0, L"BUTTON", L"СБРОС (RESET)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, RIGHT_PANEL_X, 20, 210, 40, hwnd, (HMENU)2000, hInst, NULL);
		hRegListBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL, RIGHT_PANEL_X, 75, 210, 245, hwnd, (HMENU)2001, NULL, NULL);
		cpu.Reset();
		UpdateRegisterDisplay();
		return 0;
	}
	case WM_SYSCOMMAND: {
		if ((wp & 0xFFF0) == IDM_FILE_OPEN) {
			LoadOrionFile(hwnd);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
	case WM_COMMAND: {
		int btn_id = LOWORD(wp);

		// 1. Обработка кнопки СБРОС (RESET)
		if (btn_id == 2000) {
			cpu.Reset();
			for (int i = 0; i < 8; i++) key_matrix_state[i] = 0xFF;
			UpdateRegisterDisplay();
			InvalidateRect(hwnd, NULL, FALSE);
			return 0;
		}

		// 2. Обработка виртуальных клавиш клавиатуры (ID от 1000 до 1071)
		int btn_idx = btn_id - 1000;
		if (btn_idx >= 0 && btn_idx < HARDWARE_MAP_SIZE) {
			UINT target_vk = 0;

			// Находим соответствующий Virtual Key (VK) для этой кнопки
			for (int vk_test = 0; vk_test < 256; vk_test++) {
				int r = -1, c = -1;
				if (MapVirtualKeyToRK(vk_test, r, c)) {
					int hw_code = rk_hardware_map[btn_idx];
					if ((hw_code / 8 == r) && (hw_code % 8 == c)) {
						target_vk = vk_test;
						break;
					}
				}
			}

			// Ручной маппинг системных модификаторов
			if (btn_idx == 57) target_vk = VK_CAPITAL;      // РУС/ЛАТ
			else if (btn_idx == 45) target_vk = VK_SHIFT;   // СС
			else if (btn_idx == 30) target_vk = VK_CONTROL; // УС

			// Если кнопка валидна — зажимаем её в матрице НАПРЯМУЮ
			if (target_vk != 0) {
				// Выделяем 4 кадра удержания (~80 мс), чтобы процессор точно успел считать её
				static_virtual_key_duration = 4;

				if (target_vk == VK_CAPITAL) { rk_rus_lat_pressed = true; }
				else if (target_vk == VK_SHIFT) { rk_cc_pressed = true; }
				else if (target_vk == VK_CONTROL) { rk_us_pressed = true; }
				else {
					int r = -1, c = -1;
					if (MapVirtualKeyToRK(target_vk, r, c)) {
						if (r >= 0 && r < 8) {
							key_matrix_state[r] &= ~(1 << c); // Замыкаем контакт напрямую
						}
					}
				}
			}

			// Возвращаем фокус на главное окно, чтобы продолжала работать физическая клавиатура
			SetFocus(hwnd);
			return 0;
		}
		break;
	}
	case WM_PAINT: {
		PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
		RenderScreen(hdc, 20, 20); // Рендеринг экрана Ориона
		EndPaint(hwnd, &ps);
		break;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default: return DefWindowProcW(hwnd, msg, wp, lp);
	}
	return 0;
}

const double PIT_CLOCK_HZ = 2500000.0;
const double TICKS_PER_SAMPLE = PIT_CLOCK_HZ / (double)AUDIO_SAMPLE_RATE;

// Генерирование звука через КР580ВИ53 на основе накопленной фазы частоты
void FillAudioBuffer(short* buffer, int samplesCount) {
	static double ticks_accumulator = 0.0;

	for (int i = 0; i < samplesCount; i++) {
		ticks_accumulator += TICKS_PER_SAMPLE;
		int ticks_to_consume = (int)ticks_accumulator;
		ticks_accumulator -= ticks_to_consume;

		if (ticks_to_consume <= 0) {
			buffer[i] = (i > 0) ? buffer[i - 1] : 0;
			continue;
		}

		int active_channels = 0;
		int mixed_signal = 0;

		// 1. Эмуляция каналов КР580ВИ53 (остается оригинальной)
		for (int ch = 0; ch < 3; ch++) {
			CPU8080::PITChannel& channel = cpu.pit_channels[ch];
			WORD divisor = channel.count;
			BYTE mode = channel.mode & 7;

			if (divisor > 3 && (mode == 2 || mode == 3)) {
				if (channel.phase <= 0.0 || channel.phase > (double)divisor) {
					channel.phase = (double)(divisor / 2);
				}
				channel.phase -= (double)ticks_to_consume;
				if (channel.phase <= 0.0) {
					channel.phase += (double)(divisor / 2);
					channel.latched = !channel.latched;
				}
				mixed_signal += channel.latched ? 1 : -1;
				active_channels++;
			}
		}

		// 2. Извлечение СИНХРОННОГО сигнала INTE из кольцевого буфера
		int inte_signal = 0;
		if (inte_ring_read_ptr != inte_ring_write_ptr) {
			inte_signal = inte_ring_buffer[inte_ring_read_ptr];
			inte_ring_read_ptr = (inte_ring_read_ptr + 1) % INTE_RING_BUF_SIZE;
		}
		else {
			// Если буфер пуст, берем последнее состояние (стабилизация)
			inte_signal = rk_inte_speaker_state ? 4000 : -4000;
		}

		// 3. Микширование сигналов
		if (active_channels > 0) {
			// Нормализуем меандр ВИ53 к значениям громкости и прибавляем сигнал INTE
			int vi53_signal = mixed_signal * 5000 / active_channels;
			buffer[i] = (short)((vi53_signal + inte_signal) / 2);
		}
		else {
			// Работает только чистый бипер INTE
			buffer[i] = (short)inte_signal;
		}
	}
}

void CALLBACK WaveOutCallback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
	if (uMsg == WOM_DONE) {
		WAVEHDR* pHdr = (WAVEHDR*)dwParam1;
		FillAudioBuffer((short*)pHdr->lpData, AUDIO_BUF_SIZE);
		waveOutWrite(hwo, pHdr, sizeof(WAVEHDR));
	}
}

static double audio_ticks_accumulator = 0.0;

// =========================================================================
// ИСПРАВЛЕННАЯ СТАБИЛЬНАЯ ФУНКЦИЯ WINMAIN С КАДРОВЫМИ ПРЕРЫВАНИЯМИ 50 ГЦ
// =========================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
	LoadRoms();
	WNDCLASSW wc = { 0 };
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = L"Orion128_Emulator_Core";
	RegisterClassW(&wc);

	HWND hwnd = CreateWindowExW(0, L"Orion128_Emulator_Core", L"Орион-128 Симулятор компьютера",
		WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 1080, 800, NULL, NULL, hInst, NULL);
	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	// --- Инициализация звуковой подсистемы waveOut ---
	WAVEFORMATEX wfx = { 0 };
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = 1;
	wfx.nSamplesPerSec = AUDIO_SAMPLE_RATE;
	wfx.wBitsPerSample = 16;
	wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

	if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)WaveOutCallback, 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
		for (int i = 0; i < 2; i++) {
			audioBuffers[i] = new short[AUDIO_BUF_SIZE];
			memset(audioBuffers[i], 0, AUDIO_BUF_SIZE * sizeof(short));
			waveHeader[i].lpData = (LPSTR)audioBuffers[i];
			waveHeader[i].dwBufferLength = AUDIO_BUF_SIZE * sizeof(short);
			waveHeader[i].dwFlags = 0;
			waveOutPrepareHeader(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
			waveOutWrite(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
		}
	}

	MSG msg;
	LARGE_INTEGER frequency;
	LARGE_INTEGER last_hardware_time;
	double internal_cycles_debt = 0.0;

	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&last_hardware_time);

	const double SYSTEM_CLOCK_HZ = 2500000.0; // Частота ядра Орион-128 (2.5 МГц)
	double time_since_last_interrupt = 0.0;

	while (emulator_running) {
		// Ожидание системных событий во избежание 100% загрузки процессора ПК
		MsgWaitForMultipleObjectsEx(0, NULL, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				emulator_running = false;
				break;
			}

			// --- НАЖАТИЕ КЛАВИШ (WM_KEYDOWN) ---
			if (msg.message == WM_KEYDOWN) {
				if (!(msg.lParam & (1 << 30))) { // Защита от автоповтора Windows
					UINT vk = (UINT)msg.wParam;
					if (vk == VK_CAPITAL) { rk_rus_lat_pressed = true; }
					else if (vk == VK_SHIFT) { rk_cc_pressed = true; }
					else if (vk == VK_CONTROL) { rk_us_pressed = true; }
					else {
						int target_row = -1, target_col = -1;
						if (MapVirtualKeyToRK(vk, target_row, target_col)) {
							key_matrix_state[target_row] &= ~(1 << target_col);
						}
					}
				}
			}

			// --- ОТПУСКАНИЕ КЛАВИШ (WM_KEYUP) ---
			else if (msg.message == WM_KEYUP) {
				UINT vk = (UINT)msg.wParam;
				if (vk == VK_CAPITAL) { rk_rus_lat_pressed = false; }
				else if (vk == VK_SHIFT) { rk_cc_pressed = false; }
				else if (vk == VK_CONTROL) { rk_us_pressed = false; }
				else {
					int target_row = -1, target_col = -1;
					if (MapVirtualKeyToRK(vk, target_row, target_col)) {
						key_matrix_state[target_row] |= (1 << target_col); // Размыкаем контакт
					}
				}
			}

			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		if (!emulator_running) break;

		// --- ДИСПЕТЧЕР И ТАКТОВАЯ СИНХРОНИЗАЦИЯ ЯДРА ---
		LARGE_INTEGER current_hardware_time;
		QueryPerformanceCounter(&current_hardware_time);
		double elapsed_seconds = (double)(current_hardware_time.QuadPart - last_hardware_time.QuadPart) / frequency.QuadPart;

		if (elapsed_seconds > 0.1) elapsed_seconds = 0.1; // Защита от лагов Windows

		if (elapsed_seconds > 0.0) {
			last_hardware_time = current_hardware_time;
			internal_cycles_debt += elapsed_seconds * SYSTEM_CLOCK_HZ; // Накапливаем такты ПК

			int cycles_to_execute = (int)internal_cycles_debt;
			if (cycles_to_execute > 0) {
				while (cycles_to_execute > 0) {
					int elapsed_ticks = cpu.Step();

					cycles_to_execute -= elapsed_ticks;
					internal_cycles_debt -= elapsed_ticks;

					// Накапливаем процессорные такты для генерации звука
					audio_ticks_accumulator += elapsed_ticks;
					while (audio_ticks_accumulator >= TICKS_PER_SAMPLE) {
						audio_ticks_accumulator -= TICKS_PER_SAMPLE;

						// Генерируем сэмпл на основе ТЕКУЩЕГО состояния флага прямо сейчас
						short sample = rk_inte_speaker_state ? 4000 : -4000;

						// Записываем в кольцевой буфер
						int next_write = (inte_ring_write_ptr + 1) % INTE_RING_BUF_SIZE;
						// Защита от переполнения буфера
						if (next_write != inte_ring_read_ptr) {
							inte_ring_buffer[inte_ring_write_ptr] = sample;
							inte_ring_write_ptr = next_write;
						}
					}

					// Отсчитываем такты до кадрового прерывания 50 Гц
					cpu.cycles_until_interrupt -= elapsed_ticks;
					if (cpu.cycles_until_interrupt <= 0) {
						cpu.int_pending = true; // Выставляем запрос на прерывание для процессора
						cpu.cycles_until_interrupt += 50000; // На запуск следующего кадра

						// Автоматическое плавное отжатие экранных кнопок GUI (раз в 20 мс)
						if (static_virtual_key_duration > 0) {
							static_virtual_key_duration--;
							if (static_virtual_key_duration == 0) {
								rk_rus_lat_pressed = false;
								rk_cc_pressed = false;
								rk_us_pressed = false;
								for (int i = 0; i < 8; i++) key_matrix_state[i] = 0xFF;
							}
						}

						// Обновляем графику и окна отладки строго 1 раз по завершении кадра
						UpdateRegisterDisplay();
						InvalidateRect(hwnd, NULL, FALSE);
					}
				}
			}
		}

		if (internal_cycles_debt <= 0.0) {
			Sleep(1); // Освобождаем CPU ПК
		}
	}

	// --- Безопасная очистка аудиодескрипторов перед выходом ---
	if (hWaveOut) {
		for (int i = 0; i < 2; i++) {
			waveOutUnprepareHeader(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
			delete[] audioBuffers[i];
		}
		waveOutClose(hWaveOut);
	}
	return (int)msg.wParam;
}

